#pragma once

#include <string>
#include <optional>
#include <vector>
#include <unordered_map>
#include <functional>
#include <chrono>

#include <nlohmann/json.hpp>
#include "mcp_collab/auth.hpp"

namespace mcp_collab {

using json = nlohmann::json;

enum class JsonRpcErrorCode : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    ServerNotInitialized = -32002,
    Unauthorized = -32001,
    Forbidden = -32003,
};

struct McpToolDef {
    std::string name;
    std::string description;
    json input_schema;
    Permission required_permission{Permission::TaskRead}; // default, overridden per tool
};

struct McpResourceDef {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type{"application/json"};
    Permission required_permission{Permission::TaskRead};
};

struct McpPromptDef {
    std::string name;
    std::string description;
    std::vector<json> arguments;
};

struct McpRequest {
    std::string jsonrpc{"2.0"};
    std::optional<json> id;
    std::string method;
    json params{json::object()};
    // Auth context injected by transport
    std::string auth_agent_id;
    Role auth_role{Role::Observer};
    std::string auth_swarm_id;
};

struct McpResponse {
    std::string jsonrpc{"2.0"};
    json id{nullptr};
    std::optional<json> result;
    std::optional<json> error;
};

struct ServerInfo {
    std::string name{"swarm-mcp"};
    std::string version{"1.0.0"};
};

struct ServerCapabilities {
    bool tools{true};
    bool resources{true};
    bool prompts{true};
    bool logging{true};
};

class McpProtocol {
public:
    explicit McpProtocol(const ServerInfo& info, const ServerCapabilities& caps = {});

    json handle_request(const json& request);
    std::string handle_raw(const std::string& raw_request);

    void register_tool(McpToolDef def, std::function<json(const json&)> handler);
    void register_resource(McpResourceDef def, std::function<json(const json&)> handler);
    void register_prompt(McpPromptDef def, std::function<json(const json&)> handler);

    void set_notification_handler(const std::string& method, std::function<void(const json&)> handler);
    void send_notification(const std::string& method, const json& params);

    using NotificationCallback = std::function<void(const std::string& method, const json& params)>;
    void on_notification(NotificationCallback cb);

private:
    McpRequest parse_request(const json& req) const;
    json make_error_response(const McpRequest& req, int code, const std::string& message, const json& data = {}) const;
    bool check_auth(const McpRequest& req, Permission required) const;

    json handle_initialize(const McpRequest& req);
    json handle_initialized(const McpRequest& req);
    json handle_tools_list(const McpRequest& req);
    json handle_tools_call(const McpRequest& req);
    json handle_resources_list(const McpRequest& req);
    json handle_resources_read(const McpRequest& req);
    json handle_resources_subscribe(const McpRequest& req);
    json handle_prompts_list(const McpRequest& req);
    json handle_prompts_get(const McpRequest& req);
    json handle_ping(const McpRequest& req);

    ServerInfo info_;
    ServerCapabilities caps_;
    bool initialized_{false};

    std::unordered_map<std::string, McpToolDef> tools_;
    std::unordered_map<std::string, std::function<json(const json&)>> tool_handlers_;
    std::unordered_map<std::string, McpResourceDef> resources_;
    std::unordered_map<std::string, std::function<json(const json&)>> resource_handlers_;
    std::unordered_map<std::string, McpPromptDef> prompts_;
    std::unordered_map<std::string, std::function<json(const json&)>> prompt_handlers_;
    std::unordered_map<std::string, std::function<void(const json&)>> notification_handlers_;
    NotificationCallback notification_cb_;
};

}