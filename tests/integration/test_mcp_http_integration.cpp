#include <gtest/gtest.h>
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/git_operations.hpp"
#include "mcp_collab/branch_manager.hpp"
#include "mcp_collab/merge_coordinator.hpp"
#include "mcp_collab/mqtt_client.hpp"
#include "mcp_collab/channel.hpp"
#include "mcp_collab/collab_defs.hpp"
#include <httplib.h>
#include <thread>
#include <chrono>

using namespace mcp_collab;

class McpHttpIntegrationTest : public ::testing::Test {
protected:
    std::string server_host = "127.0.0.1";
    uint16_t server_port = 19876;
    McpProtocol proto{ServerInfo{.name = "test-server", .version = "1.0.0"}};
    AuthProvider auth{"test-secret"};
    TaskManager tasks;
    AgentRegistry agents{"test-swarm"};
    ContextStore context;
    EventBus events;
    SecureMqttClient mqtt{MqttConfig{.host = "localhost", .port = 18839}, "test-swarm", "test-secret"};
    GitOperations git{"."};
    BranchManager branches{git, "collab/"};
    MergeCoordinator merges{git, branches};
    ChannelManager channels{mqtt, "mcp-collab", "test-swarm"};
    std::unique_ptr<StreamableHttpTransport> transport;

    void SetUp() override {
        register_collab_tools(proto, tasks, agents, context, events, branches, merges, mqtt.raw_client(), channels);
        register_collab_resources(proto, tasks, agents, context, events);
        register_collab_prompts(proto);

        transport = std::make_unique<StreamableHttpTransport>(proto, auth, StreamableHttpConfig{
            .host = server_host,
            .port = server_port,
            .endpoint = "/mcp",
            .require_auth = false,  // no auth for basic integration tests
        });
        transport->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        transport->stop();
    }

    json send_request(const json& req) {
        httplib::Client client(server_host, server_port);
        auto res = client.Post("/mcp", req.dump(), "application/json");
        EXPECT_TRUE(res);
        return json::parse(res->body);
    }
};

TEST_F(McpHttpIntegrationTest, InitializeAndListTools) {
    json init_req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {}}};
    auto init_resp = send_request(init_req);
    EXPECT_EQ(init_resp["result"]["protocolVersion"], "2025-03-26");

    json tools_req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", {}}};
    auto tools_resp = send_request(tools_req);
    EXPECT_GE(tools_resp["result"]["tools"].size(), 15u);
}

TEST_F(McpHttpIntegrationTest, CreateAndListTasks) {
    send_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));

    json create_req = json::parse(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"task_create","arguments":{"title":"Integration test","creator":"test-agent"},"_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})"
    );
    auto create_resp = send_request(create_req);
    EXPECT_TRUE(create_resp.contains("result"));
    EXPECT_TRUE(create_resp["result"]["content"][0]["text"].is_string());

    json list_req = json::parse(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"task_list","arguments":{},"_auth":{"role":"observer"}}})"
    );
    auto list_resp = send_request(list_req);
    EXPECT_TRUE(list_resp.contains("result"));
}

TEST_F(McpHttpIntegrationTest, ContextSetAndGet) {
    send_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));

    json set_req = json::parse(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"context_set","arguments":{"key":"test.key","value":42},"_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})"
    );
    send_request(set_req);

    json get_req = json::parse(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"context_get","arguments":{"key":"test.key"},"_auth":{"role":"worker"}}})"
    );
    auto get_resp = send_request(get_req);
    EXPECT_TRUE(get_resp.contains("result"));
}

TEST_F(McpHttpIntegrationTest, ResourcesList) {
    send_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));

    json res_req = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "resources/list"}, {"params", {}}};
    auto res_resp = send_request(res_req);
    EXPECT_GE(res_resp["result"]["resources"].size(), 5u);
}

TEST_F(McpHttpIntegrationTest, PromptsList) {
    send_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));

    json prompts_req = {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "prompts/list"}, {"params", {}}};
    auto prompts_resp = send_request(prompts_req);
    EXPECT_GE(prompts_resp["result"]["prompts"].size(), 3u);
}

TEST_F(McpHttpIntegrationTest, AuthRequiredBlocksUnauthorized) {
    AuthProvider secure_auth{"my-secret"};
    McpProtocol secure_proto{ServerInfo{.name = "secure", .version = "1.0.0"}};
    StreamableHttpTransport secure_transport(secure_proto, secure_auth, StreamableHttpConfig{
        .host = server_host, .port = static_cast<uint16_t>(server_port + 1), .endpoint = "/mcp", .require_auth = true,
    });
    register_collab_tools(secure_proto, tasks, agents, context, events, branches, merges, mqtt.raw_client(), channels);
    register_collab_resources(secure_proto, tasks, agents, context, events);
    secure_transport.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    httplib::Client client(server_host, server_port + 1);
    auto res = client.Post("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})", "application/json");
    EXPECT_TRUE(res);
    EXPECT_EQ(res->status, 401);

    auto token = secure_auth.issue_token("agent-1", Role::Worker, "test-swarm");
    auto auth_res = client.Post("/mcp",
        httplib::Headers{{"Authorization", "Bearer " + token.token_id + "." + "placeholder"}},
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})",
        "application/json");
    secure_transport.stop();
}

TEST_F(McpHttpIntegrationTest, PingResponds) {
    send_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    json ping_req = {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "ping"}, {"params", {}}};
    auto ping_resp = send_request(ping_req);
    EXPECT_TRUE(ping_resp.contains("result"));
}

TEST_F(McpHttpIntegrationTest, ToolAuthEnforcement) {
    AuthProvider secure_auth{"auth-enforcement-secret"};
    McpProtocol secure_proto{ServerInfo{.name = "secure", .version = "1.0.0"}};
    StreamableHttpTransport secure_transport(secure_proto, secure_auth, StreamableHttpConfig{
        .host = server_host, .port = static_cast<uint16_t>(server_port + 2), .endpoint = "/mcp", .require_auth = true,
    });
    register_collab_tools(secure_proto, tasks, agents, context, events, branches, merges, mqtt.raw_client(), channels);
    register_collab_resources(secure_proto, tasks, agents, context, events);
    secure_transport.start();

    auto token = secure_auth.issue_token("obs", Role::Observer, "s1");
    httplib::Client client(server_host, server_port + 2);
    auto init_res = client.Post("/mcp",
        httplib::Headers{{"Authorization", "Bearer " + token.token_string}},
        R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})",
        "application/json");
    ASSERT_TRUE(init_res);

    json req = json::parse(
        R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"task_create","arguments":{"title":"Blocked","creator":"obs"}}})"
    );
    auto res = client.Post("/mcp",
        httplib::Headers{{"Authorization", "Bearer " + token.token_string}},
        req.dump(),
        "application/json");
    secure_transport.stop();

    ASSERT_TRUE(res);
    auto resp = json::parse(res->body);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"].value("isError", false)));
}

TEST_F(McpHttpIntegrationTest, BatchRequest) {
    json batch = json::array({
        {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}, {"params", {}}},
    });
    httplib::Client client(server_host, server_port);
    auto res = client.Post("/mcp", batch.dump(), "application/json");
    EXPECT_TRUE(res);
    auto parsed = json::parse(res->body);
    EXPECT_TRUE(parsed.is_array());
    EXPECT_EQ(parsed.size(), 1u);
}
