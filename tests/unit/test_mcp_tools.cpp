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
#include <filesystem>

using namespace mcp_collab;

class CollabToolsTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test-tools", .version = "1.0.0"}};
    TaskManager tasks;
    AgentRegistry agents{"test-swarm"};
    ContextStore context;
    EventBus events;
    SecureMqttClient mqtt{MqttConfig{.host = "localhost", .port = 18839}, "test-swarm", "secret"};
    std::string test_repo_path;
    GitOperations git{""};
    BranchManager branches{git, "collab/"};
    MergeCoordinator merges{git, branches};

    void SetUp() override {
        test_repo_path = (std::filesystem::temp_directory_path() / ("swarm-mcp-collab-tools-" + std::to_string(reinterpret_cast<uintptr_t>(this)))).string();
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
        git = GitOperations(test_repo_path);
        branches = BranchManager(git, "collab/");
        merges = MergeCoordinator(git, branches);
        register_collab_tools(proto, tasks, agents, context, events, branches, merges, mqtt.raw_client(), channels);
        proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {{"_auth", {{"role", "coordinator"}}}}}});
    }

    json call_tool(const std::string& name, const json& args, Role role = Role::Coordinator) {
        json params = {{"name", name}, {"arguments", args}};
        if (role == Role::Coordinator) {
            params["_auth"] = {{"agent_id", "coord-1"}, {"role", "coordinator"}, {"swarm_id", "test-swarm"}};
        } else if (role == Role::Worker) {
            params["_auth"] = {{"agent_id", "worker-1"}, {"role", "worker"}, {"swarm_id", "test-swarm"}};
        } else {
            params["_auth"] = {{"agent_id", "obs-1"}, {"role", "observer"}, {"swarm_id", "test-swarm"}};
        }
        json req = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", "tools/call"},
            {"params", params},
        };
        auto resp = proto.handle_request(req);
        return resp;
    }

    json tool_result(const json& resp) {
        if (resp.contains("result") && resp["result"].contains("content")) {
            auto& content = resp["result"]["content"];
            if (content.is_array() && content.size() > 0 && content[0].contains("text")) {
                return json::parse(content[0]["text"].get<std::string>());
            }
        }
        return resp;
    }
};

