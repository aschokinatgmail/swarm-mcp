#pragma once

#include <string>
#include <optional>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"

namespace mcp_collab {

// Maximum number of queued SSE events per client. When a slow/stalled client
// cannot drain events fast enough, the oldest entries are dropped to keep the
// queue bounded (CWE-400 / slow-reader DoS mitigation).
inline constexpr std::size_t kMaxSseQueueEntries = 1024;

struct StreamableHttpConfig {
    std::string host{"0.0.0.0"};
    uint16_t port{3001};
    std::string endpoint{"/mcp"};
    std::string cors_origin{""};
    int thread_pool_size{4};
    bool require_auth{true};
    int rate_limit_rpm{0};
};

class RateLimiter {
public:
    explicit RateLimiter(int max_requests_per_minute = 0);

    bool allow(const std::string& key);

private:
    int max_rpm_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> buckets_;
};

class SseStream {
public:
    using ClientId = std::string;
    using SinkFn = std::function<bool(const std::string&)>;

    SseStream() = default;

    ClientId add_client(SinkFn sink);
    void remove_client(const ClientId& id);
    void broadcast(const std::string& method, const json& params);
    size_t client_count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<ClientId, SinkFn> clients_;
};

class StreamableHttpTransport {
public:
    explicit StreamableHttpTransport(McpProtocol& protocol, AuthProvider& auth,
                                     const StreamableHttpConfig& config = {});
    ~StreamableHttpTransport();

    void start();
    void stop();
    bool is_running() const;

    void set_notification_handler(std::function<void(const std::string&, const json&)> handler);

    // Internal request handlers — accessible to unit tests via FRIEND_TEST
    void handle_post(const httplib::Request& req, httplib::Response& res);
    void handle_get(const httplib::Request& req, httplib::Response& res);
    void handle_delete(const httplib::Request& req, httplib::Response& res);

private:
    void setup_routes();
    void send_sse_notification(const std::string& method, const json& params);

    std::optional<AuthToken> authenticate(const httplib::Request& req) const;
    bool check_permission(const AuthToken& token, Permission perm) const;

    McpProtocol& protocol_;
    AuthProvider& auth_;
    StreamableHttpConfig config_;
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    SseStream sse_;
    RateLimiter rate_limiter_;
    std::atomic<bool> running_{false};
    std::function<void(const std::string&, const json&)> notification_handler_;
};

}
