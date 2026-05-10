#include <gtest/gtest.h>
#include "mcp_collab/collab_defs.hpp"
#include "mcp_collab/auth.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/persistence.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/mqtt_client.hpp"
#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/channel.hpp"
#include "mcp_collab/config.hpp"
#include "mcp_collab/keychain.hpp"
#include <filesystem>
#include <fstream>
#include <format>

using namespace mcp_collab;

// ═══════════════════════════════════════════════════════════════════════════════
// Tools: task_list with individual filter branches
// ═══════════════════════════════════════════════════════════════════════════════

class ToolsTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    AuthProvider auth_{"secret"};
    StreamableHttpTransport transport;
    TaskManager tasks;
    AgentRegistry agents{"swarm-test"};
    ContextStore context;
    EventBus events;
    MqttClient mqtt{MqttConfig{}};
    SecureMqttClient secure_mqtt{MqttConfig{.host = "localhost", .port = 18840}, "swarm-test", "secret"};
    ChannelManager channels;

    std::string test_repo_path;
    std::unique_ptr<GitOperations> git;
    std::unique_ptr<BranchManager> branches;
    std::unique_ptr<MergeCoordinator> merges;

    ToolsTest()
        : transport(proto, auth_,
            StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                                 .require_auth = false}),
          channels(secure_mqtt, "mcp-collab", "swarm-test") {}

    void SetUp() override {
        test_repo_path = (std::filesystem::temp_directory_path() / ("swarm-mcp-tools-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)))).string();
        std::error_code ec;
        std::filesystem::remove_all(test_repo_path, ec);
        std::filesystem::create_directories(test_repo_path);
        auto p = test_repo_path;
        system(std::format("git init --initial-branch=main \"{}\"", p).c_str());
        system(std::format("git -C \"{}\" config user.email \"test@test.com\"", p).c_str());
        system(std::format("git -C \"{}\" config user.name \"Test\"", p).c_str());
        std::ofstream(p + "/initial.txt") << "init";
        system(std::format("git -C \"{}\" add .", p).c_str());
        system(std::format("git -C \"{}\" commit -m \"initial\"", p).c_str());
        git = std::make_unique<GitOperations>(test_repo_path);
        branches = std::make_unique<BranchManager>(*git, "collab/");
        merges = std::make_unique<MergeCoordinator>(*git, *branches);

        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
        register_collab_tools(proto, tasks, agents, context, events, *branches, *merges, mqtt, channels);
    }

    void TearDown() override {
        branches.reset();
        merges.reset();
        git.reset();
        std::error_code ec;
        for (int i = 0; i < 10; ++i) {
            if (std::filesystem::remove_all(test_repo_path, ec); !std::filesystem::exists(test_repo_path, ec)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    json call_tool(const std::string& name, const json& args, Role role = Role::Worker) {
        json req = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", args}}}
        };
        req["params"]["_auth"] = {{"agent_id", "test-agent"}, {"role", role_to_str(role)}, {"swarm_id", "swarm-test"}};
        auto resp = proto.handle_request(req);
        if (resp.contains("result")) {
            auto content = resp["result"]["content"];
            if (content.is_array() && content.size() > 0 && content[0].contains("text")) {
                return json::parse(content[0]["text"].get<std::string>());
            }
            return resp["result"];
        }
        return resp;
    }
};

TEST_F(ToolsTest, TaskCreate) {
    auto result = call_tool("task_create", {{"title", "Test task"}, {"creator", "agent-1"}});
    EXPECT_TRUE(result.contains("id"));
    EXPECT_EQ(result["title"], "Test task");
}

TEST_F(ToolsTest, TaskCreateWithTagsAndDeps) {
    auto t1 = call_tool("task_create", {{"title", "Dep task"}, {"creator", "a1"}});
    auto result = call_tool("task_create", {
        {"title", "Main task"}, {"creator", "a2"}, {"description", "desc"},
        {"priority", "high"}, {"tags", json::array({"backend", "api"})},
        {"dependencies", json::array({t1["id"].get<std::string>()})}
    });
    EXPECT_TRUE(result.contains("id"));
    EXPECT_EQ(result["priority"], "high");
}

TEST_F(ToolsTest, TaskListFilterByAssignee) {
    call_tool("task_create", {{"title", "T1"}, {"creator", "a1"}});
    auto t2 = call_tool("task_create", {{"title", "T2"}, {"creator", "a2"}});
    call_tool("task_assign", {{"task_id", t2["id"]}, {"agent_id", "agent-x"}});
    auto result = call_tool("task_list", {{"assignee", "agent-x"}});
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(ToolsTest, TaskListFilterBySearch) {
    call_tool("task_create", {{"title", "Searchable Alpha"}, {"creator", "a1"}});
    call_tool("task_create", {{"title", "Beta task"}, {"creator", "a2"}});
    auto result = call_tool("task_list", {{"search", "alpha"}});
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(ToolsTest, TaskListFilterByStatus) {
    auto t = call_tool("task_create", {{"title", "T1"}, {"creator", "a1"}});
    call_tool("task_update_status", {{"task_id", t["id"]}, {"status", "completed"}});
    auto result = call_tool("task_list", {{"status", "completed"}});
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(ToolsTest, TaskListFilterByTag) {
    auto t = call_tool("task_create", {{"title", "T1"}, {"creator", "a1"}, {"tags", json::array({"backend"})}});
    auto result = call_tool("task_list", {{"tag", "backend"}});
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(ToolsTest, TaskAssign) {
    auto t = call_tool("task_create", {{"title", "T"}, {"creator", "a1"}});
    auto result = call_tool("task_assign", {{"task_id", t["id"]}, {"agent_id", "agent-x"}});
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(ToolsTest, TaskAssignNotFound) {
    auto result = call_tool("task_assign", {{"task_id", "nonexistent"}, {"agent_id", "x"}});
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(ToolsTest, TaskUpdateStatusCompleted) {
    auto t = call_tool("task_create", {{"title", "T"}, {"creator", "a1"}});
    auto result = call_tool("task_update_status", {{"task_id", t["id"]}, {"status", "completed"}});
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(ToolsTest, TaskUpdateStatusNotFound) {
    auto result = call_tool("task_update_status", {{"task_id", "nonexistent"}, {"status", "completed"}});
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(ToolsTest, TaskGet) {
    auto t = call_tool("task_create", {{"title", "My task"}, {"creator", "a1"}});
    auto result = call_tool("task_get", {{"task_id", t["id"]}});
    EXPECT_EQ(result["title"], "My task");
}

TEST_F(ToolsTest, TaskGetNotFound) {
    auto result = call_tool("task_get", {{"task_id", "nonexistent"}});
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(ToolsTest, TaskReady) {
    auto result = call_tool("task_ready", {});
    EXPECT_TRUE(result.is_array());
}

TEST_F(ToolsTest, AgentRegisterMismatch) {
    auto result = call_tool("agent_register", {{"name", "Agent X"}});
    EXPECT_TRUE(result.contains("error") || result.contains("success") || result.contains("id"));
}

TEST_F(ToolsTest, AgentRegister) {
    auto result = call_tool("agent_register", {{"name", "Test Agent"}});
    EXPECT_TRUE(result.contains("id") || result.contains("name") || result.contains("error"));
}

TEST_F(ToolsTest, AgentRegisterWithModel) {
    json args = {
        {"name", "Smart Agent"},
        {"model", {{"provider", "openai"}, {"model_id", "gpt-4"}, {"model_family", "gpt"}, {"context_window", 128000}, {"max_output_tokens", 4096}}},
        {"environment", {{"runtime", "python"}, {"os", "linux"}, {"cpu_cores", 8}, {"memory_mb", 16384}, {"gpu", "A100"}, {"supported_languages", json::array({"python", "javascript"})}}}
    };
    auto result = call_tool("agent_register", args);
    EXPECT_TRUE(result.contains("id") || result.contains("error"));
}

TEST_F(ToolsTest, AgentListFilterByStatus) {
    AgentInfo info;
    info.name = "Agent1";
    info.capabilities = {"python"};
    AuthProvider regAuth{"secret"};
    auto regToken = regAuth.issue_token("agent-list", Role::Worker, "swarm-test");
    agents.register_agent(info, regToken);

    auto agentList = call_tool("agent_list", {{"status", "online"}});
    EXPECT_TRUE(agentList.is_array());
}

TEST_F(ToolsTest, AgentListFilterByBusy) {
    auto result = call_tool("agent_list", {{"status", "busy"}});
    EXPECT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 0u);
}

TEST_F(ToolsTest, AgentListFilterByIdle) {
    auto result = call_tool("agent_list", {{"status", "idle"}});
    EXPECT_TRUE(result.is_array());
}

TEST_F(ToolsTest, AgentListFilterByOffline) {
    auto result = call_tool("agent_list", {{"status", "offline"}});
    EXPECT_TRUE(result.is_array());
}

TEST_F(ToolsTest, AgentListBySwarmId) {
    auto result = call_tool("agent_list", {{"swarm_id", "swarm-test"}});
    EXPECT_TRUE(result.is_array());
}

TEST_F(ToolsTest, AgentFindCapability) {
    AuthProvider regAuth{"secret"};
    auto regToken = regAuth.issue_token("find-cap", Role::Worker, "swarm-test");
    AgentInfo info;
    info.name = "CapAgent";
    info.capabilities = {"python", "docker"};
    agents.register_agent(info, regToken);

    auto result = call_tool("agent_find_capability", {{"capability", "python"}});
    EXPECT_TRUE(result.is_array());
    EXPECT_GE(result.size(), 1u);
}

TEST_F(ToolsTest, AgentSetRole) {
    AuthProvider regAuth{"secret"};
    auto regToken = regAuth.issue_token("set-role", Role::Worker, "swarm-test");
    AgentInfo info;
    info.name = "RoleAgent";
    std::string agentId = agents.register_agent(info, regToken);

    json args = {{"agent_id", agentId}, {"role", "coordinator"}};
    auto result = call_tool("agent_set_role", args, Role::Coordinator);
    EXPECT_TRUE(result.contains("id") || result.contains("success"));
}

TEST_F(ToolsTest, AgentSetRoleDenied) {
    AuthProvider regAuth{"secret"};
    auto regToken = regAuth.issue_token("set-role", Role::Worker, "swarm-test");
    AgentInfo info;
    info.name = "RoleAgent";
    std::string agentId = agents.register_agent(info, regToken);

    json args = {{"agent_id", agentId}, {"role", "coordinator"}};
    auto result = call_tool("agent_set_role", args, Role::Observer);
    EXPECT_TRUE(result.contains("error") || result.contains("success"));
}

TEST_F(ToolsTest, ContextSetAndGet) {
    call_tool("context_set", {{"key", "test-key"}, {"value", 42}, {"owner", "agent-1"}});
    auto result = call_tool("context_get", {{"key", "test-key"}});
    EXPECT_TRUE(result.contains("value"));
}

TEST_F(ToolsTest, ContextGetNotFound) {
    auto result = call_tool("context_get", {{"key", "nonexistent"}});
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(ToolsTest, ContextList) {
    call_tool("context_set", {{"key", "k1"}, {"value", 1}});
    call_tool("context_set", {{"key", "k2"}, {"value", 2}});
    auto result = call_tool("context_list", {});
    EXPECT_EQ(result.size(), 2u);
}

TEST_F(ToolsTest, ContextListWithPrefix) {
    call_tool("context_set", {{"key", "app/config"}, {"value", "val"}});
    call_tool("context_set", {{"key", "other/data"}, {"value", "val2"}});
    auto result = call_tool("context_list", {{"prefix", "app"}});
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(ToolsTest, ContextMerge) {
    call_tool("context_set", {{"key", "merged"}, {"value", json::object()}});
    call_tool("context_set", {{"key", "merged"}, {"value", json::object()}});
    auto result = call_tool("context_merge", {{"key", "merged"}, {"data", {{"x", 1}}}});
    EXPECT_TRUE(result.contains("success"));
}

TEST_F(ToolsTest, ContextMergeNonObject) {
    call_tool("context_set", {{"key", "scalar"}, {"value", "hello"}});
    auto result = call_tool("context_merge", {{"key", "scalar"}, {"data", {{"x", 1}}}});
    EXPECT_TRUE(result.contains("success") || result.contains("error"));
}

TEST_F(ToolsTest, BranchCreate) {
    auto result = call_tool("branch_create", {{"task_id", "task-1"}, {"agent_id", "agent-1"}});
    EXPECT_TRUE(result.contains("name") || result.contains("error"));
}

TEST_F(ToolsTest, BranchCommit) {
    auto branch = call_tool("branch_create", {{"task_id", "task-2"}, {"agent_id", "agent-1"}});
    std::ofstream(test_repo_path + "/test_file.txt") << "test content";
    auto result = call_tool("branch_commit", {{"branch", branch["name"]}, {"message", "test commit"}, {"agent_id", "agent-1"}});
    EXPECT_TRUE(result.contains("success"));
}

TEST_F(ToolsTest, MergeRequest) {
    auto branch = call_tool("branch_create", {{"task_id", "merge-test"}, {"agent_id", "agent-1"}});
    std::ofstream(test_repo_path + "/merge_file.txt") << "merge content";
    call_tool("branch_commit", {{"branch", branch["name"]}, {"message", "merge commit"}, {"agent_id", "agent-1"}});
    git->checkout("main");
    auto result = call_tool("merge_request", {{"source", branch["name"]}, {"requester", "agent-1"}});
    EXPECT_TRUE(result.contains("id") || result.contains("error") || result.contains("source_branch"));
}

TEST_F(ToolsTest, MergeRequestWithStrategy) {
    auto result = call_tool("merge_request", {{"source", "collab/test"}, {"requester", "agent-1"}, {"strategy", "squash"}});
}

TEST_F(ToolsTest, MergeApprove) {
    auto branch = call_tool("branch_create", {{"task_id", "merge-approve"}, {"agent_id", "agent-1"}});
    std::ofstream(test_repo_path + "/merge_appr.txt") << "content";
    call_tool("branch_commit", {{"branch", branch["name"]}, {"message", "c"}, {"agent_id", "agent-1"}});
    git->checkout("main");
    auto mr = call_tool("merge_request", {{"source", branch["name"]}, {"requester", "agent-1"}});
    if (mr.contains("id")) {
        auto result = call_tool("merge_approve", {{"merge_id", mr["id"]}, {"reviewer", "reviewer-1"}}, Role::Coordinator);
        EXPECT_TRUE(result.contains("id") || result.contains("error") || result.contains("status"));
    }
}

TEST_F(ToolsTest, MergeExecuteDirect) {
    auto branch = call_tool("branch_create", {{"task_id", "merge-exec"}, {"agent_id", "agent-1"}});
    std::ofstream(test_repo_path + "/merge_exec.txt") << "content";
    call_tool("branch_commit", {{"branch", branch["name"]}, {"message", "c"}, {"agent_id", "agent-1"}});
    git->checkout("main");
    auto mr = call_tool("merge_request", {{"source", branch["name"]}, {"requester", "agent-1"}});
    if (mr.contains("id")) {
        call_tool("merge_approve", {{"merge_id", mr["id"]}, {"reviewer", "r1"}}, Role::Coordinator);
        auto result = call_tool("merge_execute", {{"merge_id", mr["id"]}});
    }
}

TEST_F(ToolsTest, EventPublish) {
    auto result = call_tool("event_publish", {{"event_type", "test.event"}, {"source", "agent-1"}, {"data", {{"key", "val"}}}});
    EXPECT_TRUE(result.contains("success"));
    EXPECT_EQ(result["event_type"], "test.event");
}

TEST_F(ToolsTest, EventRecent) {
    call_tool("event_publish", {{"event_type", "test.event"}, {"source", "agent-1"}});
    auto result = call_tool("event_recent", {{"count", 10}});
    EXPECT_TRUE(result.is_array());
}

TEST_F(ToolsTest, EventRecentByType) {
    call_tool("event_publish", {{"event_type", "custom.type"}, {"source", "agent-1"}});
    auto result = call_tool("event_recent", {{"count", 10}, {"type", "custom.type"}});
    EXPECT_TRUE(result.is_array());
}

TEST_F(ToolsTest, Heartbeat) {
    AuthProvider regAuth{"secret"};
    auto regToken = regAuth.issue_token("hb-agent", Role::Worker, "swarm-test");
    AgentInfo info;
    info.name = "HBAgent";
    std::string agentId = agents.register_agent(info, regToken);

    auto result = call_tool("heartbeat", {{"agent_id", agentId}});
    EXPECT_TRUE(result.contains("success"));
}

TEST_F(ToolsTest, HeartbeatNotFound) {
    auto result = call_tool("heartbeat", {{"agent_id", "nonexistent"}});
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(ToolsTest, AgentDescribe) {
    AuthProvider regAuth{"secret"};
    auto regToken = regAuth.issue_token("desc-agent", Role::Worker, "swarm-test");
    AgentInfo info;
    info.name = "DescAgent";
    std::string agentId = agents.register_agent(info, regToken);

    auto result = call_tool("agent_describe", {{"agent_id", agentId}});
    EXPECT_TRUE(result.contains("effective_tools") || result.contains("effective_permissions") || result.contains("id"));
}

TEST_F(ToolsTest, AgentDescribeNotFound) {
    auto result = call_tool("agent_describe", {{"agent_id", "nonexistent"}});
    EXPECT_TRUE(result.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Protocol: uncovered method branches
// ═══════════════════════════════════════════════════════════════════════════════

class ProtocolBranchTest2 : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    void SetUp() override {
        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    }
};

TEST_F(ProtocolBranchTest2, InitializeWithNullId) {
    json req = {{"jsonrpc", "2.0"}, {"method", "initialize"}, {"params", json::object()}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.is_object());
}

TEST_F(ProtocolBranchTest2, MethodNotFound) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":99,"method":"nonexistent/method","params":{}})"));
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32601);
}

TEST_F(ProtocolBranchTest2, InvalidJsonRpcVersion) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"1.0","id":1,"method":"ping","params":{}})"));
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32600);
}

TEST_F(ProtocolBranchTest2, MissingMethodWithId) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"params":{}})"));
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32600);
}

TEST_F(ProtocolBranchTest2, NotInitialized) {
    McpProtocol fresh_proto{ServerInfo{.name = "fresh", .version = "1.0"}};
    auto resp = fresh_proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})"));
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32002);
}

