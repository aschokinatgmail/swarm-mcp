#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/uuid.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <deque>
#include <condition_variable>
#include <atomic>

namespace mcp_collab {

// Apply CORS headers to a response. When `cors_origin` is empty, no CORS
// headers are emitted — browsers then apply their default same-origin policy,
// which is the correct behavior for an unconfigured CORS policy. An empty
// `Access-Control-Allow-Origin` header would be worse than absent: browsers
// treat it as "deny all cross-origin", breaking every CORS request.
static void apply_cors(const StreamableHttpConfig& config, httplib::Response& res) {
    if (config.cors_origin.empty()) return;
    res.set_header("Access-Control-Allow-Origin", config.cors_origin);
    res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, Accept");
}

RateLimiter::RateLimiter(int max_requests_per_minute) : max_rpm_(max_requests_per_minute) {}

bool RateLimiter::allow(const std::string& key) {
    if (max_rpm_ <= 0) return true;

    std::lock_guard lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto& timestamps = buckets_[key];

    // Evict timestamps older than the 60-second sliding window.
    while (!timestamps.empty() &&
           std::chrono::duration_cast<std::chrono::seconds>(now - timestamps.front()) >=
               std::chrono::seconds(60)) {
        timestamps.pop_front();
    }

    if (static_cast<int>(timestamps.size()) >= max_rpm_) {
        return false;
    }

    timestamps.push_back(now);
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

    server_->Options(config_.endpoint, [this](const httplib::Request&, httplib::Response& res) {
        apply_cors(config_, res);
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

    server_->Get("/health", [this](const httplib::Request&, httplib::Response& res) {
        apply_cors(config_, res);
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
    apply_cors(config_, res);

    // Rate-limit BEFORE auth (#74): throttle brute-force token attempts by
    // remote address before the expensive token validation runs.
    std::string rate_key = req.remote_addr;
    if (!rate_limiter_.allow(rate_key)) {
        res.status = 429;
        res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32004,"message":"Rate limit exceeded"}})", "application/json");
        return;
    }

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
    bool is_notification = false;
    try {
        json body = json::parse(req.body);

        // Reject oversized JSON-RPC batches (#90): a batch larger than
        // kMaxJsonRpcBatchSize is a resource-exhaustion vector.
        if (body.is_array() && body.size() > kMaxJsonRpcBatchSize) {
            json err = {
                {"jsonrpc", "2.0"},
                {"id", nullptr},
                {"error", {{"code", -32600}, {"message", std::format("Batch size limit exceeded: maximum {} requests per batch", kMaxJsonRpcBatchSize)}}}
            };
            res.status = 400;
            res.set_content(err.dump(), "application/json");
            return;
        }

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
    apply_cors(config_, res);

    // Rate-limit BEFORE auth (#74): throttle brute-force token attempts by
    // remote address before the expensive token validation runs.
    if (!rate_limiter_.allow(req.remote_addr)) {
        res.status = 429;
        res.set_content(R"({"jsonrpc":"2.0","error":{"code":-32004,"message":"Rate limit exceeded"}})", "application/json");
        return;
    }

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

    auto queue = std::make_shared<std::deque<std::string>>();
    auto queue_mutex = std::make_shared<std::mutex>();
    auto queue_cv = std::make_shared<std::condition_variable>();
    auto disconnected = std::make_shared<std::atomic<bool>>(false);

    auto sink_fn = [queue, queue_mutex, queue_cv, disconnected](const std::string& data) -> bool {
        if (disconnected->load()) return false;
        {
            std::lock_guard lock(*queue_mutex);
            // Backpressure: drop the oldest entry when the queue is full so the
            // client keeps seeing the most recent events rather than the
            // stalest ones. This bounds per-client memory (CWE-400).
            if (queue->size() >= kMaxSseQueueEntries) {
                spdlog::warn("SSE queue full ({} entries); dropping oldest event for slow client",
                             kMaxSseQueueEntries);
                queue->pop_front();
            }
            queue->push_back(data);
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
                        queue->pop_front();
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
    apply_cors(config_, res);
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

    if (!server_->bind_to_port(config_.host, config_.port)) {
        spdlog::error("Failed to bind HTTP server on {}:{}", config_.host, config_.port);
        throw std::runtime_error("Failed to bind HTTP server");
    }

    running_.store(true);
    server_thread_ = std::thread([this]() {
        if (!server_->listen_after_bind()) {
            spdlog::error("HTTP server stopped unexpectedly on {}:{}", config_.host, config_.port);
            running_.store(false);
        }
    });

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!server_->is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!server_->is_running()) {
        spdlog::error("Failed to start HTTP server on {}:{}", config_.host, config_.port);
        running_.store(false);
        server_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        throw std::runtime_error("Failed to start HTTP server");
    }
}

void StreamableHttpTransport::stop() {
    if (!running_.load() && !server_thread_.joinable()) return;
    running_.store(false);
    if (server_) {
        server_->stop();
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
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
