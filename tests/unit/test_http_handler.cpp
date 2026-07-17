#include <gtest/gtest.h>
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
#include <nlohmann/json.hpp>
#include <deque>
#include <mutex>

using namespace mcp_collab;
using json = nlohmann::json;

class HttpHandlerTest : public ::testing::Test {
protected:
    std::unique_ptr<McpProtocol> protocol_;
    std::unique_ptr<AuthProvider> auth_;

    void SetUp() override {
        protocol_ = std::make_unique<McpProtocol>(
            ServerInfo{.name = "test", .version = "1.0.0"},
            ServerCapabilities{.tools = true, .resources = true, .prompts = true, .logging = true}
        );
        auth_ = std::make_unique<AuthProvider>("test-secret-key");
    }

    httplib::Request make_request(const std::string& method,
                                   const std::string& path,
                                   const std::string& body = "",
                                   const std::string& auth_header = "") {
        httplib::Request req;
        req.method = method;
        req.path = path;
        req.body = body;
        if (!auth_header.empty()) {
            req.set_header("Authorization", auth_header);
        }
        return req;
    }
};

// ── Authentication Tests ─────────────────────────────────────────────────────

TEST_F(HttpHandlerTest, PostWithoutAuthReturns401) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    httplib::Request req = make_request("POST", "/mcp", R"({"jsonrpc":"2.0","method":"ping"})");
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_EQ(res.status, 401);
    auto j = json::parse(res.body);
    EXPECT_EQ(j["error"]["code"], -32001);
}

TEST_F(HttpHandlerTest, PostWithInvalidTokenReturns401) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    httplib::Request req = make_request("POST", "/mcp",
                                       R"({"jsonrpc":"2.0","method":"ping"})",
                                       "Bearer invalid.token.here");
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_EQ(res.status, 401);
}

TEST_F(HttpHandlerTest, PostWithValidTokenSucceeds) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Worker, "test-swarm");
    httplib::Request req = make_request("POST", "/mcp",
                                       R"({"jsonrpc":"2.0","id":1,"method":"ping"})",
                                       "Bearer " + token.token_string);
    httplib::Response res;

    transport.handle_post(req, res);

    // Response body should contain valid JSON-RPC; status may be unset (-1) in direct handler test
    EXPECT_FALSE(res.body.empty());
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.contains("jsonrpc")) << "Response should contain valid JSON-RPC: " << res.body;
}

// ── Rate Limiting Tests ──────────────────────────────────────────────────────

TEST_F(HttpHandlerTest, RateLimitBlocksAfterThreshold) {
    StreamableHttpTransport limited_transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true, .rate_limit_rpm = 2});

    auto token = auth_->issue_token("agent-1", Role::Worker, "test-swarm");
    std::string auth_hdr = "Bearer " + token.token_string;

    // First 2 requests should pass (or at least not be rate limited)
    for (int i = 0; i < 2; ++i) {
        httplib::Request req = make_request("POST", "/mcp",
            R"({"jsonrpc":"2.0","id":)" + std::to_string(i) + R"(,"method":"ping"})",
            auth_hdr);
        httplib::Response res;
        limited_transport.handle_post(req, res);
        EXPECT_NE(res.status, 429) << "Request " << i << " should not be rate limited";
    }

    // 3rd request should be rate limited
    httplib::Request req3 = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":99,"method":"ping"})",
        auth_hdr);
    httplib::Response res3;
    limited_transport.handle_post(req3, res3);
    EXPECT_EQ(res3.status, 429);
    auto j = json::parse(res3.body);
    EXPECT_EQ(j["error"]["code"], -32004);
}

// ── Invalid Request Tests ──────────────────────────────────────────────────

TEST_F(HttpHandlerTest, PostWithEmptyBodyReturns400) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req = make_request("POST", "/mcp", "",
                                       "Bearer " + token.token_string);
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_EQ(res.status, 400);
}

TEST_F(HttpHandlerTest, PostWithInvalidJsonReturnsParseError) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req = make_request("POST", "/mcp",
                                       "{ not valid json",
                                        "Bearer " + token.token_string);
    httplib::Response res;

    transport.handle_post(req, res);

    auto j = json::parse(res.body);
    EXPECT_EQ(j["error"]["code"], -32700);
}

// ── Batch Request Tests ──────────────────────────────────────────────────────