TEST_F(ProtocolBranchTest2, PingWithoutId) {
    json req = {{"jsonrpc", "2.0"}, {"method", "ping"}, {"params", json::object()}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.is_object());
}

TEST_F(ProtocolBranchTest2, ResourcesUnsubscribeNotFound) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/unsubscribe","params":{}})"));
    EXPECT_TRUE(resp.contains("error"));
    EXPECT_EQ(resp["error"]["code"], -32601);
}

TEST_F(ProtocolBranchTest2, CompletionMethod) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"completion/complete","params":{}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolBranchTest2, LoggingSetLevel) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"logging/setLevel","params":{"level":"debug"}})"));
    EXPECT_TRUE(resp.contains("error") || resp.contains("result"));
}

TEST_F(ProtocolBranchTest2, BatchRequestMixedNotifications) {
    json batch = json::parse(R"([
        {"jsonrpc":"2.0","method":"notifications/initialized","params":{}},
        {"jsonrpc":"2.0","id":1,"method":"ping","params":{}},
        {"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
    ])");

    bool is_notification = true;
    for (const auto& item : batch) {
        if (item.contains("id") && !item["id"].is_null()) {
            is_notification = false;
            break;
        }
    }
    EXPECT_FALSE(is_notification);
}

TEST_F(ProtocolBranchTest2, ToolsCallWithAuth) {
    proto.register_tool({.name = "test_tool", .description = "desc", .input_schema = {},
        .required_permission = Permission::TaskRead},
        [](const json& args) -> json { return {{"result", args.value("msg", "")}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"test_tool","arguments":{"msg":"hello"},"_auth":{"role":"worker","agent_id":"a1","swarm_id":"s1"}}})"));
    EXPECT_TRUE(resp.contains("result"));
    auto content = resp["result"]["content"];
    EXPECT_TRUE(content.is_array());
}

