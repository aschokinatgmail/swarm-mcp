#include <gtest/gtest.h>
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
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

using namespace mcp_collab;

class CollabResourcesTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test-res", .version = "1.0.0"}};
    TaskManager tasks;
    AgentRegistry agents{"test-swarm"};
    ContextStore context;
    EventBus events;

    void SetUp() override {
        register_collab_resources(proto, tasks, agents, context, events);
        proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    }

    json read_resource(const std::string& uri) {
        json req = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "resources/read"},
            {"params", {{"uri", uri}}},
            {"_auth", {{"role", "worker"}}}
        };
        auto resp = proto.handle_request(req);
        if (resp.contains("result") && resp["result"].contains("contents")) {
            auto& contents = resp["result"]["contents"];
            if (contents.is_array() && contents.size() > 0 && contents[0].contains("text")) {
                return json::parse(contents[0]["text"].get<std::string>());
            }
        }
        return resp;
    }

    json list_resources() {
        json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "resources/list"}, {"params", {}}};
        auto resp = proto.handle_request(req);
        return resp["result"]["resources"];
    }
};

TEST_F(CollabResourcesTest, ListAllResources) {
    auto resources = list_resources();
    EXPECT_GE(resources.size(), 6u);
}

TEST_F(CollabResourcesTest, ResourceTasksEmpty) {
    auto result = read_resource("swarm://tasks");
    EXPECT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 0u);
}

TEST_F(CollabResourcesTest, ResourceTasksWithTasks) {
    tasks.create_task("Task 1", "agent-1");
    tasks.create_task("Task 2", "agent-2");
    auto result = read_resource("swarm://tasks");
    EXPECT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 2u);
}

TEST_F(CollabResourcesTest, ResourceReadyTasksEmpty) {
    auto result = read_resource("swarm://tasks/ready");
    EXPECT_TRUE(result.is_array());
}

TEST_F(CollabResourcesTest, ResourceReadyTasksWithData) {
    auto t1 = tasks.create_task("Ready1", "agent-1");
    auto result = read_resource("swarm://tasks/ready");
    EXPECT_TRUE(result.is_array());
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabResourcesTest, ResourceAgentsEmpty) {
    auto result = read_resource("swarm://agents");
    EXPECT_TRUE(result.is_array());
}

TEST_F(CollabResourcesTest, ResourceAgentsWithAgents) {
    AgentInfo info;
    info.name = "TestAgent";
    info.platform = "test";
    AuthToken token;
    token.agent_id = "a1";
    token.role = Role::Coordinator;
    token.swarm_id = "test-swarm";
    agents.register_agent(info, token);

    auto result = read_resource("swarm://agents");
    EXPECT_TRUE(result.is_array());
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabResourcesTest, ResourceIdleAgents) {
    AgentInfo info;
    info.name = "IdleAgent";
    info.platform = "test";
    info.status = AgentStatus::Idle;
    AuthToken token;
    token.agent_id = "idle1";
    token.role = Role::Coordinator;
    token.swarm_id = "test-swarm";
    auto id = agents.register_agent(info, token);
    auto reg = agents.get_agent(id);
    ASSERT_TRUE(reg.has_value());

    auto result = read_resource("swarm://agents/idle");
    EXPECT_TRUE(result.is_array());
}

TEST_F(CollabResourcesTest, ResourceContextEmpty) {
    auto result = read_resource("swarm://context");
    EXPECT_TRUE(result.is_object());
}

TEST_F(CollabResourcesTest, ResourceContextWithData) {
    context.set("key1", 42, "owner");
    context.set("key2", "hello", "owner");
    auto result = read_resource("swarm://context");
    EXPECT_TRUE(result.is_object());
    EXPECT_GE(result.size(), 2u);
}

TEST_F(CollabResourcesTest, ResourceEventsRecentEmpty) {
    auto result = read_resource("swarm://events/recent");
    EXPECT_TRUE(result.is_array());
}

TEST_F(CollabResourcesTest, ResourceEventsRecentWithData) {
    events.emit("test.type", "source", {{"x", 1}});
    events.emit("another.type", "source", {{"y", 2}});
    auto result = read_resource("swarm://events/recent");
    EXPECT_TRUE(result.is_array());
    EXPECT_GE(result.size(), 2u);
}

TEST_F(CollabResourcesTest, ReadNonexistentResource) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "resources/read"},
        {"params", {{"uri", "swarm://nonexistent"}}},
        {"_auth", {{"role", "worker"}}}
    };
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
}

class CollabCapabilitiesTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test-caps", .version = "1.0.0"}};
    TaskManager tasks;
    AgentRegistry agents{"test-swarm"};
    ContextStore context;
    EventBus events;
    SecureMqttClient mqtt{MqttConfig{.host = "localhost", .port = 18839}, "test-swarm", "secret"};
    GitOperations git{"."};
    BranchManager branches{git, "collab/"};
    MergeCoordinator merges{git, branches};
    ChannelManager channels{mqtt, "mcp-collab", "test-swarm"};

    void SetUp() override {
        register_collab_tools(proto, tasks, agents, context, events, branches, merges, mqtt.raw_client(), channels);
        register_collab_resources(proto, tasks, agents, context, events);
        proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    }

    json read_resource(const std::string& uri) {
        json req = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "resources/read"},
            {"params", {{"uri", uri}}},
            {"_auth", {{"role", "coordinator"}}}
        };
        auto resp = proto.handle_request(req);
        if (resp.contains("result") && resp["result"].contains("contents")) {
            auto& contents = resp["result"]["contents"];
            if (contents.is_array() && contents.size() > 0 && contents[0].contains("text")) {
                return json::parse(contents[0]["text"].get<std::string>());
            }
        }
        return resp;
    }
};

