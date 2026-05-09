#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/uuid.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <queue>
#include <condition_variable>
#include <atomic>

namespace mcp_collab {

RateLimiter::RateLimiter(int max_requests_per_minute) : max_rpm_(max_requests_per_minute) {}

bool RateLimiter::allow(const std::string& key) {
    if (max_rpm_ <= 0) return true;

    std::lock_guard lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto it = buckets_.find(key);

    if (it == buckets_.end()) {
        buckets_[key] = {1, now};
        return true;
    }

    auto& [count, window_start] = it->second;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - window_start);

    if (elapsed >= std::chrono::seconds(60)) {
        count = 1;
        window_start = now;
        return true;
    }

    if (count >= max_rpm_) {
        return false;
    }

    count++;
    return true;
}

SseStream::ClientId SseStream::add_client(SinkFn sink) {
    auto id = generate_uuid();
    std::lock_guard lock(mutex_);
    clients_[id] = std::move(sink);
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
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (!it->second(data)) {
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t SseStream::client_count() const {
    std::lock_guard lock(mutex_);
    return clients_.size();
}

StreamableHttpTransport::StreamableHttpTransport(McpProtocol& protocol, AuthProvider& auth,
                                                 const StreamableHttpConfig& config)
    : protocol_(protocol), auth_(auth), config_(config), rate_limiter_(config.rate_limit_rpm) {
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

    server_->Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    spdlog::info("HTTP routes configured: endpoint={} auth={}", config_.endpoint, config_.require_auth);
}

std::optional<AuthToken> StreamableHttpTransport::authenticate(const httplib::Request& req) const {
    if (!config_.require_auth) {
        thread_local AuthToken anon = [] {
            AuthToken t;
            t.token_id = "anonymous";
            t.agent_id = "anonymous";
      t.role = Role::Coordinator;
      t.swarm_id = "default";
            t.expires_at = std::chrono::system_clock::now() + std::chrono::hours(87600);
            return t;
        }();
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
    auto token = authenticate(req);
    if (!token) {
        res.status = 401;
        res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32001,"message":"Authentication required"}})", "application/json");
        return;
    }

    std::string rate_key = token->agent_id.empty() ? req.remote_addr : token->agent_id;
    if (!rate_limiter_.allow(rate_key)) {
        res.status = 429;
        res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32004,"message":"Rate limit exceeded"}})", "application/json");
        return;
    }

    if (req.body.empty()) {
        res.status = 400;
        res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32700,"message":"Empty body"}})", "application/json");
        return;
    }

    std::string response_str;
    bool is_notification = false;
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
            is_notification = true;
            for (const auto& item : body) {
                if (item.contains("id") && !item["id"].is_null()) {
                    is_notification = false;
                    break;
                }
            }
        } else {
            if (body.contains("params") && body["params"].is_object()) {
                body["params"].merge_patch(auth_context);
            } else {
                body["params"] = auth_context;
            }
            response_str = protocol_.handle_raw(body.dump());
            is_notification = !body.contains("id") || body["id"].is_null();
        }
    } catch (const json::parse_error& e) {
        json err = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", -32700}, {"message", std::format("Parse error: {}", e.what())}}}
        };
        response_str = err.dump();
    }

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

    auto queue = std::make_shared<std::queue<std::string>>();
    auto queue_mutex = std::make_shared<std::mutex>();
    auto queue_cv = std::make_shared<std::condition_variable>();
    auto disconnected = std::make_shared<std::atomic<bool>>(false);

    auto sink_fn = [queue, queue_mutex, queue_cv, disconnected](const std::string& data) -> bool {
        if (disconnected->load()) return false;
        {
            std::lock_guard lock(*queue_mutex);
            queue->push(data);
        }
        queue_cv->notify_one();
        return true;
    };

    auto client_id = sse_.add_client(std::move(sink_fn));

    res.set_chunked_content_provider(
        "text/event-stream",
        [this, client_id, queue, queue_mutex, queue_cv, disconnected](size_t, httplib::DataSink& sink) -> bool {
            std::string init = std::format("event: connected\ndata: {{\"clientId\": \"{}\"}}\n\n", client_id);
            sink.write(init.c_str(), init.size());

            while (!disconnected->load()) {
                std::string msg;
                {
                    std::unique_lock lock(*queue_mutex);
                    queue_cv->wait_for(lock, std::chrono::milliseconds(100), [&] {
                        return !queue->empty() || disconnected->load();
                    });
                    if (disconnected->load() && queue->empty()) break;
                    if (!queue->empty()) {
                        msg = std::move(queue->front());
                        queue->pop();
                    }
                }
                if (!msg.empty()) {
                    if (!sink.write(msg.c_str(), msg.size())) return false;
                }
            }
            return true;
        },
        [this, client_id, queue_cv, disconnected](bool success) {
            disconnected->store(true);
            queue_cv->notify_all();
            sse_.remove_client(client_id);
        });

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
    if (notification_handler_) notification_handler_(method, params);
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
    notification_handler_ = std::move(handler);
}

}