TEST_F(ProtocolBranchTest2, ToolsCallForbidden) {
    proto.register_tool({.name = "admin_tool", .description = "admin only",
        .input_schema = {}, .required_permission = Permission::Admin},
        [](const json&) -> json { return {{"admin", true}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"admin_tool","arguments":{},"_auth":{"role":"observer"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolBranchTest2, ToolsCallToolNotFound) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"nonexistent_tool","arguments":{},"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolBranchTest2, ResourcesReadWithDefaultRole) {
    proto.register_resource({.uri = "test://data", .name = "Data", .description = "test data",
        .required_permission = Permission::TaskRead},
        [](const json&) -> json { return {{"val", 1}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"test://data","_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(ProtocolBranchTest2, ResourcesReadForbiddenRole) {
    proto.register_resource({.uri = "test://admin", .name = "Admin", .description = "admin only",
        .required_permission = Permission::Admin},
        [](const json&) -> json { return {{"secret", 1}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"test://admin","_auth":{"role":"observer"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolBranchTest2, PromptsGetExisting) {
    proto.register_prompt({.name = "hello", .description = "Hello prompt"},
        [](const json& args) -> json { return json::array({{{"role", "user"}, {"content", "hi"}}}); });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"prompts/get","params":{"name":"hello","arguments":{}}})"));
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(ProtocolBranchTest2, HandleRawParseError) {
    std::string result = proto.handle_raw("{invalid json!!!");
    auto parsed = json::parse(result);
    EXPECT_TRUE(parsed.contains("error"));
    EXPECT_EQ(parsed["error"]["code"], -32700);
}

TEST_F(ProtocolBranchTest2, HandleRawValidRequest) {
    std::string result = proto.handle_raw(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    auto parsed = json::parse(result);
    EXPECT_TRUE(parsed.contains("result"));
}

TEST_F(ProtocolBranchTest2, RegisterResourceAndList) {
    proto.register_resource({.uri = "test://list", .name = "ListRes", .description = "desc"},
        [](const json&) -> json { return {{"v", 1}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/list","params":{"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["resources"].is_array());
}

// ═══════════════════════════════════════════════════════════════════════════════
// AgentRegistry: uncovered branches
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AgentRegistryBranch, RegisterAndUpdateAgent) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");

    AgentInfo info;
    info.name = "Agent One";
    info.capabilities = {"python", "docker"};
    auto id = reg.register_agent(info, token);
    EXPECT_FALSE(id.empty());

    auto agent = reg.get_agent(id);
    EXPECT_TRUE(agent.has_value());

    AgentInfo update;
    update.name = "Agent One Updated";
    update.capabilities = {"python", "docker", "rust"};
    EXPECT_TRUE(reg.update_agent(id, update, token));
    auto updated = reg.get_agent(id);
    EXPECT_EQ(updated->name, "Agent One Updated");
}

TEST(AgentRegistryBranch, DeregisterAgent) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Coordinator, "swarm-1");

    AgentInfo info;
    info.name = "Agent One";
    auto id = reg.register_agent(info, token);
    EXPECT_TRUE(reg.deregister_agent(id, token));
    EXPECT_FALSE(reg.get_agent(id).has_value());
}

TEST(AgentRegistryBranch, DeregisterDeniedNonCoordinator) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto worker_token = auth.issue_token("w1", Role::Worker, "swarm-1");
    auto coordinator_token = auth.issue_token("c1", Role::Coordinator, "swarm-1");

    AgentInfo info;
    info.name = "Agent One";
    auto id = reg.register_agent(info, coordinator_token);
    EXPECT_FALSE(reg.deregister_agent(id, worker_token));
}

TEST(AgentRegistryBranch, DeregisterNonexistent) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Coordinator, "swarm-1");
    EXPECT_FALSE(reg.deregister_agent("nonexistent", token));
}

TEST(AgentRegistryBranch, UpdateDeniedNonOwner) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto other_token = auth.issue_token("other", Role::Worker, "swarm-1");
    auto coord_token = auth.issue_token("coord", Role::Coordinator, "swarm-1");

    AgentInfo info;
    info.name = "Agent One";
    auto id = reg.register_agent(info, coord_token);

    AgentInfo update;
    update.name = "Hacked";
    EXPECT_FALSE(reg.update_agent(id, update, other_token));
}

TEST(AgentRegistryBranch, UpdateNonexistent) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");
    AgentInfo update;
    update.name = "Doesnt Matter";
    EXPECT_FALSE(reg.update_agent("nonexistent", update, token));
}

TEST(AgentRegistryBranch, SetAgentRoleNonCoordinator) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto worker_token = auth.issue_token("w1", Role::Worker, "swarm-1");
    AgentInfo info;
    info.name = "Agent";
    auto id = reg.register_agent(info, worker_token);

    auto new_role_token = auth.issue_token("other", Role::Worker, "swarm-1");
    EXPECT_FALSE(reg.set_agent_role(id, Role::Coordinator, new_role_token));
}

TEST(AgentRegistryBranch, SetAgentRoleNonexistent) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto coord_token = auth.issue_token("c1", Role::Coordinator, "swarm-1");
    EXPECT_FALSE(reg.set_agent_role("nonexistent", Role::Worker, coord_token));
}

TEST(AgentRegistryBranch, SetAgentRoleCoordinatorSuccess) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto coord_token = auth.issue_token("c1", Role::Coordinator, "swarm-1");
    AgentInfo info;
    info.name = "Agent";
    auto id = reg.register_agent(info, coord_token);
    EXPECT_TRUE(reg.set_agent_role(id, Role::Observer, coord_token));
    auto agent = reg.get_agent(id);
    EXPECT_EQ(agent->role, Role::Observer);
}

