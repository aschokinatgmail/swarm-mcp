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