TEST_F(CollabCapabilitiesTest, ServerCapabilitiesByRole) {
    auto result = read_resource("swarm://capabilities");
    EXPECT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 3u);

    bool found_coordinator = false, found_worker = false, found_observer = false;
    for (const auto& role_entry : result) {
        EXPECT_TRUE(role_entry.contains("role"));
        EXPECT_TRUE(role_entry.contains("permissions"));
        EXPECT_TRUE(role_entry.contains("tools"));
        EXPECT_TRUE(role_entry.contains("resources"));

        std::string role = role_entry["role"];
        if (role == "coordinator") found_coordinator = true;
        if (role == "worker") found_worker = true;
        if (role == "observer") found_observer = true;
    }
    EXPECT_TRUE(found_coordinator);
    EXPECT_TRUE(found_worker);
    EXPECT_TRUE(found_observer);
}

TEST_F(CollabCapabilitiesTest, ServerCapabilitiesCoordinatorHasAllTools) {
    auto result = read_resource("swarm://capabilities");

    json coord_entry;
    for (const auto& r : result) {
        if (r["role"] == "coordinator") { coord_entry = r; break; }
    }
    ASSERT_FALSE(coord_entry.is_null());
    EXPECT_GT(coord_entry["tools"].size(), 0u);
    EXPECT_GT(coord_entry["resources"].size(), 0u);

    bool has_task_create = false;
    for (const auto& t : coord_entry["tools"]) {
        if (t["name"] == "task_create") has_task_create = true;
    }
    EXPECT_TRUE(has_task_create);
}

TEST_F(CollabCapabilitiesTest, ServerCapabilitiesObserverReadOnly) {
    auto result = read_resource("swarm://capabilities");

    json obs_entry;
    for (const auto& r : result) {
        if (r["role"] == "observer") { obs_entry = r; break; }
    }
    ASSERT_FALSE(obs_entry.is_null());

    for (const auto& t : obs_entry["tools"]) {
        EXPECT_NE(t["name"], "task_create");
        EXPECT_NE(t["name"], "context_set");
        EXPECT_NE(t["name"], "agent_register");
    }
}

TEST_F(CollabCapabilitiesTest, AgentCapabilitiesEmpty) {
    auto result = read_resource("swarm://agents/capabilities");
    EXPECT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 0u);
}

TEST_F(CollabCapabilitiesTest, AgentCapabilitiesWithAgents) {
    AgentInfo info1;
    info1.name = "Agent1";
    info1.platform = "test";
    info1.model = AgentInfo::ModelInfo::from_json(json::object({
        {"provider", "test"}, {"model_id", "m1"}
    }));
    info1.environment = AgentInfo::EnvironmentInfo::from_json(json::object({
        {"runtime", "cpp"}, {"os", "linux"}, {"cpu_cores", 8}
    }));
    AuthToken worker_token;
    worker_token.agent_id = "a1";
    worker_token.role = Role::Worker;
    worker_token.swarm_id = "test-swarm";
    agents.register_agent(info1, worker_token);

    AgentInfo info2;
    info2.name = "Agent2";
    info2.platform = "test";
    AuthToken observer_token;
    observer_token.agent_id = "a2";
    observer_token.role = Role::Observer;
    observer_token.swarm_id = "test-swarm";
    agents.register_agent(info2, observer_token);

    auto result = read_resource("swarm://agents/capabilities");
    EXPECT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 2u);

    json worker_entry;
    json observer_entry;
    for (const auto& entry : result) {
        if (entry["name"] == "Agent1") worker_entry = entry;
        if (entry["name"] == "Agent2") observer_entry = entry;
    }
    ASSERT_FALSE(worker_entry.is_null());
    ASSERT_FALSE(observer_entry.is_null());

    EXPECT_EQ(worker_entry["role"], "worker");
    EXPECT_EQ(worker_entry["model"]["provider"], "test");
    EXPECT_EQ(worker_entry["environment"]["runtime"], "cpp");
    EXPECT_TRUE(worker_entry.contains("effective_tools"));
    EXPECT_GT(worker_entry["effective_tools"].size(), 0u);

    EXPECT_EQ(observer_entry["role"], "observer");
    EXPECT_TRUE(observer_entry["model"].is_object());
    EXPECT_TRUE(observer_entry.contains("effective_tools"));

    bool worker_has_context_write = false;
    for (const auto& t : worker_entry["effective_tools"]) {
        if (t == "context_set") worker_has_context_write = true;
    }
    EXPECT_TRUE(worker_has_context_write);

    bool observer_has_context_write = false;
    for (const auto& t : observer_entry["effective_tools"]) {
        if (t == "context_set") observer_has_context_write = true;
    }
    EXPECT_FALSE(observer_has_context_write);
}