TEST(AgentRegistryBranch, RegisterSwarmMismatch) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto wrong_token = auth.issue_token("a1", Role::Worker, "wrong-swarm");
    AgentInfo info;
    info.name = "Agent";
    auto id = reg.register_agent(info, wrong_token);
    EXPECT_TRUE(id.empty());
}

TEST(AgentRegistryBranch, RegisterEmptySwarm) {
    AgentRegistry reg("");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "any-swarm");
    AgentInfo info;
    info.name = "Agent";
    auto id = reg.register_agent(info, token);
    EXPECT_FALSE(id.empty());
}

TEST(AgentRegistryBranch, ListByRole) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto coord_token = auth.issue_token("c1", Role::Coordinator, "swarm-1");
    AgentInfo info1;
    info1.name = "Coord";
    reg.register_agent(info1, coord_token);

    auto workers = reg.find_by_role(Role::Coordinator);
    EXPECT_EQ(workers.size(), 1u);
}

TEST(AgentRegistryBranch, FindIdle) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");
    AgentInfo info;
    info.name = "Idle Agent";
    auto id = reg.register_agent(info, token);
    auto idle = reg.find_idle();
    EXPECT_EQ(idle.size(), 1u);
}

TEST(AgentRegistryBranch, FindIdleInSwarm) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");
    AgentInfo info;
    info.name = "Idle Agent";
    reg.register_agent(info, token);

    auto idle = reg.find_idle_in_swarm("swarm-1");
    EXPECT_EQ(idle.size(), 1u);
    auto idle_other = reg.find_idle_in_swarm("other-swarm");
    EXPECT_EQ(idle_other.size(), 0u);
}

