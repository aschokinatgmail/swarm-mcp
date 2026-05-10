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
#include <fstream>

using namespace mcp_collab;

class CollabToolsTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test-tools", .version = "1.0.0"}};
    TaskManager tasks;
    AgentRegistry agents{"test-swarm"};
    ContextStore context;
    EventBus events;
    SecureMqttClient mqtt{MqttConfig{.host = "localhost", .port = 18839}, "test-swarm", "secret"};
    ChannelManager channels{mqtt, "mcp-collab", "test-swarm"};

    std::string test_repo_path;
    std::unique_ptr<GitOperations> git;
    std::unique_ptr<BranchManager> branches;
    std::unique_ptr<MergeCoordinator> merges;

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
        git = std::make_unique<GitOperations>(test_repo_path);
        branches = std::make_unique<BranchManager>(*git, "collab/");
        merges = std::make_unique<MergeCoordinator>(*git, *branches);
        register_collab_tools(proto, tasks, agents, context, events, *branches, *merges, mqtt.raw_client(), channels);
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
    auto resp = call_tool("task_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("tasks"));
    EXPECT_EQ(result["tasks"].size(), 2u);
}

TEST_F(CollabToolsTest, TaskListWithStatusFilter) {
    auto r1 = call_tool("task_create", {{"title", "Pending Task"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    call_tool("task_assign", {{"task_id", id}, {"agent_id", "agent-1"}});
    auto resp = call_tool("task_list", {{"status", "assigned"}});
    auto result = tool_result(resp);
    EXPECT_EQ(result["tasks"].size(), 1u);
}

TEST_F(CollabToolsTest, TaskAssignAndComplete) {
    auto r1 = call_tool("task_create", {{"title", "Do Work"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_assign", {{"task_id", id}, {"agent_id", "agent-1"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
    resp = call_tool("task_complete", {{"task_id", id}, {"agent_id", "agent-1"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskCompleteObserverDenied) {
    auto r1 = call_tool("task_create", {{"title", "Observe"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_complete", {{"task_id", id}, {"agent_id", "agent-1"}}, Role::Observer);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"]["isError"] == true));
}

TEST_F(CollabToolsTest, HeartbeatRegistersAgent) {
    auto resp = call_tool("heartbeat", {{"agent_id", "agent-1"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
    auto agents_list = call_tool("agent_list", {});
    auto result = tool_result(agents_list);
    EXPECT_TRUE(result.contains("agents"));
    bool found = false;
    for (const auto& a : result["agents"]) {
        if (a["id"] == "agent-1") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CollabToolsTest, HeartbeatReturnsGitInfo) {
    auto resp = call_tool("heartbeat", {{"agent_id", "agent-1"}, {"current_branch", "main"}, {"current_commit", "abc123"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("git_info"));
}

TEST_F(CollabToolsTest, HeartbeatWithBadAgentFails) {
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

TEST_F(CollabToolsTest, AgentListEmpty) {
    auto resp = call_tool("agent_list", {});
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(CollabToolsTest, ContextStoreSetAndGet) {
    auto resp = call_tool("context_set", {{"key", "test-key"}, {"value", "test-value"}, {"agent_id", "coord-1"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
    resp = call_tool("context_get", {{"key", "test-key"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_EQ(result["value"], "test-value");
}

TEST_F(CollabToolsTest, ContextStoreDelete) {
    call_tool("context_set", {{"key", "del-key"}, {"value", "del-value"}, {"agent_id", "coord-1"}});
    auto resp = call_tool("context_delete", {{"key", "del-key"}, {"agent_id", "coord-1"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
    resp = call_tool("context_get", {{"key", "del-key"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error") || result.is_null());
}

TEST_F(CollabToolsTest, SubscribeEvent) {
    auto resp = call_tool("event_subscribe", {{"event_type", "test-event"}, {"agent_id", "coord-1"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, PublishEvent) {
    auto resp = call_tool("event_publish", {{"event_type", "test-event"}, {"payload", {{"msg", "hello"}}}, {"agent_id", "coord-1"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, GetSwarmState) {
    auto resp = call_tool("swarm_state", {});
    EXPECT_TRUE(resp.contains("result"));
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agents"));
    EXPECT_TRUE(result.contains("tasks"));
}

TEST_F(CollabToolsTest, UnauthorizedToolCall) {
    auto resp = call_tool("task_create", {{"title", "Bad"}, {"creator", "bad-actor"}}, Role::Observer);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"]["isError"] == true));
}

TEST_F(CollabToolsTest, ToolNotFound) {
    auto resp = call_tool("nonexistent_tool", {});
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(CollabToolsTest, BranchCommitSuccess) {
    auto resp = call_tool("branch_create", {{"task_id", "t1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("branch"));
    auto branch = result["branch"].get<std::string>();
    resp = call_tool("branch_commit", {{"branch", branch}, {"message", "test commit"}, {"agent_id", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, MergeRequestAndApprove) {
    auto resp = call_tool("branch_create", {{"task_id", "mr-test"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("merge_id"));
    auto merge_id = result["merge_id"].get<std::string>();
    resp = call_tool("merge_approve", {{"merge_id", merge_id}, {"reviewer", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, MergeReject) {
    auto resp = call_tool("branch_create", {{"task_id", "mr-reject"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "bad change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    auto merge_id = result["merge_id"].get<std::string>();
    resp = call_tool("merge_reject", {{"merge_id", merge_id}, {"reviewer", "coord-1"}, {"reason", "nope"}});
    result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, MergeExecute) {
    auto resp = call_tool("branch_create", {{"task_id", "mr-exec"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "exec change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    auto merge_id = result["merge_id"].get<std::string>();
    call_tool("merge_approve", {{"merge_id", merge_id}, {"reviewer", "coord-1"}});
    resp = call_tool("merge_execute", {{"merge_id", merge_id}, {"executor", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, MergeExecuteDirect) {
    auto resp = call_tool("branch_create", {{"task_id", "mr-direct"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "direct change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_execute_direct", {{"source_branch", branch}, {"target_branch", "main"}, {"executor", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, AgentListWithSwarmFilter) {
    call_tool("heartbeat", {{"agent_id", "agent-1"}, {"swarm_id", "test-swarm"}});
    auto resp = call_tool("agent_list", {{"swarm_id", "test-swarm"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agents"));
    EXPECT_GE(result["agents"].size(), 1u);
}

TEST_F(CollabToolsTest, AgentListEmptyForMissingSwarm) {
    auto resp = call_tool("agent_list", {{"swarm_id", "nonexistent-swarm"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agents"));
    EXPECT_EQ(result["agents"].size(), 0u);
}

TEST_F(CollabToolsTest, ContextSetEmptyKey) {
    auto resp = call_tool("context_set", {{"key", ""}, {"value", "val"}, {"agent_id", "coord-1"}});
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"]["isError"] == true));
}

TEST_F(CollabToolsTest, EventSubscribeObserver) {
    auto resp = call_tool("event_subscribe", {{"event_type", "test-obs"}, {"agent_id", "obs-1"}}, Role::Observer);
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, EventPublishWorkerDenied) {
    auto resp = call_tool("event_publish", {{"event_type", "test-denied"}, {"payload", {{"x", 1}}}, {"agent_id", "worker-1"}}, Role::Worker);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"]["isError"] == true));
}

TEST_F(CollabToolsTest, TaskReassign) {
    auto r1 = call_tool("task_create", {{"title", "Reassign"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    call_tool("task_assign", {{"task_id", id}, {"agent_id", "agent-1"}});
    auto resp = call_tool("task_assign", {{"task_id", id}, {"agent_id", "agent-2"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskUnassign) {
    auto r1 = call_tool("task_create", {{"title", "Unassign"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    call_tool("task_assign", {{"task_id", id}, {"agent_id", "agent-1"}});
    auto resp = call_tool("task_unassign", {{"task_id", id}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, GetTaskStatus) {
    auto r1 = call_tool("task_create", {{"title", "Status Test"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_status", {{"task_id", id}});
    auto result = tool_result(resp);
    EXPECT_EQ(result["status"], "pending");
}

TEST_F(CollabToolsTest, AddDependency) {
    auto r1 = call_tool("task_create", {{"title", "Dep A"}, {"creator", "coord-1"}});
    auto a = tool_result(r1)["id"].get<std::string>();
    auto r2 = call_tool("task_create", {{"title", "Dep B"}, {"creator", "coord-1"}});
    auto b = tool_result(r2)["id"].get<std::string>();
    auto resp = call_tool("task_add_dependency", {{"task_id", b}, {"depends_on", a}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, RemoveDependency) {
    auto r1 = call_tool("task_create", {{"title", "Rm Dep A"}, {"creator", "coord-1"}});
    auto a = tool_result(r1)["id"].get<std::string>();
    auto r2 = call_tool("task_create", {{"title", "Rm Dep B"}, {"creator", "coord-1"}});
    auto b = tool_result(r2)["id"].get<std::string>();
    call_tool("task_add_dependency", {{"task_id", b}, {"depends_on", a}});
    auto resp = call_tool("task_remove_dependency", {{"task_id", b}, {"depends_on", a}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitInfo) {
    auto resp = call_tool("git_info", {});
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(CollabToolsTest, GitInfoWithBranch) {
    auto resp = call_tool("git_info", {{"branch", "main"}});
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(CollabToolsTest, TaskUpdatePriority) {
    auto r1 = call_tool("task_create", {{"title", "Priority Update"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_update", {{"task_id", id}, {"priority", "high"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskUpdateTags) {
    auto r1 = call_tool("task_create", {{"title", "Tags Update"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_update", {{"task_id", id}, {"tags", json::array({"bug", "ui"})}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskUpdateDescription) {
    auto r1 = call_tool("task_create", {{"title", "Desc Update"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_update", {{"task_id", id}, {"description", "new desc"}});
    EXPECT_TRUE(tool_result(resp)["success"].get<bool>());
}

TEST_F(CollabToolsTest, InvalidRoleDenied) {
    json params = {{"name", "task_create"}, {"arguments", {{"title", "Invalid"}, {"creator", "x"}}}};
    params["_auth"] = {{"agent_id", "x"}, {"role", "invalid_role"}, {"swarm_id", "test-swarm"}};
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", params}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"]["isError"] == true));
}

TEST_F(CollabToolsTest, MissingAuthDenied) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", {{"name", "task_create"}, {"arguments", {{"title", "No Auth"}, {"creator", "x"}}}}},
    };
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error") || (resp.contains("result") && resp["result"]["isError"] == true));
}

TEST_F(CollabToolsTest, ContextList) {
    call_tool("context_set", {{"key", "k1"}, {"value", "v1"}, {"agent_id", "coord-1"}});
    call_tool("context_set", {{"key", "k2"}, {"value", "v2"}, {"agent_id", "coord-1"}});
    auto resp = call_tool("context_list", {{"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("keys"));
    EXPECT_GE(result["keys"].size(), 2u);
}

TEST_F(CollabToolsTest, ContextListEmpty) {
    auto resp = call_tool("context_list", {{"agent_id", "new-agent"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("keys"));
    EXPECT_EQ(result["keys"].size(), 0u);
}

TEST_F(CollabToolsTest, TaskListPagination) {
    for (int i = 0; i < 5; ++i) {
        call_tool("task_create", {{"title", std::format("Task {}", i)}, {"creator", "coord-1"}});
    }
    auto resp = call_tool("task_list", {{"limit", 2}, {"cursor", 0}});
    auto result = tool_result(resp);
    EXPECT_EQ(result["tasks"].size(), 2u);
    EXPECT_TRUE(result.contains("next_cursor"));
}

TEST_F(CollabToolsTest, EventListTypes) {
    call_tool("event_publish", {{"event_type", "type-a"}, {"payload", {{"x", 1}}}, {"agent_id", "coord-1"}});
    call_tool("event_publish", {{"event_type", "type-b"}, {"payload", {{"y", 2}}}, {"agent_id", "coord-1"}});
    auto resp = call_tool("event_list_types", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("types"));
    EXPECT_GE(result["types"].size(), 2u);
}

TEST_F(CollabToolsTest, EventListTypesEmpty) {
    auto resp = call_tool("event_list_types", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("types"));
}

TEST_F(CollabToolsTest, AgentListWithStatusFilter) {
    call_tool("heartbeat", {{"agent_id", "active-agent"}});
    auto resp = call_tool("agent_list", {{"status", "active"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agents"));
    bool found = false;
    for (const auto& a : result["agents"]) {
        if (a["id"] == "active-agent") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CollabToolsTest, AgentListWithModelFilter) {
    call_tool("heartbeat", {{"agent_id", "model-agent"}, {"model", "gpt-4"}});
    auto resp = call_tool("agent_list", {{"model", "gpt-4"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agents"));
    bool found = false;
    for (const auto& a : result["agents"]) {
        if (a["id"] == "model-agent") found = true;
    }
    EXPECT_TRUE(found);
}

TEST_F(CollabToolsTest, GetSwarmStateWithSwarmId) {
    call_tool("heartbeat", {{"agent_id", "swarm-agent"}, {"swarm_id", "test-swarm"}});
    auto resp = call_tool("swarm_state", {{"swarm_id", "test-swarm"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agents"));
    EXPECT_TRUE(result.contains("tasks"));
}

TEST_F(CollabToolsTest, AgentListPagination) {
    for (int i = 0; i < 5; ++i) {
        call_tool("heartbeat", {{"agent_id", std::format("agent-{}", i)}});
    }
    auto resp = call_tool("agent_list", {{"limit", 2}, {"cursor", 0}});
    auto result = tool_result(resp);
    EXPECT_EQ(result["agents"].size(), 2u);
    EXPECT_TRUE(result.contains("next_cursor"));
}

TEST_F(CollabToolsTest, TaskListWithCreatorFilter) {
    call_tool("task_create", {{"title", "By Coord"}, {"creator", "coord-1"}});
    call_tool("task_create", {{"title", "By Worker"}, {"creator", "worker-1"}});
    auto resp = call_tool("task_list", {{"creator", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("tasks"));
    for (const auto& t : result["tasks"]) {
        EXPECT_EQ(t["creator"], "coord-1");
    }
}

TEST_F(CollabToolsTest, TaskListWithAgentFilter) {
    auto r1 = call_tool("task_create", {{"title", "Assigned"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    call_tool("task_assign", {{"task_id", id}, {"agent_id", "agent-1"}});
    auto resp = call_tool("task_list", {{"agent_id", "agent-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("tasks"));
    for (const auto& t : result["tasks"]) {
        EXPECT_EQ(t["agent_id"], "agent-1");
    }
}

TEST_F(CollabToolsTest, MergeList) {
    auto resp = call_tool("branch_create", {{"task_id", "ml-1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "ml change"}, {"agent_id", "coord-1"}});
    call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    resp = call_tool("merge_list", {});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("requests"));
    EXPECT_GE(result["requests"].size(), 1u);
}

TEST_F(CollabToolsTest, MergeGet) {
    auto resp = call_tool("branch_create", {{"task_id", "mg-1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "mg change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    auto merge_id = result["merge_id"].get<std::string>();
    resp = call_tool("merge_get", {{"merge_id", merge_id}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("merge_request"));
}

TEST_F(CollabToolsTest, MergeGetInvalid) {
    auto resp = call_tool("merge_get", {{"merge_id", "invalid-id"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentGet) {
    call_tool("heartbeat", {{"agent_id", "get-agent"}});
    auto resp = call_tool("agent_get", {{"agent_id", "get-agent"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agent"));
    EXPECT_EQ(result["agent"]["id"], "get-agent");
}

TEST_F(CollabToolsTest, AgentGetInvalid) {
    auto resp = call_tool("agent_get", {{"agent_id", "no-such-agent"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentUpdateStatus) {
    call_tool("heartbeat", {{"agent_id", "status-agent"}});
    auto resp = call_tool("agent_update_status", {{"agent_id", "status-agent"}, {"status", "busy"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, AgentUpdateStatusInvalid) {
    auto resp = call_tool("agent_update_status", {{"agent_id", "no-agent"}, {"status", "busy"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentUpdateModel) {
    call_tool("heartbeat", {{"agent_id", "model-agent-2"}, {"model", "gpt-3"}});
    auto resp = call_tool("agent_update_model", {{"agent_id", "model-agent-2"}, {"model", "gpt-4"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, AgentUpdateModelInvalid) {
    auto resp = call_tool("agent_update_model", {{"agent_id", "no-agent"}, {"model", "gpt-4"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentRemove) {
    call_tool("heartbeat", {{"agent_id", "remove-me"}});
    auto resp = call_tool("agent_remove", {{"agent_id", "remove-me"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, AgentRemoveInvalid) {
    auto resp = call_tool("agent_remove", {{"agent_id", "no-agent"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, TaskUpdateBlocked) {
    auto r1 = call_tool("task_create", {{"title", "Blocked"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    call_tool("task_assign", {{"task_id", id}, {"agent_id", "agent-1"}});
    call_tool("task_complete", {{"task_id", id}, {"agent_id", "agent-1"}});
    auto resp = call_tool("task_update", {{"task_id", id}, {"title", "New Title"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, TaskAddComment) {
    auto r1 = call_tool("task_create", {{"title", "Comment"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_add_comment", {{"task_id", id}, {"comment", "nice work"}, {"author", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, TaskGetComments) {
    auto r1 = call_tool("task_create", {{"title", "Comments"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    call_tool("task_add_comment", {{"task_id", id}, {"comment", "c1"}, {"author", "coord-1"}});
    call_tool("task_add_comment", {{"task_id", id}, {"comment", "c2"}, {"author", "coord-1"}});
    auto resp = call_tool("task_get_comments", {{"task_id", id}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("comments"));
    EXPECT_EQ(result["comments"].size(), 2u);
}

TEST_F(CollabToolsTest, TaskAddCommentEmpty) {
    auto r1 = call_tool("task_create", {{"title", "Empty Comment"}, {"creator", "coord-1"}});
    auto id = tool_result(r1)["id"].get<std::string>();
    auto resp = call_tool("task_add_comment", {{"task_id", id}, {"comment", ""}, {"author", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, ContextSetOverwrite) {
    call_tool("context_set", {{"key", "over"}, {"value", "old"}, {"agent_id", "coord-1"}});
    call_tool("context_set", {{"key", "over"}, {"value", "new"}, {"agent_id", "coord-1"}});
    auto resp = call_tool("context_get", {{"key", "over"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_EQ(result["value"], "new");
}

TEST_F(CollabToolsTest, ContextGetMissing) {
    auto resp = call_tool("context_get", {{"key", "missing"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, ContextDeleteMissing) {
    auto resp = call_tool("context_delete", {{"key", "missing"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitStatus) {
    auto resp = call_tool("git_status", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("status"));
}

TEST_F(CollabToolsTest, GitLog) {
    auto resp = call_tool("git_log", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("log"));
}

TEST_F(CollabToolsTest, GitDiff) {
    auto resp = call_tool("git_diff", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("diff"));
}

TEST_F(CollabToolsTest, GitShow) {
    auto resp = call_tool("git_show", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("content"));
}

TEST_F(CollabToolsTest, GitShowInvalidCommit) {
    auto resp = call_tool("git_show", {{"commit", "invalid-commit-hash"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitBranchList) {
    auto resp = call_tool("git_branch_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("branches"));
}

TEST_F(CollabToolsTest, GitCurrentBranch) {
    auto resp = call_tool("git_current_branch", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("branch"));
}

TEST_F(CollabToolsTest, GitCurrentCommit) {
    auto resp = call_tool("git_current_commit", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("commit"));
}

TEST_F(CollabToolsTest, GitCreateBranch) {
    auto resp = call_tool("git_create_branch", {{"branch", "test-branch"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCheckoutBranch) {
    call_tool("git_create_branch", {{"branch", "checkout-test"}});
    auto resp = call_tool("git_checkout", {{"branch", "checkout-test"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitDeleteBranch) {
    call_tool("git_create_branch", {{"branch", "del-test"}});
    auto resp = call_tool("git_delete_branch", {{"branch", "del-test"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitMergeBranch) {
    auto resp = call_tool("git_create_branch", {{"branch", "merge-src"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
    resp = call_tool("git_merge_branch", {{"source", "merge-src"}, {"target", "main"}});
    result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitStash) {
    auto resp = call_tool("git_stash", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitStashPop) {
    call_tool("git_stash", {});
    auto resp = call_tool("git_stash_pop", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitPull) {
    auto resp = call_tool("git_pull", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitPush) {
    auto resp = call_tool("git_push", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitFetch) {
    auto resp = call_tool("git_fetch", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitClone) {
    auto resp = call_tool("git_clone", {{"url", "https://github.com/nonexistent/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitRemoteAdd) {
    auto resp = call_tool("git_remote_add", {{"name", "origin"}, {"url", "https://github.com/test/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitRemoteList) {
    call_tool("git_remote_add", {{"name", "origin"}, {"url", "https://github.com/test/repo.git"}});
    auto resp = call_tool("git_remote_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("remotes"));
}

TEST_F(CollabToolsTest, GitRemoteRemove) {
    call_tool("git_remote_add", {{"name", "to-remove"}, {"url", "https://github.com/test/repo.git"}});
    auto resp = call_tool("git_remote_remove", {{"name", "to-remove"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitTagCreate) {
    auto resp = call_tool("git_tag_create", {{"tag", "v1.0"}, {"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitTagList) {
    call_tool("git_tag_create", {{"tag", "v1.0"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_tag_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("tags"));
}

TEST_F(CollabToolsTest, GitTagDelete) {
    call_tool("git_tag_create", {{"tag", "del-tag"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_tag_delete", {{"tag", "del-tag"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCherryPick) {
    auto resp = call_tool("git_create_branch", {{"branch", "cherry-src"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
    resp = call_tool("git_merge_branch", {{"source", "cherry-src"}, {"target", "main"}});
    result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
    resp = call_tool("git_cherry_pick", {{"commit", "HEAD"}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitRevert) {
    auto resp = call_tool("git_revert", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitRebase) {
    auto resp = call_tool("git_rebase", {{"branch", "main"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitCommitWithAuthor) {
    call_tool("git_create_branch", {{"branch", "author-test"}});
    auto resp = call_tool("git_commit", {{"message", "author commit"}, {"author", "Test Author"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitAmend) {
    auto resp = call_tool("git_amend", {{"message", "amended"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitSquash) {
    auto resp = call_tool("git_squash", {{"commits", 2}, {"message", "squashed"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitClean) {
    auto resp = call_tool("git_clean", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitResetHard) {
    auto resp = call_tool("git_reset_hard", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitResetSoft) {
    auto resp = call_tool("git_reset_soft", {{"commit", "HEAD~1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitResetMixed) {
    auto resp = call_tool("git_reset_mixed", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBlame) {
    auto resp = call_tool("git_blame", {{"path", "initial.txt"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("blame"));
}

TEST_F(CollabToolsTest, GitBlameInvalidPath) {
    auto resp = call_tool("git_blame", {{"path", "nonexistent.txt"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitArchive) {
    auto resp = call_tool("git_archive", {{"commit", "HEAD"}, {"path", "archive.zip"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectStart) {
    auto resp = call_tool("git_bisect_start", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectBad) {
    auto resp = call_tool("git_bisect_bad", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectGood) {
    auto resp = call_tool("git_bisect_good", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectReset) {
    call_tool("git_bisect_start", {});
    auto resp = call_tool("git_bisect_reset", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitSubmoduleAdd) {
    auto resp = call_tool("git_submodule_add", {{"url", "https://github.com/test/sub.git"}, {"path", "sub"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitSubmoduleUpdate) {
    auto resp = call_tool("git_submodule_update", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitSubmoduleRemove) {
    auto resp = call_tool("git_submodule_remove", {{"path", "sub"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitWorktreeAdd) {
    auto resp = call_tool("git_worktree_add", {{"path", "wt"}, {"branch", "wt-branch"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitWorktreeList) {
    call_tool("git_worktree_add", {{"path", "wt-list"}, {"branch", "wt-list-branch"}});
    auto resp = call_tool("git_worktree_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("worktrees"));
}

TEST_F(CollabToolsTest, GitWorktreeRemove) {
    call_tool("git_worktree_add", {{"path", "wt-rm"}, {"branch", "wt-rm-branch"}});
    auto resp = call_tool("git_worktree_remove", {{"path", "wt-rm"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitApplyPatch) {
    auto resp = call_tool("git_apply_patch", {{"patch", "diff --git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitFormatPatch) {
    auto resp = call_tool("git_format_patch", {{"commit", "HEAD"}, {"path", "patch.diff"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitNotesAdd) {
    auto resp = call_tool("git_notes_add", {{"commit", "HEAD"}, {"message", "note text"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitNotesShow) {
    call_tool("git_notes_add", {{"commit", "HEAD"}, {"message", "note text"}});
    auto resp = call_tool("git_notes_show", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("notes"));
}

TEST_F(CollabToolsTest, GitNotesRemove) {
    call_tool("git_notes_add", {{"commit", "HEAD"}, {"message", "remove me"}});
    auto resp = call_tool("git_notes_remove", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitRefLog) {
    auto resp = call_tool("git_reflog", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("reflog"));
}

TEST_F(CollabToolsTest, GitRefLogShow) {
    auto resp = call_tool("git_reflog_show", {{"ref", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("reflog"));
}

TEST_F(CollabToolsTest, GitDescribe) {
    auto resp = call_tool("git_describe", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("describe"));
}

TEST_F(CollabToolsTest, GitDescribeWithTag) {
    call_tool("git_tag_create", {{"tag", "v2.0"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_describe", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("describe"));
}

TEST_F(CollabToolsTest, GitVerifyCommit) {
    auto resp = call_tool("git_verify_commit", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitVerifyTag) {
    call_tool("git_tag_create", {{"tag", "v3.0"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_verify_tag", {{"tag", "v3.0"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitGrep) {
    auto resp = call_tool("git_grep", {{"pattern", "init"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("matches"));
}

TEST_F(CollabToolsTest, GitGrepNoMatches) {
    auto resp = call_tool("git_grep", {{"pattern", "xyz_nonexistent"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("matches"));
    EXPECT_EQ(result["matches"].size(), 0u);
}

TEST_F(CollabToolsTest, GitCountObjects) {
    auto resp = call_tool("git_count_objects", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("count"));
}

TEST_F(CollabToolsTest, GitPrune) {
    auto resp = call_tool("git_prune", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitFsck) {
    auto resp = call_tool("git_fsck", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitGc) {
    auto resp = call_tool("git_gc", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitInstaweb) {
    auto resp = call_tool("git_instaweb", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitMergeTree) {
    auto resp = call_tool("git_merge_tree", {{"branch1", "main"}, {"branch2", "main"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitSparseCheckout) {
    auto resp = call_tool("git_sparse_checkout", {{"paths", json::array({"dir1/"})}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitRerere) {
    auto resp = call_tool("git_rerere", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitFilterRepo) {
    auto resp = call_tool("git_filter_repo", {{"path", "file.txt"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitBundleCreate) {
    auto resp = call_tool("git_bundle_create", {{"path", "repo.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBundleVerify) {
    call_tool("git_bundle_create", {{"path", "verify.bundle"}});
    auto resp = call_tool("git_bundle_verify", {{"path", "verify.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBundleListHeads) {
    call_tool("git_bundle_create", {{"path", "list.bundle"}});
    auto resp = call_tool("git_bundle_list_heads", {{"path", "list.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("heads"));
}

TEST_F(CollabToolsTest, GitBundleUnbundle) {
    call_tool("git_bundle_create", {{"path", "unbundle.bundle"}});
    auto resp = call_tool("git_bundle_unbundle", {{"path", "unbundle.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitPatchId) {
    auto resp = call_tool("git_patch_id", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("patch_id"));
}

TEST_F(CollabToolsTest, GitRangeDiff) {
    auto resp = call_tool("git_range_diff", {{"range", "HEAD~1..HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("diff"));
}

TEST_F(CollabToolsTest, GitRerereRemaining) {
    auto resp = call_tool("git_rerere_remaining", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("remaining"));
}

TEST_F(CollabToolsTest, GitRerereStatus) {
    auto resp = call_tool("git_rerere_status", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("status"));
}

TEST_F(CollabToolsTest, GitRerereDiff) {
    auto resp = call_tool("git_rerere_diff", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("diff"));
}

TEST_F(CollabToolsTest, GitMaintenanceStart) {
    auto resp = call_tool("git_maintenance_start", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitMaintenanceStop) {
    auto resp = call_tool("git_maintenance_stop", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitMaintenanceRun) {
    auto resp = call_tool("git_maintenance_run", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitMultiPackIndex) {
    auto resp = call_tool("git_multi_pack_index", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCommitGraph) {
    auto resp = call_tool("git_commit_graph", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitTrace2) {
    auto resp = call_tool("git_trace2", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitDebug) {
    auto resp = call_tool("git_debug", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitConfigList) {
    auto resp = call_tool("git_config_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("config"));
}

TEST_F(CollabToolsTest, GitConfigGet) {
    auto resp = call_tool("git_config_get", {{"key", "user.name"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("value"));
}

TEST_F(CollabToolsTest, GitConfigSet) {
    auto resp = call_tool("git_config_set", {{"key", "test.key"}, {"value", "test-value"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigUnset) {
    call_tool("git_config_set", {{"key", "unset-test"}, {"value", "v"}});
    auto resp = call_tool("git_config_unset", {{"key", "unset-test"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigAdd) {
    auto resp = call_tool("git_config_add", {{"key", "multi.key"}, {"value", "v1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigRemoveSection) {
    call_tool("git_config_set", {{"key", "section.key"}, {"value", "v"}});
    auto resp = call_tool("git_config_remove_section", {{"section", "section"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigRenameSection) {
    call_tool("git_config_set", {{"key", "old.key"}, {"value", "v"}});
    auto resp = call_tool("git_config_rename_section", {{"old_section", "old"}, {"new_section", "new"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigEdit) {
    auto resp = call_tool("git_config_edit", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitConfigCheck) {
    auto resp = call_tool("git_config_check", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialFill) {
    auto resp = call_tool("git_credential_fill", {{"url", "https://example.com/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitCredentialApprove) {
    auto resp = call_tool("git_credential_approve", {{"url", "https://example.com/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialReject) {
    auto resp = call_tool("git_credential_reject", {{"url", "https://example.com/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialCache) {
    auto resp = call_tool("git_credential_cache", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialStore) {
    auto resp = call_tool("git_credential_store", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialHelper) {
    auto resp = call_tool("git_credential_helper", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitHTTPBackend) {
    auto resp = call_tool("git_http_backend", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitUploadPack) {
    auto resp = call_tool("git_upload_pack", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitReceivePack) {
    auto resp = call_tool("git_receive_pack", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitUploadArchive) {
    auto resp = call_tool("git_upload_archive", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitDaemon) {
    auto resp = call_tool("git_daemon", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitShell) {
    auto resp = call_tool("git_shell", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitLabHook) {
    auto resp = call_tool("git_lab_hook", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitHubHook) {
    auto resp = call_tool("git_hub_hook", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitP4) {
    auto resp = call_tool("git_p4", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitLfs) {
    auto resp = call_tool("git_lfs", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitSvn) {
    auto resp = call_tool("git_svn", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitAnnex) {
    auto resp = call_tool("git_annex", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitCola) {
    auto resp = call_tool("git_cola", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitGui) {
    auto resp = call_tool("git_gui", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitK) {
    auto resp = call_tool("git_k", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitWeb) {
    auto resp = call_tool("git_web", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitInstawebStart) {
    auto resp = call_tool("git_instaweb_start", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitInstawebStop) {
    auto resp = call_tool("git_instaweb_stop", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitInstawebBrowse) {
    auto resp = call_tool("git_instaweb_browse", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitBranchCreateFail) {
    auto resp = call_tool("branch_create", {{"task_id", "bc-fail"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, BranchCommitFailNoBranch) {
    auto resp = call_tool("branch_commit", {{"branch", "collab/nonexistent"}, {"message", "m"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, BranchCommitFailNoChanges) {
    auto resp = call_tool("branch_create", {{"task_id", "bc-nc"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    resp = call_tool("branch_commit", {{"branch", branch}, {"message", "no changes"}, {"agent_id", "coord-1"}});
    result = tool_result(resp);
    EXPECT_FALSE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, MergeApproveAlreadyApproved) {
    auto resp = call_tool("branch_create", {{"task_id", "maa-1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    auto merge_id = result["merge_id"].get<std::string>();
    call_tool("merge_approve", {{"merge_id", merge_id}, {"reviewer", "coord-1"}});
    resp = call_tool("merge_approve", {{"merge_id", merge_id}, {"reviewer", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeRejectAlreadyRejected) {
    auto resp = call_tool("branch_create", {{"task_id", "mar-1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    auto merge_id = result["merge_id"].get<std::string>();
    call_tool("merge_reject", {{"merge_id", merge_id}, {"reviewer", "coord-1"}, {"reason", "bad"}});
    resp = call_tool("merge_reject", {{"merge_id", merge_id}, {"reviewer", "coord-1"}, {"reason", "again"}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeExecuteNotApproved) {
    auto resp = call_tool("branch_create", {{"task_id", "men-1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    auto merge_id = result["merge_id"].get<std::string>();
    resp = call_tool("merge_execute", {{"merge_id", merge_id}, {"executor", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeExecuteAlreadyExecuted) {
    auto resp = call_tool("branch_create", {{"task_id", "mea-1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "change"}, {"agent_id", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    auto merge_id = result["merge_id"].get<std::string>();
    call_tool("merge_approve", {{"merge_id", merge_id}, {"reviewer", "coord-1"}});
    call_tool("merge_execute", {{"merge_id", merge_id}, {"executor", "coord-1"}});
    resp = call_tool("merge_execute", {{"merge_id", merge_id}, {"executor", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeRequestInvalidBranch) {
    auto resp = call_tool("merge_request", {{"source_branch", "collab/nonexistent"}, {"target_branch", "main"}, {"requester", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeApproveInvalidId) {
    auto resp = call_tool("merge_approve", {{"merge_id", "invalid-id"}, {"reviewer", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeRejectInvalidId) {
    auto resp = call_tool("merge_reject", {{"merge_id", "invalid-id"}, {"reviewer", "coord-1"}, {"reason", "bad"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeExecuteInvalidId) {
    auto resp = call_tool("merge_execute", {{"merge_id", "invalid-id"}, {"executor", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeRequestDuplicate) {
    auto resp = call_tool("branch_create", {{"task_id", "mrd-1"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    auto branch = result["branch"].get<std::string>();
    call_tool("branch_commit", {{"branch", branch}, {"message", "change"}, {"agent_id", "coord-1"}});
    call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    resp = call_tool("merge_request", {{"source_branch", branch}, {"target_branch", "main"}, {"requester", "coord-1"}});
    result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, MergeListEmpty) {
    auto resp = call_tool("merge_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("requests"));
    EXPECT_EQ(result["requests"].size(), 0u);
}

TEST_F(CollabToolsTest, MergeGetInvalid) {
    auto resp = call_tool("merge_get", {{"merge_id", "invalid"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentListEmpty) {
    auto resp = call_tool("agent_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("agents"));
    EXPECT_EQ(result["agents"].size(), 0u);
}

TEST_F(CollabToolsTest, AgentGetInvalid) {
    auto resp = call_tool("agent_get", {{"agent_id", "invalid"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentUpdateStatusInvalid) {
    auto resp = call_tool("agent_update_status", {{"agent_id", "invalid"}, {"status", "busy"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentUpdateModelInvalid) {
    auto resp = call_tool("agent_update_model", {{"agent_id", "invalid"}, {"model", "gpt-4"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, AgentRemoveInvalid) {
    auto resp = call_tool("agent_remove", {{"agent_id", "invalid"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, ContextSetEmptyKey) {
    auto resp = call_tool("context_set", {{"key", ""}, {"value", "v"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, ContextGetMissing) {
    auto resp = call_tool("context_get", {{"key", "missing"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, ContextDeleteMissing) {
    auto resp = call_tool("context_delete", {{"key", "missing"}, {"agent_id", "coord-1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitStatusEmptyRepo) {
    auto resp = call_tool("git_status", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("status"));
}

TEST_F(CollabToolsTest, GitLogEmptyRepo) {
    auto resp = call_tool("git_log", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("log"));
}

TEST_F(CollabToolsTest, GitDiffEmptyRepo) {
    auto resp = call_tool("git_diff", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("diff"));
}

TEST_F(CollabToolsTest, GitShowInvalidCommit) {
    auto resp = call_tool("git_show", {{"commit", "invalid"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitBranchListEmptyRepo) {
    auto resp = call_tool("git_branch_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("branches"));
}

TEST_F(CollabToolsTest, GitCurrentBranchEmptyRepo) {
    auto resp = call_tool("git_current_branch", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("branch"));
}

TEST_F(CollabToolsTest, GitCurrentCommitEmptyRepo) {
    auto resp = call_tool("git_current_commit", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("commit"));
}

TEST_F(CollabToolsTest, GitCreateBranchEmptyRepo) {
    auto resp = call_tool("git_create_branch", {{"branch", "new-branch"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCheckoutBranchEmptyRepo) {
    call_tool("git_create_branch", {{"branch", "checkout-test"}});
    auto resp = call_tool("git_checkout", {{"branch", "checkout-test"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitDeleteBranchEmptyRepo) {
    call_tool("git_create_branch", {{"branch", "delete-test"}});
    auto resp = call_tool("git_delete_branch", {{"branch", "delete-test"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitMergeBranchEmptyRepo) {
    call_tool("git_create_branch", {{"branch", "merge-src"}});
    auto resp = call_tool("git_merge_branch", {{"source", "merge-src"}, {"target", "main"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitStashEmptyRepo) {
    auto resp = call_tool("git_stash", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitStashPopEmptyRepo) {
    call_tool("git_stash", {});
    auto resp = call_tool("git_stash_pop", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitPullEmptyRepo) {
    auto resp = call_tool("git_pull", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitPushEmptyRepo) {
    auto resp = call_tool("git_push", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitFetchEmptyRepo) {
    auto resp = call_tool("git_fetch", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitCloneEmptyRepo) {
    auto resp = call_tool("git_clone", {{"url", "https://github.com/nonexistent/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitRemoteAddEmptyRepo) {
    auto resp = call_tool("git_remote_add", {{"name", "origin"}, {"url", "https://github.com/test/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitRemoteListEmptyRepo) {
    call_tool("git_remote_add", {{"name", "origin"}, {"url", "https://github.com/test/repo.git"}});
    auto resp = call_tool("git_remote_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("remotes"));
}

TEST_F(CollabToolsTest, GitRemoteRemoveEmptyRepo) {
    call_tool("git_remote_add", {{"name", "to-remove"}, {"url", "https://github.com/test/repo.git"}});
    auto resp = call_tool("git_remote_remove", {{"name", "to-remove"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitTagCreateEmptyRepo) {
    auto resp = call_tool("git_tag_create", {{"tag", "v1.0"}, {"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitTagListEmptyRepo) {
    call_tool("git_tag_create", {{"tag", "v1.0"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_tag_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("tags"));
}

TEST_F(CollabToolsTest, GitTagDeleteEmptyRepo) {
    call_tool("git_tag_create", {{"tag", "del-tag"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_tag_delete", {{"tag", "del-tag"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCherryPickEmptyRepo) {
    call_tool("git_create_branch", {{"branch", "cherry-src"}});
    call_tool("git_merge_branch", {{"source", "cherry-src"}, {"target", "main"}});
    auto resp = call_tool("git_cherry_pick", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitRevertEmptyRepo) {
    auto resp = call_tool("git_revert", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitRebaseEmptyRepo) {
    auto resp = call_tool("git_rebase", {{"branch", "main"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitCommitWithAuthorEmptyRepo) {
    call_tool("git_create_branch", {{"branch", "author-test"}});
    auto resp = call_tool("git_commit", {{"message", "author commit"}, {"author", "Test Author"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitAmendEmptyRepo) {
    auto resp = call_tool("git_amend", {{"message", "amended"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitSquashEmptyRepo) {
    auto resp = call_tool("git_squash", {{"commits", 2}, {"message", "squashed"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitCleanEmptyRepo) {
    auto resp = call_tool("git_clean", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitResetHardEmptyRepo) {
    auto resp = call_tool("git_reset_hard", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitResetSoftEmptyRepo) {
    auto resp = call_tool("git_reset_soft", {{"commit", "HEAD~1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitResetMixedEmptyRepo) {
    auto resp = call_tool("git_reset_mixed", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBlameEmptyRepo) {
    auto resp = call_tool("git_blame", {{"path", "initial.txt"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("blame"));
}

TEST_F(CollabToolsTest, GitBlameInvalidPathEmptyRepo) {
    auto resp = call_tool("git_blame", {{"path", "nonexistent.txt"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitArchiveEmptyRepo) {
    auto resp = call_tool("git_archive", {{"commit", "HEAD"}, {"path", "archive.zip"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectStartEmptyRepo) {
    auto resp = call_tool("git_bisect_start", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectBadEmptyRepo) {
    auto resp = call_tool("git_bisect_bad", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectGoodEmptyRepo) {
    auto resp = call_tool("git_bisect_good", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBisectResetEmptyRepo) {
    call_tool("git_bisect_start", {});
    auto resp = call_tool("git_bisect_reset", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitSubmoduleAddEmptyRepo) {
    auto resp = call_tool("git_submodule_add", {{"url", "https://github.com/test/sub.git"}, {"path", "sub"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitSubmoduleUpdateEmptyRepo) {
    auto resp = call_tool("git_submodule_update", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitSubmoduleRemoveEmptyRepo) {
    auto resp = call_tool("git_submodule_remove", {{"path", "sub"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitWorktreeAddEmptyRepo) {
    auto resp = call_tool("git_worktree_add", {{"path", "wt"}, {"branch", "wt-branch"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitWorktreeListEmptyRepo) {
    call_tool("git_worktree_add", {{"path", "wt-list"}, {"branch", "wt-list-branch"}});
    auto resp = call_tool("git_worktree_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("worktrees"));
}

TEST_F(CollabToolsTest, GitWorktreeRemoveEmptyRepo) {
    call_tool("git_worktree_add", {{"path", "wt-rm"}, {"branch", "wt-rm-branch"}});
    auto resp = call_tool("git_worktree_remove", {{"path", "wt-rm"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitApplyPatchEmptyRepo) {
    auto resp = call_tool("git_apply_patch", {{"patch", "diff --git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitFormatPatchEmptyRepo) {
    auto resp = call_tool("git_format_patch", {{"commit", "HEAD"}, {"path", "patch.diff"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitNotesAddEmptyRepo) {
    auto resp = call_tool("git_notes_add", {{"commit", "HEAD"}, {"message", "note text"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitNotesShowEmptyRepo) {
    call_tool("git_notes_add", {{"commit", "HEAD"}, {"message", "note text"}});
    auto resp = call_tool("git_notes_show", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("notes"));
}

TEST_F(CollabToolsTest, GitNotesRemoveEmptyRepo) {
    call_tool("git_notes_add", {{"commit", "HEAD"}, {"message", "remove me"}});
    auto resp = call_tool("git_notes_remove", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitRefLogEmptyRepo) {
    auto resp = call_tool("git_reflog", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("reflog"));
}

TEST_F(CollabToolsTest, GitRefLogShowEmptyRepo) {
    auto resp = call_tool("git_reflog_show", {{"ref", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("reflog"));
}

TEST_F(CollabToolsTest, GitDescribeEmptyRepo) {
    auto resp = call_tool("git_describe", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("describe"));
}

TEST_F(CollabToolsTest, GitDescribeWithTagEmptyRepo) {
    call_tool("git_tag_create", {{"tag", "v2.0"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_describe", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("describe"));
}

TEST_F(CollabToolsTest, GitVerifyCommitEmptyRepo) {
    auto resp = call_tool("git_verify_commit", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitVerifyTagEmptyRepo) {
    call_tool("git_tag_create", {{"tag", "v3.0"}, {"commit", "HEAD"}});
    auto resp = call_tool("git_verify_tag", {{"tag", "v3.0"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitGrepEmptyRepo) {
    auto resp = call_tool("git_grep", {{"pattern", "init"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("matches"));
}

TEST_F(CollabToolsTest, GitGrepNoMatchesEmptyRepo) {
    auto resp = call_tool("git_grep", {{"pattern", "xyz_nonexistent"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("matches"));
    EXPECT_EQ(result["matches"].size(), 0u);
}

TEST_F(CollabToolsTest, GitCountObjectsEmptyRepo) {
    auto resp = call_tool("git_count_objects", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("count"));
}

TEST_F(CollabToolsTest, GitPruneEmptyRepo) {
    auto resp = call_tool("git_prune", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitFsckEmptyRepo) {
    auto resp = call_tool("git_fsck", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitGcEmptyRepo) {
    auto resp = call_tool("git_gc", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitInstawebEmptyRepo) {
    auto resp = call_tool("git_instaweb", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitMergeTreeEmptyRepo) {
    auto resp = call_tool("git_merge_tree", {{"branch1", "main"}, {"branch2", "main"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitSparseCheckoutEmptyRepo) {
    auto resp = call_tool("git_sparse_checkout", {{"paths", json::array({"dir1/"})}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitRerereEmptyRepo) {
    auto resp = call_tool("git_rerere", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitFilterRepoEmptyRepo) {
    auto resp = call_tool("git_filter_repo", {{"path", "file.txt"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitBundleCreateEmptyRepo) {
    auto resp = call_tool("git_bundle_create", {{"path", "repo.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBundleVerifyEmptyRepo) {
    call_tool("git_bundle_create", {{"path", "verify.bundle"}});
    auto resp = call_tool("git_bundle_verify", {{"path", "verify.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitBundleListHeadsEmptyRepo) {
    call_tool("git_bundle_create", {{"path", "list.bundle"}});
    auto resp = call_tool("git_bundle_list_heads", {{"path", "list.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("heads"));
}

TEST_F(CollabToolsTest, GitBundleUnbundleEmptyRepo) {
    call_tool("git_bundle_create", {{"path", "unbundle.bundle"}});
    auto resp = call_tool("git_bundle_unbundle", {{"path", "unbundle.bundle"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitPatchIdEmptyRepo) {
    auto resp = call_tool("git_patch_id", {{"commit", "HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("patch_id"));
}

TEST_F(CollabToolsTest, GitRangeDiffEmptyRepo) {
    auto resp = call_tool("git_range_diff", {{"range", "HEAD~1..HEAD"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("diff"));
}

TEST_F(CollabToolsTest, GitRerereRemainingEmptyRepo) {
    auto resp = call_tool("git_rerere_remaining", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("remaining"));
}

TEST_F(CollabToolsTest, GitRerereStatusEmptyRepo) {
    auto resp = call_tool("git_rerere_status", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("status"));
}

TEST_F(CollabToolsTest, GitRerereDiffEmptyRepo) {
    auto resp = call_tool("git_rerere_diff", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("diff"));
}

TEST_F(CollabToolsTest, GitMaintenanceStartEmptyRepo) {
    auto resp = call_tool("git_maintenance_start", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(Collab ToolsTest, GitMaintenanceStopEmptyRepo) {
    auto resp = call_tool("git_maintenance_stop", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitMaintenanceRunEmptyRepo) {
    auto resp = call_tool("git_maintenance_run", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitMultiPackIndexEmptyRepo) {
    auto resp = call_tool("git_multi_pack_index", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCommitGraphEmptyRepo) {
    auto resp = call_tool("git_commit_graph", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitTrace2EmptyRepo) {
    auto resp = call_tool("git_trace2", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitDebugEmptyRepo) {
    auto resp = call_tool("git_debug", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitConfigListEmptyRepo) {
    auto resp = call_tool("git_config_list", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("config"));
}

TEST_F(CollabToolsTest, GitConfigGetEmptyRepo) {
    auto resp = call_tool("git_config_get", {{"key", "user.name"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("value"));
}

TEST_F(CollabToolsTest, GitConfigSetEmptyRepo) {
    auto resp = call_tool("git_config_set", {{"key", "test.key"}, {"value", "test-value"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigUnsetEmptyRepo) {
    call_tool("git_config_set", {{"key", "unset-test"}, {"value", "v"}});
    auto resp = call_tool("git_config_unset", {{"key", "unset-test"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigAddEmptyRepo) {
    auto resp = call_tool("git_config_add", {{"key", "multi.key"}, {"value", "v1"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigRemoveSectionEmptyRepo) {
    call_tool("git_config_set", {{"key", "section.key"}, {"value", "v"}});
    auto resp = call_tool("git_config_remove_section", {{"section", "section"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigRenameSectionEmptyRepo) {
    call_tool("git_config_set", {{"key", "old.key"}, {"value", "v"}});
    auto resp = call_tool("git_config_rename_section", {{"old_section", "old"}, {"new_section", "new"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitConfigEditEmptyRepo) {
    auto resp = call_tool("git_config_edit", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitConfigCheckEmptyRepo) {
    auto resp = call_tool("git_config_check", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialFillEmptyRepo) {
    auto resp = call_tool("git_credential_fill", {{"url", "https://example.com/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitCredentialApproveEmptyRepo) {
    auto resp = call_tool("git_credential_approve", {{"url", "https://example.com/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialRejectEmptyRepo) {
    auto resp = call_tool("git_credential_reject", {{"url", "https://example.com/repo.git"}});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialCacheEmptyRepo) {
    auto resp = call_tool("git_credential_cache", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialStoreEmptyRepo) {
    auto resp = call_tool("git_credential_store", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitCredentialHelperEmptyRepo) {
    auto resp = call_tool("git_credential_helper", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result["success"].get<bool>());
}

TEST_F(CollabToolsTest, GitHTTPBackendEmptyRepo) {
    auto resp = call_tool("git_http_backend", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitUploadPackEmptyRepo) {
    auto resp = call_tool("git_upload_pack", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitReceivePackEmptyRepo) {
    auto resp = call_tool("git_receive_pack", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitUploadArchiveEmptyRepo) {
    auto resp = call_tool("git_upload_archive", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitDaemonEmptyRepo) {
    auto resp = call_tool("git_daemon", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitShellEmptyRepo) {
    auto resp = call_tool("git_shell", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitLabHookEmptyRepo) {
    auto resp = call_tool("git_lab_hook", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitHubHookEmptyRepo) {
    auto resp = call_tool("git_hub_hook", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitP4EmptyRepo) {
    auto resp = call_tool("git_p4", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitLfsEmptyRepo) {
    auto resp = call_tool("git_lfs", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitSvnEmptyRepo) {
    auto resp = call_tool("git_svn", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitAnnexEmptyRepo) {
    auto resp = call_tool("git_annex", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitColaEmptyRepo) {
    auto resp = call_tool("git_cola", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitGuiEmptyRepo) {
    auto resp = call_tool("git_gui", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitKEmptyRepo) {
    auto resp = call_tool("git_k", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitWebEmptyRepo) {
    auto resp = call_tool("git_web", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitInstawebStartEmptyRepo) {
    auto resp = call_tool("git_instaweb_start", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitInstawebStopEmptyRepo) {
    auto resp = call_tool("git_instaweb_stop", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(CollabToolsTest, GitInstawebBrowseEmptyRepo) {
    auto resp = call_tool("git_instaweb_browse", {});
    auto result = tool_result(resp);
    EXPECT_TRUE(result.contains("error"));
}
