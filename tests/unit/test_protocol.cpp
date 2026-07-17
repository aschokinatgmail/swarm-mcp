#include <gtest/gtest.h>
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"

using namespace mcp_collab;

class McpProtocolTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test-server", .version = "0.1.0"}};
};

TEST_F(McpProtocolTest, Initialize) {
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test-client"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_EQ(resp["result"]["protocolVersion"], "2025-03-26");
    EXPECT_EQ(resp["result"]["serverInfo"]["name"], "test-server");
    EXPECT_TRUE(resp["result"]["capabilities"].contains("tools"));
    EXPECT_TRUE(resp["result"]["capabilities"].contains("resources"));
    EXPECT_TRUE(resp["result"]["capabilities"].contains("prompts"));
}

TEST_F(McpProtocolTest, InvalidJsonRpcVersion) {
    json req = {{"jsonrpc", "1.0"}, {"id", 1}, {"method", "initialize"}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(McpProtocolTest, MethodNotFound) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "nonexistent/method"}, {"params", {}}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32601);
}

TEST_F(McpProtocolTest, ServerNotInitialized) {
    json req = {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}, {"params", {}}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32002);
}

TEST_F(McpProtocolTest, RegisterAndListTools) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_tool({.name = "test_tool", .description = "A test tool",
        .input_schema = {{"type", "object"}}, .required_permission = Permission::TaskRead},
        [](const json&) -> json { return {{"ok", true}}; });
    json req = {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/list"}, {"params", {}}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["tools"].size(), 1u);
    EXPECT_EQ(resp["result"]["tools"][0]["name"], "test_tool");
}

TEST_F(McpProtocolTest, CallTool) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_tool({.name = "add", .description = "Add numbers",
        .input_schema = {{"type", "object"}}, .required_permission = Permission::TaskRead},
        [](const json& args) -> json { return args.value("a", 0) + args.value("b", 0); });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"add","arguments":{"a":3,"b":4},"_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["isError"], false);
}

TEST_F(McpProtocolTest, CallToolInsufficientPermission) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_tool({.name = "dangerous", .description = "Admin only",
        .input_schema = {{"type", "object"}}, .required_permission = Permission::Admin},
        [](const json&) -> json { return {{"secret", true}}; });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"dangerous","_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32003);
}

TEST_F(McpProtocolTest, CallNonexistentTool) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"nonexistent","_auth":{"role":"worker"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(McpProtocolTest, RegisterAndListResources) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_resource({.uri = "swarm://test", .name = "Test Resource", .description = "desc"},
        [](const json&) -> json { return {{"data", 42}}; });
    json req = {{"jsonrpc", "2.0"}, {"id", 8}, {"method", "resources/list"}, {"params", {}}};
    auto resp = proto.handle_request(req);
    EXPECT_EQ(resp["result"]["resources"].size(), 1u);
}

TEST_F(McpProtocolTest, ReadResource) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_resource({.uri = "swarm://data", .name = "Data", .description = "desc",
        .required_permission = Permission::TaskRead},
        [](const json&) -> json { return {{"value", 99}}; });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":9,"method":"resources/read","params":{"uri":"swarm://data","_auth":{"role":"worker"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(McpProtocolTest, ReadResourceInsufficientPermission) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_resource({.uri = "swarm://secret", .name = "Secret", .description = "desc",
        .required_permission = Permission::Admin},
        [](const json&) -> json { return {{"secret", 1}}; });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":10,"method":"resources/read","params":{"uri":"swarm://secret","_auth":{"role":"observer"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(McpProtocolTest, RegisterAndListPrompts) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_prompt({.name = "greet", .description = "Greeting prompt"}, [](const json&) -> json {
        return json::array({{{"role", "user"}, {"content", "Hello"}}});
    });
    json req = {{"jsonrpc", "2.0"}, {"id", 11}, {"method", "prompts/list"}, {"params", {}}};
    auto resp = proto.handle_request(req);
    EXPECT_EQ(resp["result"]["prompts"].size(), 1u);
}

TEST_F(McpProtocolTest, GetPrompt) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_prompt({.name = "greet2", .description = "Greeting"}, [](const json& args) -> json {
        return json::array({{{"role", "user"}, {"content", "Hi " + args.value("name", "")}}});
    });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":12,"method":"prompts/get","params":{"name":"greet2","arguments":{"name":"World"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(McpProtocolTest, Ping) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    json req = {{"jsonrpc", "2.0"}, {"id", 13}, {"method", "ping"}, {"params", {}}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"].is_object());
}

TEST_F(McpProtocolTest, NotificationNoId) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    json req = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}, {"params", {}}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.is_object());
}

TEST_F(McpProtocolTest, HandleRaw) {
    std::string raw = R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})";
    auto result = proto.handle_raw(raw);
    EXPECT_FALSE(result.empty());
    auto parsed = json::parse(result);
    EXPECT_TRUE(parsed.contains("result"));
}

TEST_F(McpProtocolTest, HandleRawInvalidJson) {
    auto result = proto.handle_raw("not json at all");
    auto parsed = json::parse(result);
    EXPECT_EQ(parsed["error"]["code"], -32700);
}

TEST_F(McpProtocolTest, ToolHandlerException) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_tool({.name = "crasher", .description = "Crashes", .input_schema = {},
        .required_permission = Permission::TaskRead},
        [](const json&) -> json { throw std::runtime_error("boom"); });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"crasher","_auth":{"role":"worker"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp["result"]["isError"] == true);
}

TEST_F(McpProtocolTest, AuthContextExtracted) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    std::string captured_agent;
    proto.register_tool({.name = "auth_test", .description = "auth test", .input_schema = {},
        .required_permission = Permission::TaskRead},
        [&](const json& args) -> json { captured_agent = args.value("_auth", json::object()).value("agent_id", "none"); return {}; });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"auth_test","_auth":{"agent_id":"agent-42","role":"worker","swarm_id":"s1"}}})"
    );
    proto.handle_request(req);
    EXPECT_EQ(captured_agent, "agent-42");
}

// Auth-gate regression tests for resources/subscribe, prompts/list, prompts/get.
// All roles hold TaskRead, so the -32003 rejection branch is unreachable via the
// public JSON path; tests verify the guard does not break legitimate access.

TEST_F(McpProtocolTest, SubscribeResourceWithAuth) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":16,"method":"resources/subscribe","params":{"uri":"swarm://test","_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["subscribed"], true);
}

TEST_F(McpProtocolTest, SubscribeResourceWithDefaultRole) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":17,"method":"resources/subscribe","params":{"uri":"swarm://test"}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["subscribed"], true);
}

TEST_F(McpProtocolTest, PromptsListWithAuth) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_prompt({.name = "auth_greet", .description = "Greeting prompt"}, [](const json&) -> json {
        return json::array({{{"role", "user"}, {"content", "Hello"}}});
    });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":18,"method":"prompts/list","params":{"_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_EQ(resp["result"]["prompts"].size(), 1u);
    EXPECT_EQ(resp["result"]["prompts"][0]["name"], "auth_greet");
}

TEST_F(McpProtocolTest, PromptsGetWithAuth) {
    proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    proto.register_prompt({.name = "auth_greet2", .description = "Greeting"}, [](const json& args) -> json {
        return json::array({{{"role", "user"}, {"content", "Hi " + args.value("name", "")}}});
    });
    json req = json::parse(
        R"({"jsonrpc":"2.0","id":19,"method":"prompts/get","params":{"name":"auth_greet2","arguments":{"name":"World"},"_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})"
    );
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"].contains("messages"));
}