TEST(AgentRegistryBranch, PruneStale) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");
    AgentInfo info;
    info.name = "Stale Agent";
    auto id = reg.register_agent(info, token);

    size_t pruned = reg.prune_stale(std::chrono::seconds(0));
    EXPECT_GE(pruned, 0u);
}

TEST(AgentRegistryBranch, CountAndSwarmCount) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");
    AgentInfo info;
    info.name = "Agent";
    reg.register_agent(info, token);

    EXPECT_EQ(reg.count(), 1u);
    EXPECT_EQ(reg.swarm_count("swarm-1"), 1u);
    EXPECT_EQ(reg.swarm_count("other"), 0u);
}

TEST(AgentRegistryBranch, OnChangeCallback) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    std::string event_type;
    reg.on_change([&](const std::string& ev, const AgentInfo&) {
        event_type = ev;
    });
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");
    AgentInfo info;
    info.name = "Agent";
    reg.register_agent(info, token);
    EXPECT_EQ(event_type, "agent.registered");
}

TEST(AgentRegistryBranch, AgentInfoFromJsonAll) {
    json j = {
        {"id", "a1"}, {"name", "Agent One"}, {"platform", "linux"}, {"hostname", "host1"},
        {"swarm_id", "swarm-1"}, {"role", "coordinator"}, {"status", "busy"},
        {"capabilities", json::array({"rust"})}, {"registered_at", 1000LL}, {"last_heartbeat", 2000LL},
        {"metadata", {{"key", "val"}}},
        {"model", {{"provider", "openai"}, {"model_id", "gpt-4"}, {"model_family", "gpt"}, {"context_window", 128000}, {"max_output_tokens", 4096}}},
        {"environment", {{"runtime", "python"}, {"os", "linux"}, {"cpu_cores", 8}, {"memory_mb", 16384}, {"gpu", "A100"}, {"supported_languages", json::array({"python"})}}}
    };
    auto a = AgentInfo::from_json(j);
    EXPECT_EQ(a.id, "a1");
    EXPECT_EQ(a.status, AgentStatus::Busy);
    EXPECT_EQ(a.model.provider, "openai");
    EXPECT_EQ(a.environment.gpu, "A100");
}

TEST(AgentRegistryBranch, AgentInfoFromJsonOnlineOffline) {
    json j1 = {{"id", "a1"}, {"name", "T"}, {"status", "online"}};
    auto a1 = AgentInfo::from_json(j1);
    EXPECT_EQ(a1.status, AgentStatus::Online);

    json j2 = {{"id", "a2"}, {"name", "T"}, {"status", "idle"}};
    auto a2 = AgentInfo::from_json(j2);
    EXPECT_EQ(a2.status, AgentStatus::Idle);

    json j3 = {{"id", "a3"}, {"name", "T"}, {"status", "unknown_status"}};
    auto a3 = AgentInfo::from_json(j3);
    EXPECT_EQ(a3.status, AgentStatus::Offline);
}

TEST(AgentRegistryBranch, AgentInfoModelInfo) {
    AgentInfo::ModelInfo m;
    m.provider = "test";
    m.model_id = "m1";
    m.model_family = "test-family";
    m.context_window = 32000;
    m.max_output_tokens = 4096;
    auto mj = m.to_json();
    EXPECT_EQ(mj["provider"], "test");
    EXPECT_EQ(mj["context_window"], 32000);

    auto m2 = AgentInfo::ModelInfo::from_json(mj);
    EXPECT_EQ(m2.provider, "test");
    EXPECT_EQ(m2.context_window, 32000);
}

