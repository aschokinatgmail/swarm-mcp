#include "mcp_collab/protocol.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

McpProtocol::McpProtocol(const ServerInfo& info, const ServerCapabilities& caps)
    : info_(info), caps_(caps) {}

McpRequest McpProtocol::parse_request(const json& req) const {
    McpRequest mcp_req;
    mcp_req.jsonrpc = req.value("jsonrpc", "2.0");
    if (req.contains("id") && !req["id"].is_null()) mcp_req.id = req["id"];
    mcp_req.method = req.value("method", "");
    mcp_req.params = req.value("params", json::object());

    // Extract auth context injected by transport
    if (mcp_req.params.contains("_auth")) {
        auto& auth = mcp_req.params["_auth"];
        mcp_req.auth_agent_id = auth.value("agent_id", "");
        mcp_req.auth_role = role_from_str(auth.value("role", "observer"));
        mcp_req.auth_swarm_id = auth.value("swarm_id", "");
    }

    return mcp_req;
}

bool McpProtocol::check_auth(const McpRequest& req, Permission required) const {
    return has_permission(req.auth_role, required);
}

json McpProtocol::make_error_response(const McpRequest& req, int code, const std::string& message, const json& data) const {
    json err = {{"code", code}, {"message", message}};
    if (!data.is_null()) err["data"] = data;

    if (!req.id.has_value()) {
        return {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", err}};
    }
    return {{"jsonrpc", "2.0"}, {"id", *req.id}, {"error", err}};
}

json McpProtocol::handle_request(const json& request) {
    auto req = parse_request(request);

    if (req.jsonrpc != "2.0") {
        return make_error_response(req, -32600, "Invalid Request: jsonrpc must be \"2.0\"");
    }

    if (req.method.empty() && req.id.has_value()) {
        return make_error_response(req, -32600, "Invalid Request: missing method");
    }

    // initialize is the only unauthenticated method
    if (req.method == "initialize") {
        auto result = handle_initialize(req);
        if (req.id.has_value()) {
            return {{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}};
        }
        return json::object();
    }

    if (!initialized_) {
        return make_error_response(req, -32002, "Server not initialized");
    }

    // notifications/initialized is also unauthenticated
    if (req.method == "notifications/initialized") {
        handle_initialized(req);
        return json::object();
    }

    // ping is always allowed
    if (req.method == "ping") {
        auto result = handle_ping(req);
        if (req.id.has_value()) {
            return {{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}};
        }
        return json::object();
    }

    // ── Authenticated methods ────────────────────────────────────────

    if (req.method == "tools/list") {
        auto result = handle_tools_list(req);
        return req.id.has_value()
            ? json{{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}}
            : json::object();
    }

    if (req.method == "tools/call") {
        if (!check_auth(req, Permission::TaskRead)) {
            return make_error_response(req, -32003,
                std::format("Forbidden: role '{}' insufficient for this tool", role_to_str(req.auth_role)));
        }
        auto result = handle_tools_call(req);
        if (result.contains("code") && result.contains("message")) {
            return make_error_response(req, result["code"].get<int>(), result["message"].get<std::string>());
        }
        return req.id.has_value()
            ? json{{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}}
            : json::object();
    }

    if (req.method == "resources/list") {
        auto result = handle_resources_list(req);
        return req.id.has_value()
            ? json{{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}}
            : json::object();
    }

    if (req.method == "resources/read") {
        if (!check_auth(req, Permission::TaskRead)) {
            return make_error_response(req, -32003, "Forbidden: insufficient role to read resources");
        }
        auto result = handle_resources_read(req);
        if (result.contains("code") && result.contains("message")) {
            return make_error_response(req, result["code"].get<int>(), result["message"].get<std::string>());
        }
        return req.id.has_value()
            ? json{{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}}
            : json::object();
    }

    if (req.method == "resources/subscribe") {
        auto result = handle_resources_subscribe(req);
        return req.id.has_value()
            ? json{{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}}
            : json::object();
    }

    if (req.method == "prompts/list") {
        auto result = handle_prompts_list(req);
        return req.id.has_value()
            ? json{{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}}
            : json::object();
    }

    if (req.method == "prompts/get") {
        auto result = handle_prompts_get(req);
        if (result.contains("code") && result.contains("message")) {
            return make_error_response(req, result["code"].get<int>(), result["message"].get<std::string>());
        }
        return req.id.has_value()
            ? json{{"jsonrpc", "2.0"}, {"id", *req.id}, {"result", result}}
            : json::object();
    }

    return make_error_response(req, -32601, std::format("Method not found: {}", req.method));
}

std::string McpProtocol::handle_raw(const std::string& raw_request) {
    try {
        json req = json::parse(raw_request);
        json resp = handle_request(req);
        return resp.dump();
    } catch (const json::parse_error& e) {
        json err = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", -32700}, {"message", "Parse error"}, {"data", e.what()}}}
        };
        return err.dump();
    }
}