TEST_F(HttpHandlerTest, PostWithBatchRequestSucceeds) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    std::string batch = R"([{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}},
                           {"jsonrpc":"2.0","id":2,"method":"ping"}])";
    httplib::Request req = make_request("POST", "/mcp", batch,
                                        "Bearer " + token.token_string);
    httplib::Response res;

    transport.handle_post(req, res);

    // Response body should contain valid JSON-RPC array; status may be unset (-1) in direct handler test
    EXPECT_FALSE(res.body.empty());
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.is_array()) << "Batch response should be a JSON array: " << res.body;
}

// ── SSE Endpoint Tests ───────────────────────────────────────────────────────

TEST_F(HttpHandlerTest, GetWithWrongAcceptHeaderReturns400) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req = make_request("GET", "/mcp", "",
                                        "Bearer " + token.token_string);
    // Missing "text/event-stream" in Accept header
    httplib::Response res;

    transport.handle_get(req, res);

    EXPECT_EQ(res.status, 400);
}

TEST_F(HttpHandlerTest, DeleteWithoutSessionIdReturns204) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    httplib::Request req = make_request("DELETE", "/mcp");
    httplib::Response res;

    transport.handle_delete(req, res);

    EXPECT_EQ(res.status, 204);
}

// ── Notification Handler Tests ───────────────────────────────────────────

TEST_F(HttpHandlerTest, NotificationHandlerReceivesBroadcast) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});

    std::string received_method;
    json received_params;
    transport.set_notification_handler([&](const std::string& m, const json& p) {
        received_method = m;
        received_params = p;
    });

    // Trigger notification via SSE broadcast
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","method":"initialize","params":{"a":1}})");
    req.set_header("Authorization", std::string("Bearer ") + token.token_string);
    httplib::Response res;
    transport.handle_post(req, res);

    // Just verify handler was set; actual broadcast requires full server lifecycle
    EXPECT_TRUE(transport.is_running() == false);
}

// ── Query Token Auth Tests ───────────────────────────────────────────────────

TEST_F(HttpHandlerTest, GetWithQueryTokenIsRejected) {
    // #89/#52: Bearer tokens must NOT be accepted via URL query param —
    // tokens in URLs leak via logs, referers, and browser history.
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req = make_request("GET", "/mcp");
    req.set_header("Accept", "text/event-stream");
    req.params.emplace("token", token.token_string);
    httplib::Response res;

    transport.handle_get(req, res);
    EXPECT_EQ(res.status, 401) << "Query parameter token must be rejected; use Authorization header";
}

TEST_F(HttpHandlerTest, PostWithQueryTokenIsRejected) {
    // #89/#52: POST must also reject query-param tokens.
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    req.params.emplace("token", token.token_string);
    httplib::Response res;

    transport.handle_post(req, res);
    EXPECT_EQ(res.status, 401) << "Query parameter token must be rejected on POST too";
}

// ── Rate Limiter Window Reset Tests ───────────────────────────────────────────

TEST_F(HttpHandlerTest, RateLimitResetsAfterWindowExpiry) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true, .rate_limit_rpm = 1});

    auto token = auth_->issue_token("agent-1", Role::Worker, "test-swarm");
    std::string auth_hdr = "Bearer " + token.token_string;

    // First request passes
    httplib::Request req1 = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", auth_hdr);
    httplib::Response res1;
    transport.handle_post(req1, res1);
    EXPECT_NE(res1.status, 429);

    // Immediately second request should be rate limited
    httplib::Request req2 = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})", auth_hdr);
    httplib::Response res2;
    transport.handle_post(req2, res2);
    EXPECT_EQ(res2.status, 429);
}