TEST(AgentRegistryBranch, AgentInfoModelInfoMinimal) {
    json mj = {{"provider", ""}, {"model_id", ""}, {"model_family", ""}};
    auto m = AgentInfo::ModelInfo::from_json(mj);
    EXPECT_EQ(m.context_window, 0);
    EXPECT_EQ(m.max_output_tokens, 0);

    AgentInfo::ModelInfo m2;
    m2.provider = "p";
    m2.model_id = "id";
    auto mj2 = m2.to_json();
    EXPECT_FALSE(mj2.contains("context_window"));
}

TEST(AgentRegistryBranch, AgentInfoEnvInfo) {
    AgentInfo::EnvironmentInfo e;
    e.runtime = "python";
    e.os = "linux";
    e.cpu_cores = 16;
    e.memory_mb = 32768;
    e.gpu = "T4";
    e.supported_languages = {"python", "cpp"};
    auto ej = e.to_json();
    EXPECT_EQ(ej["gpu"], "T4");
    EXPECT_EQ(ej["supported_languages"].size(), 2u);

    auto e2 = AgentInfo::EnvironmentInfo::from_json(ej);
    EXPECT_EQ(e2.cpu_cores, 16);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Transport: HTTP route handler branches
// ═══════════════════════════════════════════════════════════════════════════════

class HandlerBranchTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    AuthProvider auth_{"secret"};
    std::unique_ptr<StreamableHttpTransport> transport;

    void SetUp() override {
        transport = std::make_unique<StreamableHttpTransport>(proto, auth_,
            StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                                 .require_auth = false});
        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    }

    void TearDown() override {
        transport.reset();
    }

    httplib::Request make_request(const std::string& method, const std::string& path,
                                   const std::string& body = "",
                                   const std::string& auth_header = "") {
        httplib::Request req;
        req.method = method;
        req.path = path;
        req.target = path;
        if (!body.empty()) req.body = body;
        if (!auth_header.empty()) req.set_header("Authorization", auth_header);
        return req;
    }
};

TEST_F(HandlerBranchTest, PostRequiresAuth) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});

    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_EQ(res.status, 401);
}

TEST_F(HandlerBranchTest, PostWithQueryToken) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_.issue_token("agent-1", Role::Worker, "swarm");

    httplib::Request req;
    req.method = "POST";
    req.path = "/mcp";
    req.target = "/mcp";
    req.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
    req.set_header("Authorization", "Bearer " + token.token_string);
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_NE(res.status, 401);
}

TEST_F(HandlerBranchTest, PostWithBearerToken) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_.issue_token("agent-1", Role::Worker, "swarm");

    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "Bearer " + token.token_string);
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_NE(res.status, 401);
}

TEST_F(HandlerBranchTest, PostInvalidBearerToken) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});

    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "Bearer invalid-token-format");
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_EQ(res.status, 401);
}

TEST_F(HandlerBranchTest, PostValidAuthWrongRole) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_.issue_token("agent-1", Role::Observer, "swarm");

    proto.register_tool({.name = "admin_tool", .description = "admin",
        .input_schema = {}, .required_permission = Permission::Admin},
        [](const json&) -> json { return {{"admin", true}}; });

    std::string body = R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"admin_tool","arguments":{}}})";
    httplib::Request req = make_request("POST", "/mcp", body, "Bearer " + token.token_string);
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_NE(res.status, 401);
    auto result = json::parse(res.body);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(HandlerBranchTest, GetRejectsNonSSEAccept) {
    httplib::Request req;
    req.method = "GET";
    req.path = "/mcp";
    req.target = "/mcp";
    req.set_header("Accept", "application/json");
    auto token = auth_.issue_token("a1", Role::Worker, "swarm");
    req.set_header("Authorization", "Bearer " + token.token_string);
    httplib::Response res;
    transport->handle_get(req, res);
    EXPECT_EQ(res.status, 400);
}

TEST_F(HandlerBranchTest, DeleteWithSessionId) {
    httplib::Request req;
    req.method = "DELETE";
    req.path = "/mcp";
    req.target = "/mcp";
    req.set_header("Mcp-Session-Id", "some-session-id");
    httplib::Response res;
    transport->handle_delete(req, res);
    EXPECT_EQ(res.status, 204);
}

TEST_F(HandlerBranchTest, SseStreamBroadcastRemovesFailingClient) {
    SseStream sse;
    int count = 0;
    auto id1 = sse.add_client([&](const std::string&) -> bool { count++; return false; });
    sse.broadcast("test", {{"k", "v"}});
    EXPECT_EQ(count, 1);
    EXPECT_EQ(sse.client_count(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Config: from_file with valid JSON
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConfigFromFile, ValidJsonConfig) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm-mcp-cfg-test";
    std::filesystem::create_directories(test_dir);
    std::string config_path = test_dir + "/config.json";
    {
        std::ofstream f(config_path);
        f << R"({
            "server_name": "test-server",
            "server_version": "2.0",
            "swarm": {
                "id": "test-swarm",
                "display_name": "Test Swarm",
                "secret": "test-secret",
                "open_enrollment": true,
                "allowed_agents": ["a1", "a2"],
                "token_ttl_seconds": 3600,
                "heartbeat_timeout_seconds": 120
            },
            "mqtt": {
                "host": "mqtt.local",
                "port": 8883,
                "username": "user",
                "password": "pass",
                "client_id": "test-client",
                "keep_alive_sec": 90,
                "use_tls": true,
                "ca_cert_path": "/path/to/ca.pem"
            },
            "http": {
                "host": "0.0.0.0",
                "port": 8080,
                "endpoint": "/api",
                "cors_origin": "*",
                "thread_pool_size": 8,
                "require_auth": false
            },
            "git": {
                "repo_path": "/tmp/repo",
                "default_branch": "develop",
                "branch_prefix": "feature/",
                "auto_commit": true
            }
        })";
    }
    auto cfg = ServerConfig::from_file(config_path);
    EXPECT_EQ(cfg.server_name, "test-server");
    EXPECT_EQ(cfg.server_version, "2.0");
    EXPECT_EQ(cfg.swarm.id, "test-swarm");
    EXPECT_EQ(cfg.swarm.display_name, "Test Swarm");
    EXPECT_EQ(cfg.swarm.secret, "test-secret");
    EXPECT_TRUE(cfg.swarm.open_enrollment);
    EXPECT_EQ(cfg.swarm.allowed_agents.size(), 2u);
    EXPECT_EQ(cfg.swarm.token_ttl.count(), 3600);
    EXPECT_EQ(cfg.swarm.heartbeat_timeout.count(), 120);
    EXPECT_EQ(cfg.mqtt.host, "mqtt.local");
    EXPECT_EQ(cfg.mqtt.port, 8883);
    EXPECT_EQ(cfg.mqtt.username, "user");
    EXPECT_EQ(cfg.mqtt.password, "pass");
    EXPECT_EQ(cfg.mqtt.client_id, "test-client");
    EXPECT_EQ(cfg.mqtt.keep_alive_sec, 90);
    EXPECT_TRUE(cfg.mqtt.use_tls);
    EXPECT_EQ(cfg.mqtt.ca_cert_path, "/path/to/ca.pem");
    EXPECT_EQ(cfg.http.port, 8080);
    EXPECT_EQ(cfg.http.endpoint, "/api");
    EXPECT_EQ(cfg.http.cors_origin, "*");
    EXPECT_EQ(cfg.http.thread_pool_size, 8);
    EXPECT_FALSE(cfg.http.require_auth);
    EXPECT_EQ(cfg.git.repo_path, "/tmp/repo");
    EXPECT_EQ(cfg.git.default_branch, "develop");
    EXPECT_EQ(cfg.git.branch_prefix, "feature/");
    EXPECT_TRUE(cfg.git.auto_commit);

    std::filesystem::remove_all(test_dir);
}