void McpProtocol::register_tool(McpToolDef def, std::function<json(const json&)> handler) {
    auto name = def.name;
    tools_[name] = std::move(def);
    tool_handlers_[name] = std::move(handler);
}

void McpProtocol::register_resource(McpResourceDef def, std::function<json(const json&)> handler) {
    auto uri = def.uri;
    resources_[uri] = std::move(def);
    resource_handlers_[uri] = std::move(handler);
}

void McpProtocol::register_prompt(McpPromptDef def, std::function<json(const json&)> handler) {
    auto name = def.name;
    prompts_[name] = std::move(def);
    prompt_handlers_[name] = std::move(handler);
}

void McpProtocol::set_notification_handler(const std::string& method, std::function<void(const json&)> handler) {
    notification_handlers_[method] = std::move(handler);
}

void McpProtocol::send_notification(const std::string& method, const json& params) {
    if (notification_cb_) notification_cb_(method, params);
}

void McpProtocol::on_notification(NotificationCallback cb) {
    notification_cb_ = std::move(cb);
}

json McpProtocol::handle_initialize(const McpRequest&) {
    initialized_ = true;

    json capabilities = {};
    if (caps_.tools) {
        capabilities["tools"] = {{"listChanged", true}};
    }
    if (caps_.resources) {
        capabilities["resources"] = {{"subscribe", true}, {"listChanged", true}};
    }
    if (caps_.prompts) {
        capabilities["prompts"] = {{"listChanged", true}};
    }
    if (caps_.logging) {
        capabilities["logging"] = {};
    }

    return {
        {"protocolVersion", "2025-03-26"},
        {"capabilities", capabilities},
        {"serverInfo", {{"name", info_.name}, {"version", info_.version}}},
    };
}

json McpProtocol::handle_initialized(const McpRequest&) {
    spdlog::info("Client initialized");
    return {};
}

json McpProtocol::handle_tools_list(const McpRequest& req) {
    int cursor = 0;
    int limit = 100;
    if (req.params.contains("_cursor") && req.params["_cursor"].is_number()) cursor = req.params["_cursor"].get<int>();
    if (req.params.contains("_limit") && req.params["_limit"].is_number()) limit = req.params["_limit"].get<int>();
    if (limit <= 0) limit = 100;
    if (limit > 1000) limit = 1000;

    json tools = json::array();
    int i = 0;
    int count = 0;
    for (const auto& entry : tools_) {
        const auto& name = entry.first;
        const auto& def = entry.second;
        if (!has_permission(req.auth_role, def.required_permission)) continue;
        if (i < cursor) { i++; continue; }
        if (count >= limit) break;
        json t = {
            {"name", def.name},
            {"description", def.description},
            {"inputSchema", def.input_schema},
        };
        tools.push_back(t);
        count++;
        i++;
    }

    json result = {{"tools", tools}};
    if (i < static_cast<int>(tools_.size())) {
        result["_nextCursor"] = i;
    }
    return result;
}

json McpProtocol::handle_tools_call(const McpRequest& req) {
    auto name = req.params.value("name", "");
    auto args = req.params.value("arguments", json::object());

    if (req.params.contains("_auth")) {
        args["_auth"] = req.params["_auth"];
    }

    auto it = tool_handlers_.find(name);
    if (it == tool_handlers_.end()) {
        return {{"code", -32601}, {"message", std::format("Tool not found: {}", name)}};
    }

    // Per-tool permission check
    auto def_it = tools_.find(name);
    if (def_it != tools_.end() && !has_permission(req.auth_role, def_it->second.required_permission)) {
        return {{"code", -32003}, {"message",
            std::format("Forbidden: role '{}' lacks permission for tool '{}'",
                role_to_str(req.auth_role), name)}};
    }

    try {
        json result = it->second(args);
        return {{"content", json::array({{{"type", "text"}, {"text", result.dump()}}})}, {"isError", false}};
    } catch (const std::exception& e) {
        return {{"content", json::array({{{"type", "text"}, {"text", std::format("Error: {}", e.what())}}})}, {"isError", true}};
    }
}