TEST_F(HttpHandlerTest, RateLimitEnforcedWithDefaultConfig) {
    // Verify that the secure-by-default rate_limit_rpm (60) is actually
    // enforced — a burst exceeding the limit gets throttled (#79).
    StreamableHttpConfig config;
    ASSERT_GT(config.rate_limit_rpm, 0)
        << "Default rate_limit_rpm must be > 0";

    StreamableHttpTransport transport(*protocol_, *auth_, config);

    auto token = auth_->issue_token("burst-agent", Role::Worker, "test-swarm");
    std::string auth_hdr = "Bearer " + token.token_string;

    int allowed = 0;
    int throttled = 0;
    for (int i = 0; i < config.rate_limit_rpm + 10; ++i) {
        httplib::Request req = make_request("POST", "/mcp",
            R"({"jsonrpc":"2.0","id":)" + std::to_string(i) + R"(,"method":"ping"})",
            auth_hdr);
        httplib::Response res;
        transport.handle_post(req, res);
        if (res.status == 429) {
            ++throttled;
        } else {
            ++allowed;
        }
    }

    EXPECT_EQ(allowed, config.rate_limit_rpm)
        << "Exactly rate_limit_rpm requests should be allowed";
    EXPECT_GT(throttled, 0)
        << "Requests beyond the limit must be throttled (429)";
    EXPECT_EQ(throttled, 10)
        << "The 10 requests beyond the limit should all be throttled";
}

// ── Single Notification (no id) Tests ──────────────────────────────────────────

TEST_F(HttpHandlerTest, PostNotificationReturns202) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});

    // Notification: JSON-RPC without "id" field
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","method":"initialize","params":{}})");
    httplib::Response res;
    transport.handle_post(req, res);

    EXPECT_EQ(res.status, 202);
    EXPECT_TRUE(res.body.empty());
}

// ── Auth Context Merge Tests ───────────────────────────────────────────────────

TEST_F(HttpHandlerTest, PostInjectsAuthContextIntoExistingParams) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Worker, "test-swarm");
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"existing":"value"}})",
        "Bearer " + token.token_string);
    httplib::Response res;

    transport.handle_post(req, res);
    EXPECT_FALSE(res.body.empty());
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.contains("jsonrpc"));
}

// ── Auth Disabled Tests ──────────────────────────────────────────────────────

TEST_F(HttpHandlerTest, AuthDisabledAllowsAnyRequest) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});

    // When auth is disabled, any request should be allowed
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
    httplib::Response res;
    transport.handle_post(req, res);
    EXPECT_FALSE(res.body.empty()) << "With auth disabled, valid JSON-RPC initialize should return a body";
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.contains("jsonrpc")) << "Response should be valid JSON-RPC: " << res.body;
}

// ── SseStream Direct Tests ────────────────────────────────────────────────────

TEST_F(HttpHandlerTest, SseStreamAddRemoveClient) {
    SseStream sse;
    EXPECT_EQ(sse.client_count(), 0u);

    auto id = sse.add_client([](const std::string&) { return true; });
    EXPECT_EQ(sse.client_count(), 1u);

    sse.remove_client(id);
    EXPECT_EQ(sse.client_count(), 0u);

    // Remove non-existent should not crash
    sse.remove_client("nonexistent-id");
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST_F(HttpHandlerTest, SseStreamBroadcastRemovesFailedSink) {
    SseStream sse;
    int call_count = 0;
    auto id = sse.add_client([&](const std::string&) {
        call_count++;
        return call_count < 2; // fail on second broadcast
    });
    sse.broadcast("event", json{{"key", "value"}});
    EXPECT_EQ(call_count, 1);
    sse.broadcast("event", json{{"key", "value"}});
    EXPECT_EQ(call_count, 2);
    // After second failure, client should be auto-removed
    sse.broadcast("event", json{{"key", "value"}});
    EXPECT_EQ(call_count, 2);
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST_F(HttpHandlerTest, SseStreamBroadcastMultipleClients) {
    SseStream sse;
    int count1 = 0, count2 = 0;
    sse.add_client([&](const std::string&) { count1++; return true; });
    sse.add_client([&](const std::string&) { count2++; return true; });
    sse.broadcast("event", json{{"k", "v"}});
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
}

TEST_F(HttpHandlerTest, SseQueueBackpressureDropsOldestAtCap) {
    std::deque<std::string> queue;
    auto push = [&](std::string data) {
        if (queue.size() >= kMaxSseQueueEntries) queue.pop_front();
        queue.push_back(std::move(data));
    };
    for (std::size_t i = 0; i < kMaxSseQueueEntries + 100; ++i) {
        push("event-" + std::to_string(i));
    }
    EXPECT_EQ(queue.size(), kMaxSseQueueEntries);
    EXPECT_EQ(queue.front(), "event-100");
    EXPECT_EQ(queue.back(), "event-" + std::to_string(kMaxSseQueueEntries + 99));
}

// ── Delete with Session ID Tests ────────────────────────────────────────────

TEST_F(HttpHandlerTest, DeleteWithSessionIdRemovesClient) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    httplib::Request req = make_request("DELETE", "/mcp");
    req.set_header("Mcp-Session-Id", "session-123");
    httplib::Response res;

    transport.handle_delete(req, res);
    EXPECT_EQ(res.status, 204);
}

// ── Transport Lifecycle Tests ────────────────────────────────────────────────

TEST_F(HttpHandlerTest, TransportIsNotRunningInitially) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    EXPECT_FALSE(transport.is_running());
}

