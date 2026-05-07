#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/uuid.hpp"
#include <spdlog/spdlog.h>
#include <sstream>

namespace mcp_collab {

SseStream::ClientId SseStream::add_client(httplib::Response& res) {
    auto id = generate_uuid();
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, id](size_t, httplib::DataSink& sink) -> bool {
            std::string init = std::format("event: connected\ndata: {{\"clientId\": \"{}\"}}\n\n", id);
            sink.write(init.c_str(), init.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return true;
        }
    );

    std::lock_guard lock(mutex_);
    return id;
}

void SseStream::remove_client(const ClientId& id) {
    std::lock_guard lock(mutex_);
    clients_.erase(id);
}

void SseStream::broadcast(const std::string& method, const json& params) {
    json notification = {
        {"jsonrpc", "2.0"},
        {"method", std::format("notifications/{}", method)},
        {"params", params},
    };
    std::string data = std::format("event: message\ndata: {}\n\n", notification.dump());

    std::lock_guard lock(mutex_);
    for (auto& [id, sender] : clients_) {
        sender(data);
    }
}

size_t SseStream::client_count() const {
    std::lock_guard lock(mutex_);
    return clients_.size();
}

StreamableHttpTransport::StreamableHttpTransport(McpProtocol& protocol, AuthProvider& auth,
                                                 const StreamableHttpConfig& config)
    : protocol_(protocol), auth_(auth), config_(config) {
    protocol_.on_notification([this](const std::string& method, const json& params) {
        send_sse_notification(method, params);
    });
}

StreamableHttpTransport::~StreamableHttpTransport() {
    stop();
}

void StreamableHttpTransport::setup_routes() {
    server_ = std::make_unique<httplib::Server>();

    server_->set_default_headers({
        {"Access-Control-Allow-Origin", config_.cors_origin},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, Accept"},
    });

    server_->Options(config_.endpoint, [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    server_->Post(config_.endpoint, [this](const httplib::Request& req, httplib::Response& res) {
        handle_post(req, res);
    });

    server_->Get(config_.endpoint, [this](const httplib::Request& req, httplib::Response& res) {
        handle_get(req, res);
    });

    server_->Delete(config_.endpoint, [this](const httplib::Request& req, httplib::Response& res) {
        handle_delete(req, res);
    });

    spdlog::info("HTTP routes configured: endpoint={} auth={}", config_.endpoint, config_.require_auth);
}

std::optional<AuthToken> StreamableHttpTransport::authenticate(const httplib::Request& req) const {
    if (!config_.require_auth) {
        // Return a synthetic token for anonymous access (observer role)
        static AuthToken anon;
        anon.token_id = "anonymous";
        anon.agent_id = "anonymous";
        anon.role = Role::Observer;
        anon.swarm_id = "";
        anon.expires_at = std::chrono::system_clock::now() + std::chrono::hours(24);
        return anon;
    }

    std::string auth_header = req.get_header_value("Authorization");
    if (auth_header.empty()) {
        // Also accept token as query parameter for SSE clients
        auto query_token = req.get_param_value("token");
        if (!query_token.empty()) {
            return auth_.validate_token(query_token);
        }
        return std::nullopt;
    }

    auto bearer = AuthProvider::extract_bearer(auth_header);
    if (!bearer) return std::nullopt;

    return auth_.validate_token(*bearer);
}

bool StreamableHttpTransport::check_permission(const AuthToken& token, Permission perm) const {
    return has_permission(token.role, perm);
}

void StreamableHttpTransport::handle_post(const httplib::Request& req, httplib::Response& res) {
    // Auth check
    auto token = authenticate(req);
    if (!token) {
        res.status = 401;
        res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"}})", "application/json");
        return;
    }

    if (req.body.empty()) {
        res.status = 400;
        res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Empty body"}})", "application/json");
        return;
    }

    std::string response_str;
    try {
        json body = json::parse(req.body);

        // Inject auth context into params for authorization checks
        json auth_context = {
            {"_auth", {
                {"agent_id", token->agent_id},
                {"role", role_to_str(token->role)},
                {"swarm_id", token->swarm_id},
            }}
        };

        if (body.is_array()) {
            json results = json::array();
            for (auto item : body) {
                if (item.contains("params") && item["params"].is_object()) {
                    item["params"].merge_patch(auth_context);
                } else {
                    item["params"] = auth_context;
                }
                results.push_back(protocol_.handle_request(item));
            }
            response_str = results.dump();
        } else {
            if (body.contains("params") && body["params"].is_object()) {
                body["params"].merge_patch(auth_context);
            } else {
                body["params"] = auth_context;
            }
            response_str = protocol_.handle_raw(req.body.empty() ? "{}" : body.dump());
        }
    } catch (const json::parse_error& e) {
        json err = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", -32700}, {"message", std::format("Parse error: {}", e.what())}}}
        };
        response_str = err.dump();
    }

    bool is_notification = false;
    try {
        json body = json::parse(req.body);
        is_notification = !body.contains("id") || body["id"].is_null();
    } catch (...) {}

    if (is_notification) {
        res.status = 202;
        res.set_content("", "application/json");
    } else {
        res.set_content(response_str, "application/json");
    }
}

void StreamableHttpTransport::handle_get(const httplib::Request& req, httplib::Response& res) {
    auto token = authenticate(req);
    if (!token) {
        res.status = 401;
        res.set_content(R"({"error":"Authentication required for SSE stream"})", "application/json");
        return;
    }

    std::string accept = req.get_header_value("Accept");
    if (accept.find("text/event-stream") == std::string::npos) {
        res.status = 400;
        res.set_content(R"({"error":"Accept header must include text/event-stream"})", "application/json");
        return;
    }

    auto client_id = sse_.add_client(res);
    spdlog::info("SSE client connected: {} agent={} role={}", client_id, token->agent_id, role_to_str(token->role));
}

void StreamableHttpTransport::handle_delete(const httplib::Request& req, httplib::Response& res) {
    std::string session_id = req.get_header_value("Mcp-Session-Id");
    if (!session_id.empty()) {
        sse_.remove_client(session_id);
    }
    res.status = 204;
}

void StreamableHttpTransport::send_sse_notification(const std::string& method, const json& params) {
    sse_.broadcast(method, params);
}

void StreamableHttpTransport::start() {
    if (running_.load()) return;

    setup_routes();

    spdlog::info("Starting Swarm MCP server on {}:{}{} (auth={})",
        config_.host, config_.port, config_.endpoint, config_.require_auth ? "on" : "off");
    running_.store(true);

    if (!server_->listen(config_.host, config_.port)) {
        spdlog::error("Failed to start HTTP server on {}:{}", config_.host, config_.port);
        running_.store(false);
        throw std::runtime_error("Failed to start HTTP server");
    }
}

void StreamableHttpTransport::stop() {
    if (!running_.load()) return;
    running_.store(false);
    if (server_) {
        server_->stop();
    }
    spdlog::info("HTTP server stopped");
}

bool StreamableHttpTransport::is_running() const {
    return running_.load();
}

void StreamableHttpTransport::set_notification_handler(std::function<void(const std::string&, const json&)> handler) {
    (void)handler;
}

}