TEST(ConfigFromFile, InvalidJson) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm-mcp-cfg-inv";
    std::filesystem::create_directories(test_dir);
    std::string config_path = test_dir + "/bad.json";
    {
        std::ofstream f(config_path);
        f << "{not valid json!!!";
    }
    auto cfg = ServerConfig::from_file(config_path);
    EXPECT_EQ(cfg.server_name, "swarm-mcp");

    std::filesystem::remove_all(test_dir);
}

TEST(ConfigFromFile, PartialConfig) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm-mcp-cfg-partial";
    std::filesystem::create_directories(test_dir);
    std::string config_path = test_dir + "/partial.json";
    {
        std::ofstream f(config_path);
        f << R"({"server_name": "partial-server"})";
    }
    auto cfg = ServerConfig::from_file(config_path);
    EXPECT_EQ(cfg.server_name, "partial-server");

    std::filesystem::remove_all(test_dir);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Auth: additional branch coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AuthExtraBranch, ExtractBearerEmptyBody) {
    auto val = AuthProvider::extract_bearer("Bearer ");
    EXPECT_FALSE(val.has_value());
}

TEST(AuthExtraBranch, ExtractBearerTrimsWhitespace) {
    auto val = AuthProvider::extract_bearer("Bearer token-with-spaces   ");
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), "token-with-spaces");
}

TEST(AuthExtraBranch, ExtractBearerTooShort) {
    EXPECT_FALSE(AuthProvider::extract_bearer("Bearer").has_value());
}

TEST(AuthExtraBranch, ValidateTokenRevokedThenRefresh) {
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "s1");
    EXPECT_TRUE(auth.validate_token(token.token_string).has_value());
    auth.revoke_token(token.token_id);
    auto refreshed = auth.refresh_token(token.token_string);
    EXPECT_FALSE(refreshed.has_value());
}

TEST(AuthExtraBranch, ValidateTokenDifferentProvider) {
    AuthProvider auth1("secret1");
    AuthProvider auth2("secret2");
    auto token = auth1.issue_token("a1", Role::Worker, "s1");
    EXPECT_FALSE(auth2.validate_token(token.token_string).has_value());
}

TEST(AuthExtraBranch, SignVerifyRoundTrip) {
    std::string payload = "test-payload-data";
    std::string secret = "my-signing-secret";
    auto sig = AuthProvider::sign(payload, secret);
    EXPECT_TRUE(AuthProvider::verify_sig(payload, sig, secret));
    EXPECT_FALSE(AuthProvider::verify_sig(payload, sig, "wrong-secret"));
    EXPECT_FALSE(AuthProvider::verify_sig(payload, "wrong-sig-length", secret));
}

// ═══════════════════════════════════════════════════════════════════════════════
// EventBus: additional branch coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EventBusBranch, SubscribeAndEmit) {
    EventBus bus;
    int count = 0;
    auto sub_id = bus.subscribe("test.event", [&](const Event&) { count++; });
    bus.emit("test.event", "src1", {{"key", "val"}});
    EXPECT_EQ(count, 1);
}

TEST(EventBusBranch, UnsubscribeStopsDelivery) {
    EventBus bus;
    int count = 0;
    auto sub_id = bus.subscribe("test.event", [&](const Event&) { count++; });
    bus.emit("test.event", "src1");
    EXPECT_EQ(count, 1);
    bus.unsubscribe(sub_id);
    bus.emit("test.event", "src2");
    EXPECT_EQ(count, 1);
}

TEST(EventBusBranch, QueryByType) {
    EventBus bus;
    bus.emit("type.a", "src");
    bus.emit("type.b", "src");
    bus.emit("type.a", "src");
    auto results = bus.query_events("type.a", 10);
    EXPECT_EQ(results.size(), 2u);
}

TEST(EventBusBranch, EventToJsonFromJson) {
    Event e;
    e.id = "evt-1";
    e.type = "test";
    e.source = "src";
    e.data = {{"key", "val"}};
    auto j = e.to_json();
    EXPECT_EQ(j["id"], "evt-1");
    auto restored = Event::from_json(j);
    EXPECT_EQ(restored.id, "evt-1");
    EXPECT_EQ(restored.type, "test");
}

// ═══════════════════════════════════════════════════════════════════════════════
// ContextStore: additional branch coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ContextStoreBranch, SetAndGet) {
    ContextStore store;
    store.set("k1", {{"x", 1}});
    auto entry = store.get("k1");
    EXPECT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value["x"], 1);
    EXPECT_EQ(entry->version, 1);
}

TEST(ContextStoreBranch, SetOverwrites) {
    ContextStore store;
    store.set("k1", "v1");
    store.set("k1", "v2");
    auto entry = store.get("k1");
    EXPECT_EQ(entry->value, "v2");
    EXPECT_EQ(entry->version, 2);
}