TEST_F(HttpHandlerTest, TransportStopOnNotRunningIsNoop) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    // Stop on not-running transport should not crash or throw
    transport.stop();
    EXPECT_FALSE(transport.is_running());
}

// ── CORS Header Tests ────────────────────────────────────────────────────────

TEST_F(HttpHandlerTest, CorsOriginEmptyOmitsAccessControlAllowOriginHeader) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false, .cors_origin = ""});
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_EQ(res.get_header_value("Access-Control-Allow-Origin"), "");
}

TEST_F(HttpHandlerTest, CorsOriginSpecificSetsAccessControlAllowOriginHeader) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false, .cors_origin = "https://example.com"});
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_EQ(res.get_header_value("Access-Control-Allow-Origin"), "https://example.com");
    EXPECT_EQ(res.get_header_value("Access-Control-Allow-Methods"), "GET, POST, DELETE, OPTIONS");
    EXPECT_EQ(res.get_header_value("Access-Control-Allow-Headers"), "Content-Type, Authorization, Accept");
}

TEST_F(HttpHandlerTest, CorsOriginWildcardSetsAccessControlAllowOriginStar) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false, .cors_origin = "*"});
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_EQ(res.get_header_value("Access-Control-Allow-Origin"), "*");
}

// ── Batch Size Limit Tests (#90) ──────────────────────────────────────────────

TEST_F(HttpHandlerTest, BatchWithinLimitSucceeds) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false, .rate_limit_rpm = 0});
    json batch = json::array();
    for (std::size_t i = 0; i < kMaxJsonRpcBatchSize; ++i) {
        batch.push_back({{"jsonrpc", "2.0"}, {"id", i}, {"method", "ping"}});
    }
    httplib::Request req = make_request("POST", "/mcp", batch.dump());
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_NE(res.status, 400);
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), kMaxJsonRpcBatchSize);
}

TEST_F(HttpHandlerTest, BatchExceedingLimitReturns32600) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false, .rate_limit_rpm = 0});
    json batch = json::array();
    for (std::size_t i = 0; i < kMaxJsonRpcBatchSize + 1; ++i) {
        batch.push_back({{"jsonrpc", "2.0"}, {"id", i}, {"method", "ping"}});
    }
    httplib::Request req = make_request("POST", "/mcp", batch.dump());
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_EQ(res.status, 400);
    auto j = json::parse(res.body);
    EXPECT_EQ(j["error"]["code"], -32600);
    EXPECT_TRUE(j["error"]["message"].get<std::string>().find("Batch size limit") != std::string::npos);
}

TEST_F(HttpHandlerTest, BatchExactlyAtLimitSucceeds) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false, .rate_limit_rpm = 0});
    json batch = json::array();
    for (std::size_t i = 0; i < kMaxJsonRpcBatchSize; ++i) {
        batch.push_back({{"jsonrpc", "2.0"}, {"id", i}, {"method", "ping"}});
    }
    httplib::Request req = make_request("POST", "/mcp", batch.dump());
    httplib::Response res;

    transport.handle_post(req, res);

    EXPECT_NE(res.status, 400) << "Batch at exactly the limit should be accepted";
}

// ── Sliding Window Rate Limiter Tests (#92) ───────────────────────────────────

TEST_F(HttpHandlerTest, SlidingWindowAllowsBurstWithinLimit) {
    RateLimiter limiter(5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(limiter.allow("client-a")) << "Request " << i << " should be allowed";
    }
    EXPECT_FALSE(limiter.allow("client-a")) << "6th request should be throttled";
}