TEST_F(CollabToolsTest, TaskCreate) {
    auto resp = call_tool("task_create", {{"title", "My Task"}, {"creator", "coord-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["title"], "My Task");
    EXPECT_EQ(result["creator"], "coord-1");
    EXPECT_EQ(result["status"], "pending");
}

TEST_F(CollabToolsTest, TaskCreateWithPriorityAndTags) {
    auto resp = call_tool("task_create", {
        {"title", "Tagged Task"},
        {"creator", "coord-1"},
        {"priority", "high"},
        {"tags", json::array({"backend", "urgent"})},
        {"description", "A task with all fields"}
    });
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["priority"], "high");
    auto fetched = call_tool("task_get", {{"task_id", result["id"].get<std::string>()}});
    auto fetched_task = tool_result(fetched);
    EXPECT_EQ(fetched_task["tags"].size(), 2u);
}

TEST_F(CollabToolsTest, TaskCreateObserverDenied) {
    auto resp = call_tool("task_create", {{"title", "Blocked"}, {"creator", "obs-1"}}, Role::Observer);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"]["isError"] == true));
}

TEST_F(CollabToolsTest, TaskList) {
    call_tool("task_create", {{"title", "Task 1"}, {"creator", "coord-1"}});
    call_tool("task_create", {{"title", "Task 2"}, {"creator", "coord-1"}});
    auto resp = call_tool("task_list", {}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 2u);
}

TEST_F(CollabToolsTest, TaskListFilterByStatus) {
    call_tool("task_create", {{"title", "T1"}, {"creator", "coord-1"}});
    auto resp = call_tool("task_list", {{"status", "pending"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabToolsTest, TaskAssignSuccess) {
    auto create_resp = call_tool("task_create", {{"title", "AssignMe"}, {"creator", "coord-1"}});
    auto task = tool_result(create_resp);
    std::string task_id = task["id"];

    auto resp = call_tool("task_assign", {{"task_id", task_id}, {"agent_id", "worker-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskAssignNotFound) {
    auto resp = call_tool("task_assign", {{"task_id", "nonexistent"}, {"agent_id", "worker-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskUpdateStatus) {
    auto create_resp = call_tool("task_create", {{"title", "StatusTask"}, {"creator", "coord-1"}});
    auto task = tool_result(create_resp);
    std::string task_id = task["id"];

    auto resp = call_tool("task_update_status", {{"task_id", task_id}, {"status", "in_progress"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskUpdateStatusCompleted) {
    auto create_resp = call_tool("task_create", {{"title", "CompleteTask"}, {"creator", "coord-1"}});
    auto task = tool_result(create_resp);
    std::string task_id = task["id"];

    auto resp = call_tool("task_update_status", {{"task_id", task_id}, {"status", "completed"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskUpdateStatusNotFound) {
    auto resp = call_tool("task_update_status", {{"task_id", "nope"}, {"status", "in_progress"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskGet) {
    auto create_resp = call_tool("task_create", {{"title", "GetMe"}, {"creator", "coord-1"}});
    auto task = tool_result(create_resp);
    std::string task_id = task["id"];

    auto resp = call_tool("task_get", {{"task_id", task_id}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["title"], "GetMe");
}

TEST_F(CollabToolsTest, TaskGetNotFound) {
    auto resp = call_tool("task_get", {{"task_id", "nonexistent"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, TaskReady) {
    auto resp = call_tool("task_ready", {}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.is_array());
}

TEST_F(CollabToolsTest, TaskCreateWithDependencies) {
    auto t1_resp = call_tool("task_create", {{"title", "Dep1"}, {"creator", "coord-1"}});
    auto t1 = tool_result(t1_resp);
    auto t2_resp = call_tool("task_create", {{"title", "Dep2"}, {"creator", "coord-1"}, {"dependencies", json::array({t1["id"].get<std::string>()})}});
    EXPECT_TRUE(t2_resp.contains("result"));
    auto t2 = tool_result(t2_resp);
    auto fetched = call_tool("task_get", {{"task_id", t2["id"].get<std::string>()}});
    auto fetched_task = tool_result(fetched);
    EXPECT_EQ(fetched_task["dependencies"].size(), 1u);
}

TEST_F(CollabToolsTest, AgentRegister) {
    auto resp = call_tool("agent_register", {
        {"name", "TestAgent"},
        {"platform", "test"},
        {"capabilities", json::array({"coding", "review"})}
    });
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["name"], "TestAgent");
    EXPECT_EQ(result["capabilities"].size(), 2u);
}

TEST_F(CollabToolsTest, AgentRegisterWithModel) {
    auto resp = call_tool("agent_register", {
        {"name", "ModelAgent"},
        {"platform", "test"},
        {"model", json::object({
            {"provider", "zhipu"},
            {"model_id", "glm-5.1"},
            {"model_family", "glm"},
            {"context_window", 128000},
            {"max_output_tokens", 8192}
        })}
    });
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["name"], "ModelAgent");
    EXPECT_EQ(result["model"]["provider"], "zhipu");
    EXPECT_EQ(result["model"]["model_id"], "glm-5.1");
    EXPECT_EQ(result["model"]["model_family"], "glm");
    EXPECT_EQ(result["model"]["context_window"], 128000);
    EXPECT_EQ(result["model"]["max_output_tokens"], 8192);
}

TEST_F(CollabToolsTest, AgentRegisterWithEnvironment) {
    auto resp = call_tool("agent_register", {
        {"name", "EnvAgent"},
        {"platform", "test"},
        {"environment", json::object({
            {"runtime", "node.js"},
            {"os", "linux"},
            {"cpu_cores", 16},
            {"memory_mb", 32768},
            {"gpu", "NVIDIA RTX 4090"},
            {"supported_languages", json::array({"typescript", "python", "rust"})}
        })}
    });
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["name"], "EnvAgent");
    EXPECT_EQ(result["environment"]["runtime"], "node.js");
    EXPECT_EQ(result["environment"]["os"], "linux");
    EXPECT_EQ(result["environment"]["cpu_cores"], 16);
    EXPECT_EQ(result["environment"]["memory_mb"], 32768);
    EXPECT_EQ(result["environment"]["gpu"], "NVIDIA RTX 4090");
    EXPECT_EQ(result["environment"]["supported_languages"].size(), 3u);
}

TEST_F(CollabToolsTest, AgentRegisterWithModelAndEnvironment) {
    auto resp = call_tool("agent_register", {
        {"name", "FullAgent"},
        {"platform", "test"},
        {"capabilities", json::array({"coding"})},
        {"model", json::object({
            {"provider", "anthropic"},
            {"model_id", "claude-opus-4"},
            {"context_window", 200000},
            {"max_output_tokens", 16384}
        })},
        {"environment", json::object({
            {"runtime", "python"},
            {"os", "macos"},
            {"cpu_cores", 8},
            {"memory_mb", 16384}
        })}
    });
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["name"], "FullAgent");
    EXPECT_EQ(result["model"]["provider"], "anthropic");
    EXPECT_EQ(result["environment"]["runtime"], "python");
    EXPECT_EQ(result["capabilities"].size(), 1u);
}

TEST_F(CollabToolsTest, AgentDescribe) {
    auto reg_resp = call_tool("agent_register", {
        {"name", "DescAgent"},
        {"platform", "test"},
        {"capabilities", json::array({"coding"})},
        {"model", json::object({{"provider", "test-provider"}, {"model_id", "test-model"}})},
        {"environment", json::object({{"runtime", "go"}, {"os", "linux"}, {"cpu_cores", 4}})}
    });
    auto agent = tool_result(reg_resp);
    std::string agent_id = agent["id"];

    auto resp = call_tool("agent_describe", {{"agent_id", agent_id}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["name"], "DescAgent");
    EXPECT_EQ(result["model"]["provider"], "test-provider");
    EXPECT_EQ(result["environment"]["runtime"], "go");
    EXPECT_TRUE(result.contains("effective_tools"));
    EXPECT_TRUE(result["effective_tools"].is_array());
    EXPECT_GT(result["effective_tools"].size(), 0u);
    EXPECT_TRUE(result.contains("effective_permissions"));
    EXPECT_TRUE(result["effective_permissions"].is_number());
}

TEST_F(CollabToolsTest, AgentDescribeWorkerRole) {
    auto reg_resp = call_tool("agent_register", {{"name", "WorkerAgent"}, {"platform", "test"}});
    auto agent = tool_result(reg_resp);
    std::string agent_id = agent["id"];

    call_tool("agent_set_role", {{"agent_id", agent_id}, {"role", "worker"}});

    auto resp = call_tool("agent_describe", {{"agent_id", agent_id}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("effective_tools"));
    EXPECT_TRUE(result["effective_tools"].is_array());

    bool has_task_read = false;
    for (const auto& t : result["effective_tools"]) {
        if (t == "task_list") has_task_read = true;
    }
    EXPECT_TRUE(has_task_read);
}

TEST_F(CollabToolsTest, AgentDescribeObserverRole) {
    auto reg_resp = call_tool("agent_register", {{"name", "ObsAgent"}, {"platform", "test"}});
    auto agent = tool_result(reg_resp);
    std::string agent_id = agent["id"];

    call_tool("agent_set_role", {{"agent_id", agent_id}, {"role", "observer"}});

    auto resp = call_tool("agent_describe", {{"agent_id", agent_id}}, Role::Observer);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("effective_tools"));

    bool has_task_create = false;
    for (const auto& t : result["effective_tools"]) {
        if (t == "task_create") has_task_create = true;
    }
    EXPECT_FALSE(has_task_create);
}

TEST_F(CollabToolsTest, AgentDescribeNotFound) {
    auto resp = call_tool("agent_describe", {{"agent_id", "nonexistent"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentList) {
    call_tool("agent_register", {{"name", "Agent1"}, {"platform", "test"}});
    auto resp = call_tool("agent_list", {}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabToolsTest, AgentListFilterByStatus) {
    call_tool("agent_register", {{"name", "Agent2"}, {"platform", "test"}});
    auto resp = call_tool("agent_list", {{"status", "online"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 0u);
}

TEST_F(CollabToolsTest, AgentListBySwarm) {
    call_tool("agent_register", {{"name", "SwarmAgent"}, {"platform", "test"}});
    auto resp = call_tool("agent_list", {{"swarm_id", "test-swarm"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabToolsTest, AgentFindCapability) {
    call_tool("agent_register", {{"name", "CapAgent"}, {"platform", "test"}, {"capabilities", json::array({"rust"})}});
    auto resp = call_tool("agent_find_capability", {{"capability", "rust"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 1u);
    EXPECT_EQ(result[0]["name"], "CapAgent");
}

TEST_F(CollabToolsTest, AgentSetRole) {
    auto reg_resp = call_tool("agent_register", {{"name", "RoleAgent"}, {"platform", "test"}});
    auto agent = tool_result(reg_resp);
    std::string agent_id = agent["id"];

    auto resp = call_tool("agent_set_role", {{"agent_id", agent_id}, {"role", "worker"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("role") || result.contains("success"));
}

TEST_F(CollabToolsTest, AgentSetRoleDeniedForWorker) {
    auto reg_resp = call_tool("agent_register", {{"name", "RoleAgent2"}, {"platform", "test"}});
    auto agent = tool_result(reg_resp);
    std::string agent_id = agent["id"];

    auto resp = call_tool("agent_set_role", {{"agent_id", agent_id}, {"role", "coordinator"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && tool_result(resp).contains("error")));
}

TEST_F(CollabToolsTest, ContextSet) {
    auto resp = call_tool("context_set", {{"key", "my.key"}, {"value", 42}, {"owner", "coord-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
    EXPECT_EQ(result["key"], "my.key");
}

TEST_F(CollabToolsTest, ContextGet) {
    call_tool("context_set", {{"key", "test.val"}, {"value", "hello"}, {"owner", "coord-1"}});
    auto resp = call_tool("context_get", {{"key", "test.val"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["value"], "hello");
}

TEST_F(CollabToolsTest, ContextGetNotFound) {
    auto resp = call_tool("context_get", {{"key", "nonexistent"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, ContextList) {
    call_tool("context_set", {{"key", "list.a"}, {"value", 1}, {"owner", "coord-1"}});
    call_tool("context_set", {{"key", "list.b"}, {"value", 2}, {"owner", "coord-1"}});
    auto resp = call_tool("context_list", {}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 2u);
}

TEST_F(CollabToolsTest, ContextListWithPrefix) {
    call_tool("context_set", {{"key", "prefix.x"}, {"value", 1}, {"owner", "coord-1"}});
    call_tool("context_set", {{"key", "other.y"}, {"value", 2}, {"owner", "coord-1"}});
    auto resp = call_tool("context_list", {{"prefix", "prefix"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabToolsTest, ContextMergeSuccess) {
    call_tool("context_set", {{"key", "merge.key"}, {"value", json::object({{"a", 1}})}, {"owner", "coord-1"}});
    auto resp = call_tool("context_merge", {{"key", "merge.key"}, {"data", json::object({{"b", 2}})}, {"owner", "coord-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, ContextMergeSuccessOnNew) {
    auto resp = call_tool("context_merge", {{"key", "new.merge.key"}, {"data", json::object({{"b", 2}})}, {"owner", "coord-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, MergeRequest) {
    auto resp = call_tool("merge_request", {{"source", "feature/x"}, {"target", "main"}, {"requester", "coord-1"}, {"strategy", "squash"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["source_branch"], "feature/x");
    EXPECT_EQ(result["status"], "pending");
}

TEST_F(CollabToolsTest, MergeRequestWithStrategy) {
    auto resp = call_tool("merge_request", {{"source", "feature/y"}, {"requester", "coord-1"}, {"strategy", "rebase"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_EQ(result["strategy"], "rebase");
}

TEST_F(CollabToolsTest, EventPublish) {
    auto resp = call_tool("event_publish", {{"event_type", "test.event"}, {"source", "coord-1"}, {"data", json::object({{"x", 1}})}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
    EXPECT_EQ(result["event_type"], "test.event");
}

TEST_F(CollabToolsTest, EventRecent) {
    events.emit("test.type", "coord-1", {{"val", 1}});
    auto resp = call_tool("event_recent", {}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabToolsTest, EventRecentWithCountAndType) {
    events.emit("filtered.type", "coord-1", {});
    events.emit("other.type", "coord-1", {});
    auto resp = call_tool("event_recent", {{"count", 1}, {"type", "filtered.type"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_GE(result.size(), 1u);
}

TEST_F(CollabToolsTest, HeartbeatSuccess) {
    auto reg_resp = call_tool("agent_register", {{"name", "HB"}, {"platform", "test"}});
    auto agent = tool_result(reg_resp);
    std::string agent_id = agent["id"];

    auto resp = call_tool("heartbeat", {{"agent_id", agent_id}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, HeartbeatNotFound) {
    auto resp = call_tool("heartbeat", {{"agent_id", "ghost"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, BranchCreateFailNoGitRepo) {
    auto resp = call_tool("branch_create", {{"task_id", "t1"}, {"agent_id", "coord-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, BranchCommitFail) {
    auto resp = call_tool("branch_commit", {{"branch", "collab/nope"}, {"message", "test"}, {"agent_id", "coord-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, MergeApproveFail) {
    auto resp = call_tool("merge_approve", {{"merge_id", "nonexistent"}, {"reviewer", "coord-1"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeExecuteFail) {
    auto resp = call_tool("merge_execute", {{"merge_id", "nonexistent"}});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}