json McpProtocol::handle_resources_list(const McpRequest& req) {
    int cursor = 0;
    int limit = 100;
    if (req.params.contains("_cursor") && req.params["_cursor"].is_number()) cursor = req.params["_cursor"].get<int>();
    if (req.params.contains("_limit") && req.params["_limit"].is_number()) limit = req.params["_limit"].get<int>();
    if (limit <= 0) limit = 100;
    if (limit > 1000) limit = 1000;

    json res = json::array();
    int i = 0;
    int count = 0;
    for (const auto& entry : resources_) {
        const auto& uri = entry.first;
        const auto& def = entry.second;
        if (!has_permission(req.auth_role, def.required_permission)) continue;
        if (i < cursor) { i++; continue; }
        if (count >= limit) break;
        res.push_back({
            {"uri", def.uri},
            {"name", def.name},
            {"description", def.description},
            {"mimeType", def.mime_type},
        });
        count++;
        i++;
    }

    json result = {{"resources", res}};
    if (i < static_cast<int>(resources_.size())) {
        result["_nextCursor"] = i;
    }
    return result;
}

json McpProtocol::handle_resources_read(const McpRequest& req) {
    auto uri = req.params.value("uri", "");
    auto it = resource_handlers_.find(uri);
    if (it == resource_handlers_.end()) {
        return {{"code", -32602}, {"message", std::format("Resource not found: {}", uri)}};
    }

    // Per-resource permission check
    auto def_it = resources_.find(uri);
    if (def_it != resources_.end() && !has_permission(req.auth_role, def_it->second.required_permission)) {
        return {{"code", -32003}, {"message",
            std::format("Forbidden: role '{}' lacks permission for resource '{}'",
                role_to_str(req.auth_role), uri)}};
    }

    json content = it->second(req.params);
    return {{"contents", json::array({{{"uri", uri}, {"mimeType", "application/json"}, {"text", content.dump()}}})}};
}

json McpProtocol::handle_resources_subscribe(const McpRequest&) {
    return {{"subscribed", true}};
}

json McpProtocol::handle_prompts_list(const McpRequest& req) {
    int cursor = 0;
    int limit = 100;
    if (req.params.contains("_cursor") && req.params["_cursor"].is_number()) cursor = req.params["_cursor"].get<int>();
    if (req.params.contains("_limit") && req.params["_limit"].is_number()) limit = req.params["_limit"].get<int>();
    if (limit <= 0) limit = 100;
    if (limit > 1000) limit = 1000;

    json prompts = json::array();
    int i = 0;
    int count = 0;
    for (const auto& entry : prompts_) {
        const auto& name = entry.first;
        const auto& def = entry.second;
        if (i < cursor) { i++; continue; }
        if (count >= limit) break;
        json p = {{"name", def.name}, {"description", def.description}};
        if (!def.arguments.empty()) {
            json args = json::array();
            for (const auto& arg : def.arguments) args.push_back(arg);
            p["arguments"] = args;
        }
        prompts.push_back(p);
        count++;
        i++;
    }

    json result = {{"prompts", prompts}};
    if (i < static_cast<int>(prompts_.size())) {
        result["_nextCursor"] = i;
    }
    return result;
}

json McpProtocol::handle_prompts_get(const McpRequest& req) {
    auto name = req.params.value("name", "");
    auto args = req.params.value("arguments", json::object());

    auto it = prompt_handlers_.find(name);
    if (it == prompt_handlers_.end()) {
        return {{"code", -32602}, {"message", std::format("Prompt not found: {}", name)}};
    }

    json content = it->second(args);
    return {{"messages", content}};
}

json McpProtocol::handle_ping(const McpRequest&) {
    return json::object();
}

}