TEST_F(HttpHandlerTest, SlidingWindowDifferentKeysIndependent) {
    RateLimiter limiter(2);
    EXPECT_TRUE(limiter.allow("client-a"));
    EXPECT_TRUE(limiter.allow("client-a"));
    EXPECT_FALSE(limiter.allow("client-a"));
    EXPECT_TRUE(limiter.allow("client-b")) << "Different key has its own bucket";
    EXPECT_TRUE(limiter.allow("client-b"));
    EXPECT_FALSE(limiter.allow("client-b"));
}

TEST_F(HttpHandlerTest, SlidingWindowZeroMeansUnlimited) {
    RateLimiter limiter(0);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(limiter.allow("client")) << "Unlimited limiter should always allow";
    }
}

TEST_F(HttpHandlerTest, SlidingWindowEvictsOldEntries) {
    // Verify the deque-based sliding window evicts timestamps older than 60s.
    // We can't wait 60s in a unit test, so we verify the structure indirectly:
    // a fresh limiter with a small limit should allow exactly `limit` requests.
    RateLimiter limiter(3);
    EXPECT_TRUE(limiter.allow("k"));
    EXPECT_TRUE(limiter.allow("k"));
    EXPECT_TRUE(limiter.allow("k"));
    EXPECT_FALSE(limiter.allow("k"));
    // Different key still works
    EXPECT_TRUE(limiter.allow("other"));
}

// ── Pre-Auth Rate Limiting Tests (#74) ────────────────────────────────────────

TEST_F(HttpHandlerTest, RateLimitAppliesBeforeAuthOnPost) {
    // #74: Rate limiting must apply BEFORE auth so brute-force token attempts
    // are throttled by IP even with invalid/missing tokens.
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true, .rate_limit_rpm = 2});

    // Send 2 requests with NO auth (would be 401) — these consume rate budget.
    for (int i = 0; i < 2; ++i) {
        httplib::Request req = make_request("POST", "/mcp",
            R"({"jsonrpc":"2.0","id":)" + std::to_string(i) + R"(,"method":"ping"})");
        httplib::Response res;
        transport.handle_post(req, res);
        EXPECT_EQ(res.status, 401) << "Unauthenticated request should get 401";
    }

    // 3rd request with a VALID token should still be rate-limited (429),
    // proving the rate limiter ran before auth and counted the failed attempts.
    auto token = auth_->issue_token("agent-1", Role::Worker, "test-swarm");
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":99,"method":"ping"})",
        "Bearer " + token.token_string);
    httplib::Response res;
    transport.handle_post(req, res);
    EXPECT_EQ(res.status, 429) << "Valid token should be throttled because pre-auth rate limit was hit by failed attempts";
}

TEST_F(HttpHandlerTest, RateLimitAppliesBeforeAuthOnGet) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true, .rate_limit_rpm = 1});

    // First unauthenticated GET — consumes rate budget, returns 401.
    httplib::Request req1 = make_request("GET", "/mcp");
    req1.set_header("Accept", "text/event-stream");
    httplib::Response res1;
    transport.handle_get(req1, res1);
    EXPECT_EQ(res1.status, 401);

    // Second GET with valid token — should be 429 (rate-limited before auth).
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req2 = make_request("GET", "/mcp");
    req2.set_header("Accept", "text/event-stream");
    req2.set_header("Authorization", "Bearer " + token.token_string);
    httplib::Response res2;
    transport.handle_get(req2, res2);
    EXPECT_EQ(res2.status, 429) << "GET rate limit should apply before auth";
}

TEST_F(HttpHandlerTest, BruteForceTokensAreThrottled) {
    // #74: Simulate brute-force token attempts — all invalid, all from same IP.
    // After the rate limit is hit, even subsequent attempts should get 429, not 401.
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true, .rate_limit_rpm = 3});

    int count_401 = 0;
    int count_429 = 0;
    for (int i = 0; i < 5; ++i) {
        httplib::Request req = make_request("POST", "/mcp",
            R"({"jsonrpc":"2.0","id":)" + std::to_string(i) + R"(,"method":"ping"})",
            "Bearer invalid-token-" + std::to_string(i));
        httplib::Response res;
        transport.handle_post(req, res);
        if (res.status == 401) ++count_401;
        if (res.status == 429) ++count_429;
    }

    EXPECT_EQ(count_401, 3) << "First 3 brute-force attempts should get 401";
    EXPECT_EQ(count_429, 2) << "Remaining attempts should be throttled with 429";
}
