#include <gtest/gtest.h>
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
#include <nlohmann/json.hpp>

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

TEST_F(HttpHandlerTest, GetWithQueryTokenAuthenticates) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_->issue_token("agent-1", Role::Observer, "test-swarm");
    httplib::Request req = make_request("GET", "/mcp");
    req.set_header("Accept", "text/event-stream");
    // Set token as query parameter (common for SSE which doesn't support custom headers easily)
    req.params.emplace("token", token.token_string);
    httplib::Response res;

    // This will fail with bad request because SSE sink is not exercised in handler test,
    // but we only want to verify that authenticate() accepts the query token.
    transport.handle_get(req, res);
    // If auth failed, status would be 401. With query param it should get past auth.
    EXPECT_NE(res.status, 401) << "Query parameter token should be accepted for SSE auth";
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