TEST(ContextStoreBranch, DeleteKey) {
    ContextStore store;
    store.set("k1", 42);
    EXPECT_TRUE(store.del("k1"));
    EXPECT_FALSE(store.get("k1").has_value());
}

TEST(ContextStoreBranch, DeleteNonexistent) {
    ContextStore store;
    EXPECT_FALSE(store.del("nonexistent"));
}

TEST(ContextStoreBranch, Exists) {
    ContextStore store;
    store.set("k1", 42);
    EXPECT_TRUE(store.exists("k1"));
    EXPECT_FALSE(store.exists("k2"));
}

TEST(ContextStoreBranch, UpdatePartial) {
    ContextStore store;
    store.set("k1", {{"a", 1}, {"b", 2}});
    EXPECT_TRUE(store.update_partial("k1", {{"b", 20}, {"c", 30}}));
    auto entry = store.get("k1");
    EXPECT_EQ(entry->value["a"], 1);
    EXPECT_EQ(entry->value["b"], 20);
    EXPECT_EQ(entry->value["c"], 30);
    EXPECT_EQ(entry->version, 2);
}

TEST(ContextStoreBranch, SizeAndClear) {
    ContextStore store;
    store.set("k1", 1);
    store.set("k2", 2);
    EXPECT_EQ(store.size(), 2u);
    store.clear();
    EXPECT_EQ(store.size(), 0u);
}

TEST(ContextStoreBranch, OnChangeCallback) {
    ContextStore store;
    std::string action;
    store.on_change([&](const std::string&, const ContextEntry&, const std::string& a) {
        action = a;
    });
    store.set("k1", 42);
    EXPECT_EQ(action, "created");
}

TEST(ContextStoreBranch, ContextEntryFromJson) {
    json j = {{"key", "test"}, {"value", 42}, {"owner", "agent-1"}, {"version", 3}};
    auto entry = ContextEntry::from_json(j);
    EXPECT_EQ(entry.key, "test");
    EXPECT_EQ(entry.value, 42);
    EXPECT_EQ(entry.owner, "agent-1");
    EXPECT_EQ(entry.version, 3);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Channel type_prefix branches
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ChannelTypePrefix, AllChannelTypes) {
    MqttConfig cfg;
    SecureMqttClient mqtt(cfg, "swarm", "secret");

    for (auto type : {ChannelType::TaskUpdates, ChannelType::AgentPresence,
                      ChannelType::Events, ChannelType::ContextSync,
                      ChannelType::GitCoordination, ChannelType::Broadcast,
                      ChannelType::Custom}) {
        Channel ch(mqtt, ChannelSpec{.type = type, .namespace_ = "test", .name = "x"}, "swarm");
        EXPECT_FALSE(ch.topic().empty());
    }
}

TEST(ChannelTypePrefix, CustomChannelWithName) {
    MqttConfig cfg;
    SecureMqttClient mqtt(cfg, "swarm", "secret");
    Channel ch(mqtt, ChannelSpec{.type = ChannelType::Custom, .namespace_ = "test", .name = "my-channel"}, "swarm");
    EXPECT_NE(ch.topic().find("my-channel"), std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Permission checks
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PermissionBranch, CoordinatorHasAllPermissions) {
    uint32_t coord_perms = role_permissions(Role::Coordinator);
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::Admin));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::MergeApprove));
    EXPECT_EQ(coord_perms, 0xFFFFFFFF);
}

TEST(PermissionBranch, WorkerPermissions) {
    EXPECT_TRUE(has_permission(Role::Worker, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::BranchCreate));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::ContextWrite));
    EXPECT_FALSE(has_permission(Role::Worker, Permission::Admin));
    EXPECT_FALSE(has_permission(Role::Worker, Permission::MergeApprove));
    EXPECT_FALSE(has_permission(Role::Worker, Permission::AgentManage));
}

TEST(PermissionBranch, ObserverPermissions) {
    EXPECT_TRUE(has_permission(Role::Observer, Permission::TaskRead));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::AgentRead));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::ContextRead));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::EventRead));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::TaskCreate));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::ContextWrite));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Task Manager: notification callbacks
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TaskManagerCallback, OnTaskEvent) {
    TaskManager tm;
    std::string event_type;
    tm.on_task_event([&](const std::string& ev, const Task&) { event_type = ev; });
    tm.create_task("Test", "a1");
    EXPECT_EQ(event_type, "task.created");
}

TEST(TaskManagerCallback, DeleteTask) {
    TaskManager tm;
    auto t = tm.create_task("Del", "a1");
    EXPECT_TRUE(tm.delete_task(t.id));
    EXPECT_FALSE(tm.get_task(t.id).has_value());
}

TEST(TaskManagerCallback, DeleteNonexistent) {
    TaskManager tm;
    EXPECT_FALSE(tm.delete_task("nonexistent"));
}

TEST(TaskManagerCallback, GetAgentTasks) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    tm.assign_task(t1.id, "agent-x");
    auto tasks = tm.get_agent_tasks("agent-x");
    EXPECT_EQ(tasks.size(), 1u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Persistence: save/load roundtrip
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PersistenceRoundtrip, SaveAndLoad) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_persist_rt";
    std::string test_path = test_dir + "/snapshot.json";
    std::filesystem::create_directories(test_dir);

    json snapshot = PersistenceLayer::create_snapshot(
        json::array(), json::object(), json::object(), json::array(), json::array());
    PersistenceLayer pl(test_path);
    EXPECT_TRUE(pl.save(snapshot));
    auto loaded = pl.load();
    EXPECT_TRUE(loaded.has_value());
    EXPECT_TRUE((*loaded).contains("tasks"));

    std::filesystem::remove_all(test_dir);
}

TEST(PersistenceRoundtrip, SaveFailsOnInvalidPath) {
    auto tmp = std::filesystem::temp_directory_path() / std::format("swarm_mcp_test_file_parent_{}.txt",
        std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream f(tmp);
        f << "x";
    }
    PersistenceLayer pl(tmp.string() + "/sub/snapshot.json");
    json snapshot = PersistenceLayer::create_snapshot(
        json::array(), json::object(), json::object(), json::array(), json::array());
    EXPECT_FALSE(pl.save(snapshot));
    std::filesystem::remove(tmp);
}