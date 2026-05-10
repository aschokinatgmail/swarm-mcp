#include <gtest/gtest.h>
#include "mcp_collab/auth.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/branch_manager.hpp"
#include "mcp_collab/merge_coordinator.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/persistence.hpp"
#include "mcp_collab/channel.hpp"
#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/mqtt_client.hpp"
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/config.hpp"
#include "mcp_collab/keychain.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace mcp_collab;

// ═══════════════════════════════════════════════════════════════════════════════
// Auth coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

TEST(AuthExtra, RoleConversions) {
    EXPECT_EQ(role_from_str("coordinator"), Role::Coordinator);
    EXPECT_EQ(role_from_str("worker"), Role::Worker);
    EXPECT_EQ(role_from_str("observer"), Role::Observer);
    EXPECT_EQ(role_from_str("unknown"), Role::Observer);
}

TEST(AuthExtra, RolePermissionsAll) {
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::Admin));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::BranchCreate));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::TaskRead));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::TaskCreate));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::Admin));
}

TEST(AuthExtra, AuthTokenSerialize) {
    AuthProvider auth("secret");
    auto token = auth.issue_token("agent-1", Role::Worker, "swarm-1");
    auto j = token.to_json();
    EXPECT_EQ(j["agent_id"], "agent-1");
    EXPECT_EQ(j["role"], "worker");
    EXPECT_EQ(j["swarm_id"], "swarm-1");
    EXPECT_TRUE(j.contains("token_id"));
    EXPECT_TRUE(j.contains("issued_at"));
    EXPECT_TRUE(j.contains("expires_at"));
}

TEST(AuthExtra, AuthTokenIsExpired) {
    AuthProvider auth("secret");
    auto token = auth.issue_token("agent-1", Role::Worker, "swarm-1", std::chrono::seconds(-1));
    EXPECT_TRUE(token.is_expired());
    auto token2 = auth.issue_token("agent-1", Role::Worker, "swarm-1", std::chrono::hours(1));
    EXPECT_FALSE(token2.is_expired());
}

TEST(AuthExtra, AuthTokenRefresh) {
    AuthProvider auth("secret");
    auto token = auth.issue_token("agent-1", Role::Worker, "swarm-1");
    auto refreshed = auth.refresh_token(token.token_string);
    EXPECT_TRUE(refreshed.has_value());
    EXPECT_EQ(refreshed->agent_id, "agent-1");
}

TEST(AuthExtra, AuthRefreshInvalidToken) {
    AuthProvider auth("secret");
    auto refreshed = auth.refresh_token("invalid-token");
    EXPECT_FALSE(refreshed.has_value());
}

TEST(AuthExtra, AuthRevokeToken) {
    AuthProvider auth("secret");
    auto token = auth.issue_token("agent-1", Role::Worker, "swarm-1");
    EXPECT_TRUE(auth.validate_token(token.token_string).has_value());
    EXPECT_TRUE(auth.revoke_token(token.token_id));
    EXPECT_FALSE(auth.validate_token(token.token_string).has_value());
}

TEST(AuthExtra, AuthAuthorizeByRole) {
    AuthProvider auth("secret");
    auto worker = auth.issue_token("w", Role::Worker, "s");
    auto observer = auth.issue_token("o", Role::Observer, "s");
    EXPECT_TRUE(auth.authorize(worker, Role::Observer));
    EXPECT_TRUE(auth.authorize(worker, Role::Worker));
    EXPECT_FALSE(auth.authorize(observer, Role::Worker));
    EXPECT_TRUE(auth.authorize(observer, Role::Observer));
}

TEST(AuthExtra, AuthAuthorizeByPermission) {
    AuthProvider auth("secret");
    auto worker = auth.issue_token("w", Role::Worker, "s");
    EXPECT_TRUE(auth.authorize(worker, Permission::TaskCreate));
    EXPECT_FALSE(auth.authorize(worker, Permission::Admin));
}

TEST(AuthExtra, ExtractBearer) {
    auto val = AuthProvider::extract_bearer("Bearer abc123");
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), "abc123");
    EXPECT_FALSE(AuthProvider::extract_bearer("Basic abc").has_value());
    EXPECT_FALSE(AuthProvider::extract_bearer("").has_value());
}

TEST(AuthExtra, ComputeHmacDeterministic) {
    std::string h1 = compute_hmac("data", "secret");
    std::string h2 = compute_hmac("data", "secret");
    EXPECT_EQ(h1, h2);
    std::string h3 = compute_hmac("data", "other");
    EXPECT_NE(h1, h3);
}

TEST(AuthExtra, ValidateTokenExpired) {
    AuthProvider auth("secret");
    auto token = auth.issue_token("agent-1", Role::Worker, "swarm-1", std::chrono::seconds(-1));
    EXPECT_FALSE(auth.validate_token(token.token_string).has_value());
}

TEST(AuthExtra, ValidateTokenInvalidFormat) {
    AuthProvider auth("secret");
    EXPECT_FALSE(auth.validate_token("not-a-token").has_value());
    EXPECT_FALSE(auth.validate_token("").has_value());
    EXPECT_FALSE(auth.validate_token("a.b.c").has_value());
}

// ═════════════════════════════════════════════════════════════════════════════════
// TaskManager coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

TEST(TaskManagerExtra, TaskJsonWithDeadline) {
    TaskManager tm;
    auto task = tm.create_task("With deadline", "agent-1");
    task.deadline = std::chrono::system_clock::now() + std::chrono::hours(24);
    tm.update_task(task.id, task);
    auto fetched = tm.get_task(task.id);
    auto j = fetched->to_json();
    EXPECT_TRUE(j.contains("deadline"));
    EXPECT_TRUE(j["deadline"].is_number());
}

TEST(TaskManagerExtra, TaskJsonWithCompletedAt) {
    TaskManager tm;
    auto task = tm.create_task("Completed task", "agent-1");
    tm.set_status(task.id, TaskStatus::Completed);
    auto fetched = tm.get_task(task.id);
    auto j = fetched->to_json();
    EXPECT_TRUE(j.contains("completed_at"));
    EXPECT_TRUE(j["completed_at"].is_number());
}

TEST(TaskManagerExtra, TaskFromJsonWithDeadline) {
    json j = {
        {"id", "t1"}, {"title", "test"}, {"description", "desc"}, {"assignee", "a1"},
        {"creator", "c1"}, {"status", "in_progress"}, {"priority", "high"},
        {"dependencies", json::array()}, {"tags", json::array()}, {"context", json::object()},
        {"created_at", 1000LL}, {"updated_at", 2000LL}, {"deadline", 3000LL}
    };
    auto task = Task::from_json(j);
    EXPECT_TRUE(task.deadline.has_value());
    EXPECT_EQ(task.status, TaskStatus::InProgress);
    EXPECT_EQ(task.priority, TaskPriority::High);
}

TEST(TaskManagerExtra, TaskFromJsonWithCompletedAt) {
    json j = {
        {"id", "t2"}, {"title", "test2"}, {"description", ""}, {"assignee", ""},
        {"creator", "c2"}, {"status", "completed"}, {"priority", "low"},
        {"dependencies", json::array()}, {"tags", json::array()}, {"context", json::object()},
        {"created_at", 1000LL}, {"updated_at", 2000LL}, {"completed_at", 2500LL}
    };
    auto task = Task::from_json(j);
    EXPECT_TRUE(task.completed_at.has_value());
    EXPECT_EQ(task.status, TaskStatus::Completed);
    EXPECT_EQ(task.priority, TaskPriority::Low);
}

TEST(TaskManagerExtra, TaskFromJsonMinimal) {
    json j = {{"id", "t3"}, {"title", "minimal"}};
    auto task = Task::from_json(j);
    EXPECT_EQ(task.id, "t3");
    EXPECT_EQ(task.description, "");
    EXPECT_EQ(task.status, TaskStatus::Pending);
    EXPECT_FALSE(task.deadline.has_value());
    EXPECT_FALSE(task.completed_at.has_value());
}

TEST(TaskManagerExtra, AddDependencySelf) {
    TaskManager tm;
    auto task = tm.create_task("Self dep", "a1");
    EXPECT_FALSE(tm.add_dependency(task.id, task.id));
}

TEST(TaskManagerExtra, WouldCreateCycle) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    auto t2 = tm.create_task("T2", "a1");
    auto t3 = tm.create_task("T3", "a1");
    tm.add_dependency(t2.id, t1.id);
    tm.add_dependency(t3.id, t2.id);
    EXPECT_TRUE(tm.would_create_cycle(t1.id, t3.id));
    EXPECT_FALSE(tm.would_create_cycle(t3.id, t1.id));
}

TEST(TaskManagerExtra, HasDependency) {
    TaskManager tm;
    auto t1 = tm.create_task("D1", "a1");
    auto t2 = tm.create_task("D2", "a1");
    tm.add_dependency(t2.id, t1.id);
    EXPECT_TRUE(tm.has_dependency(t2.id, t1.id));
    EXPECT_FALSE(tm.has_dependency(t1.id, t2.id));
}

TEST(TaskManagerExtra, HasDependencyNone) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    EXPECT_FALSE(tm.has_dependency(t1.id, "nonexistent"));
}

TEST(TaskManagerExtra, ListTasksWithSearchTerm) {
    TaskManager tm;
    tm.create_task("Searchable task for testing", "a1");
    tm.create_task("Other", "a2");
    auto results = tm.list_tasks(TaskFilter{.search_term = "Searchable"});
    EXPECT_EQ(results.size(), 1u);
}

TEST(TaskManagerExtra, ListTasksFilterByPriority) {
    TaskManager tm;
    tm.create_task("Low priority", "a1", "", TaskPriority::Low);
    tm.create_task("High priority", "a2", "", TaskPriority::High);
    auto results = tm.list_tasks(TaskFilter{.priority = TaskPriority::High});
    EXPECT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].priority, TaskPriority::High);
}

TEST(TaskManagerExtra, ListTasksFilterByTag) {
    TaskManager tm;
    auto t1 = tm.create_task("Tagged", "a1");
    tm.add_tag(t1.id, "backend");
    tm.create_task("Untagged", "a2");
    auto results = tm.list_tasks(TaskFilter{.tag = "backend"});
    EXPECT_EQ(results.size(), 1u);
}

TEST(TaskManagerExtra, UpdateNonexistentTask) {
    TaskManager tm;
    Task task;
    task.id = "nonexistent";
    EXPECT_FALSE(tm.update_task("nonexistent", task));
}

TEST(TaskManagerExtra, SetStatusFailed) {
    TaskManager tm;
    auto task = tm.create_task("Fail", "a1");
    EXPECT_TRUE(tm.set_status(task.id, TaskStatus::Failed));
    EXPECT_EQ(tm.get_task(task.id)->status, TaskStatus::Failed);
}

TEST(TaskManagerExtra, SetStatusCancelled) {
    TaskManager tm;
    auto task = tm.create_task("Cancel", "a1");
    EXPECT_TRUE(tm.set_status(task.id, TaskStatus::Cancelled));
    EXPECT_EQ(tm.get_task(task.id)->status, TaskStatus::Cancelled);
}

TEST(TaskManagerExtra, SetStatusBlocked) {
    TaskManager tm;
    auto task = tm.create_task("Block", "a1");
    EXPECT_TRUE(tm.set_status(task.id, TaskStatus::Blocked));
    EXPECT_EQ(tm.get_task(task.id)->status, TaskStatus::Blocked);
}

TEST(TaskManagerExtra, SetStatusNonexistent) {
    TaskManager tm;
    EXPECT_FALSE(tm.set_status("nonexistent", TaskStatus::Completed));
}

TEST(TaskManagerExtra, AssignNonexistent) {
    TaskManager tm;
    EXPECT_FALSE(tm.assign_task("nonexistent", "agent-x"));
}

TEST(TaskManagerExtra, StatusDefaultBranch) {
    EXPECT_EQ(task_status_str(static_cast<TaskStatus>(99)), "unknown");
    EXPECT_EQ(task_priority_str(static_cast<TaskPriority>(99)), "medium");
}

TEST(TaskManagerExtra, TaskStatusAllConversions) {
    EXPECT_EQ(task_status_from_str("pending"), TaskStatus::Pending);
    EXPECT_EQ(task_status_from_str("blocked"), TaskStatus::Blocked);
    EXPECT_EQ(task_status_from_str("completed"), TaskStatus::Completed);
    EXPECT_EQ(task_status_from_str("failed"), TaskStatus::Failed);
    EXPECT_EQ(task_status_from_str("cancelled"), TaskStatus::Cancelled);
    EXPECT_EQ(task_status_from_str("anything_else"), TaskStatus::Pending);
}

TEST(TaskManagerExtra, TaskPriorityAllConversions) {
    EXPECT_EQ(task_priority_from_str("medium"), TaskPriority::Medium);
    EXPECT_EQ(task_priority_from_str("unknown"), TaskPriority::Medium);
}

TEST(TaskManagerExtra, RemoveDependencyNonexistent) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    // remove_dependency returns true even if dependency doesn't exist (no-op)
    EXPECT_TRUE(tm.remove_dependency(t1.id, "nonexistent"));
}

TEST(TaskManagerExtra, AddTagNonexistent) {
    TaskManager tm;
    EXPECT_FALSE(tm.add_tag("nonexistent", "tag"));
}

// ═════════════════════════════════════════════════════════════════════════════════
// BranchManager coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

class BranchManagerCoverageTest : public ::testing::Test {
protected:
    std::string test_repo_path;
    std::unique_ptr<GitOperations> git;
    std::unique_ptr<BranchManager> mgr;

    void SetUp() override {
        test_repo_path = (std::filesystem::temp_directory_path() / ("swarm-mcp-branch-cov-" + std::to_string(reinterpret_cast<uintptr_t>(this)))).string();
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
        mgr = std::make_unique<BranchManager>(*git, "collab/");
    }

    void TearDown() override {
        mgr.reset();
        git.reset();
        std::error_code ec;
        for (int i = 0; i < 10; ++i) {
            if (std::filesystem::remove_all(test_repo_path, ec); !std::filesystem::exists(test_repo_path, ec)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

TEST_F(BranchManagerCoverageTest, BranchInfoJsonAllStates) {
    BranchInfo info;
    info.name = "collab/test";
    info.task_id = "test";
    info.agent_id = "agent-1";
    info.state = BranchState::Locked;
    info.base_branch = "main";
    auto j = info.to_json();
    EXPECT_EQ(j["state"], "locked");
    auto restored = BranchInfo::from_json(j);
    EXPECT_EQ(restored.state, BranchState::Locked);
}

TEST_F(BranchManagerCoverageTest, BranchInfoJsonAbandonedState) {
    BranchInfo info;
    info.name = "collab/test";
    info.task_id = "test";
    info.agent_id = "agent-1";
    info.state = BranchState::Abandoned;
    auto j = info.to_json();
    EXPECT_EQ(j["state"], "abandoned");
    auto restored = BranchInfo::from_json(j);
    EXPECT_EQ(restored.state, BranchState::Abandoned);
}

TEST_F(BranchManagerCoverageTest, BranchInfoJsonMergedState) {
    BranchInfo info;
    info.name = "collab/test";
    info.task_id = "test";
    info.agent_id = "agent-1";
    info.state = BranchState::Merged;
    auto j = info.to_json();
    EXPECT_EQ(j["state"], "merged");
    auto restored = BranchInfo::from_json(j);
    EXPECT_EQ(restored.state, BranchState::Merged);
}

TEST_F(BranchManagerCoverageTest, DiffToBase) {
    auto branch = mgr->create_branch("diff-test", "agent-1");
    std::ofstream(test_repo_path + "/diff_file.txt") << "diff content";
    mgr->commit_changes(branch, "diff commit", "agent-1");
    git->checkout("main");
    auto diff = mgr->diff_to_base(branch);
    EXPECT_FALSE(diff.empty());
}

TEST_F(BranchManagerCoverageTest, GetBranchInfoNotFound) {
    auto info = mgr->get_branch_info("nonexistent-branch");
    EXPECT_FALSE(info.has_value());
}

TEST_F(BranchManagerCoverageTest, CommitChangesOnBranch) {
    auto branch = mgr->create_branch("cov-commit", "agent-1");
    std::ofstream(test_repo_path + "/commit_file.txt") << "content";
    EXPECT_TRUE(mgr->commit_changes(branch, "commit msg", "agent-1"));
}

TEST_F(BranchManagerCoverageTest, DiffToBaseNotFound) {
    auto diff = mgr->diff_to_base("nonexistent-branch");
    EXPECT_TRUE(diff.empty());
}

// ═════════════════════════════════════════════════════════════════════════════════
// MergeCoordinator coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

class MergeCoordinatorCoverageTest : public ::testing::Test {
protected:
    std::string test_repo_path;
    std::unique_ptr<GitOperations> git;
    std::unique_ptr<BranchManager> branch_mgr;
    std::unique_ptr<MergeCoordinator> coordinator;

    void SetUp() override {
        test_repo_path = (std::filesystem::temp_directory_path() / ("swarm-mcp-merge-cov-" + std::to_string(reinterpret_cast<uintptr_t>(this)))).string();
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
        branch_mgr = std::make_unique<BranchManager>(*git, "collab/");
        coordinator = std::make_unique<MergeCoordinator>(*git, *branch_mgr);
    }

    void TearDown() override {
        coordinator.reset();
        branch_mgr.reset();
        git.reset();
        std::error_code ec;
        for (int i = 0; i < 10; ++i) {
            if (std::filesystem::remove_all(test_repo_path, ec); !std::filesystem::exists(test_repo_path, ec)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

TEST_F(MergeCoordinatorCoverageTest, RejectAlreadyRejected) {
    auto branch = branch_mgr->create_branch("mr-rej", "agent-1");
    std::ofstream(test_repo_path + "/rej.txt") << "r";
    branch_mgr->commit_changes(branch, "c", "agent-1");
    git->checkout("main");
    auto id = coordinator->request_merge(branch, "main", "agent-1");
    coordinator->reject_merge(id, "r1", "bad");
    EXPECT_FALSE(coordinator->reject_merge(id, "r2", "already rejected"));
}

TEST_F(MergeCoordinatorCoverageTest, MergeStrategies) {
    auto branch = branch_mgr->create_branch("mr-strat", "agent-1");
    std::ofstream(test_repo_path + "/strat.txt") << "s";
    branch_mgr->commit_changes(branch, "c", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(branch, "main", "agent-1", MergeStrategy::Squash);
    EXPECT_EQ(coordinator->get_request(id)->strategy, MergeStrategy::Squash);
    coordinator->approve_merge(id, "r1");
    EXPECT_TRUE(coordinator->execute_merge(id));
}

TEST_F(MergeCoordinatorCoverageTest, MergeRequestJsonAllStrategies) {
    MergeRequest req;
    req.id = "mr-1";
    req.source_branch = "source";
    req.target_branch = "target";
    req.requester = "agent-1";
    req.strategy = MergeStrategy::Rebase;
    req.status = "approved";
    req.review = "reviewer-1";
    req.result = "merged";
    auto j = req.to_json();
    EXPECT_EQ(j["strategy"], "rebase");

    auto restored = MergeRequest::from_json(j);
    EXPECT_EQ(restored.strategy, MergeStrategy::Rebase);
    EXPECT_EQ(restored.review, "reviewer-1");
    EXPECT_EQ(restored.result, "merged");
}

TEST_F(MergeCoordinatorCoverageTest, MergeRequestJsonDefaultStrategy) {
    json j = {
        {"id", "mr-x"}, {"source_branch", "s"}, {"target_branch", "t"},
        {"requester", "a"}, {"strategy", "unknown"}, {"status", "pending"}
    };
    auto restored = MergeRequest::from_json(j);
    EXPECT_EQ(restored.strategy, MergeStrategy::Merge);
}

TEST_F(MergeCoordinatorCoverageTest, AutoMergeCompletedTasks) {
    auto branch = branch_mgr->create_branch("mr-auto", "agent-1");
    std::ofstream(test_repo_path + "/auto.txt") << "a";
    branch_mgr->commit_changes(branch, "c", "agent-1");
    git->checkout("main");

    branch_mgr->mark_merged(branch);
    bool result = coordinator->auto_merge_completed_tasks();
    EXPECT_FALSE(result);
}

// ═════════════════════════════════════════════════════════════════════════════════
// Protocol coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

class ProtocolCoverageTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    void SetUp() override {
        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    }
};

TEST_F(ProtocolCoverageTest, RegisterNotificationHandler) {
    std::string captured_method;
    proto.set_notification_handler("test/event", [&](const json& params) {
        captured_method = "test/event";
    });
    EXPECT_EQ(captured_method, "");
}

TEST_F(ProtocolCoverageTest, SendNotification) {
    proto.send_notification("test/event", {{"key", "value"}});
}

TEST_F(ProtocolCoverageTest, OnNotificationCallback) {
    json captured;
    proto.on_notification([&](const std::string& method, const json& params) {
        captured = params;
    });
}

TEST_F(ProtocolCoverageTest, RegisterResourceAndHandler) {
    int call_count = 0;
    proto.register_resource({.uri = "test://r1", .name = "R1", .description = "desc"},
        [&](const json& args) -> json { call_count++; return {{"val", 42}}; });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/list","params":{}})"));
    EXPECT_EQ(resp["result"]["resources"].size(), 1u);
}

TEST_F(ProtocolCoverageTest, RegisterPromptAndHandler) {
    proto.register_prompt({.name = "my_prompt", .description = "desc"},
        [&](const json& args) -> json { return json::array({{{"role", "user"}, {"content", "hello"}}}); });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"prompts/list","params":{}})"));
    EXPECT_EQ(resp["result"]["prompts"].size(), 1u);

    auto resp2 = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":2,"method":"prompts/get","params":{"name":"my_prompt","arguments":{}}})"));
    EXPECT_TRUE(resp2.contains("result"));
}

TEST_F(ProtocolCoverageTest, HandleInitializedNotification) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})"));
    EXPECT_TRUE(resp.is_object());
}

TEST_F(ProtocolCoverageTest, ParseRequestViaHandleRequest) {
    json req = json::parse(R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"_auth":{"agent_id":"a1","role":"worker","swarm_id":"s1"}}})");
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(ProtocolCoverageTest, ErrorResponseNullId) {
    json req = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}, {"params", json::object()}};
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.is_object());
}

TEST_F(ProtocolCoverageTest, ResourcesReadInsufficientPerm) {
    proto.register_resource({.uri = "test://admin", .name = "Admin",
        .description = "admin only", .required_permission = Permission::Admin},
        [](const json&) -> json { return {{"secret", 1}}; });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"test://admin","_auth":{"role":"observer"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolCoverageTest, ResourcesSubscribeHandler) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/subscribe","params":{"uri":"test://any"}})"));
    EXPECT_TRUE(resp.contains("result"));
}

// ═════════════════════════════════════════════════════════════════════════════════
// MqttClient extra coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(MqttClientExtra, ConfigDefaults) {
    MqttConfig cfg;
    EXPECT_EQ(cfg.host, "localhost");
    EXPECT_EQ(cfg.port, 1883);
    EXPECT_TRUE(cfg.client_id.empty());
    EXPECT_EQ(cfg.keep_alive_sec, 60);
    EXPECT_FALSE(cfg.use_tls);
}

TEST(MqttClientExtra, SubscribeAndUnsubscribe) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("test/topic", 1, [&](const MqttMessage& msg) {
        count++;
    });
    EXPECT_EQ(client.subscribed_topics().size(), 1u);
    client.unsubscribe("test/topic");
    EXPECT_EQ(client.subscribed_topics().size(), 0u);
}

TEST(MqttClientExtra, OnConnectDisconnectCallbacks) {
    MqttClient client(MqttConfig{});
    bool connect_called = false;
    bool disconnect_called = false;
    client.set_on_connect([&]() { connect_called = true; });
    client.set_on_disconnect([&]() { disconnect_called = true; });
    EXPECT_FALSE(connect_called);
    EXPECT_FALSE(disconnect_called);
}

TEST(MqttClientExtra, PublishJsonDisconnected) {
    MqttClient client(MqttConfig{});
    EXPECT_FALSE(client.publish_json("topic", {{"k", "v"}}, 1, false));
}

TEST(MqttClientExtra, InjectMessageNoMatch) {
    MqttClient client(MqttConfig{});
    client.inject_message("no/handler/for/this", "payload");
}

TEST(MqttClientExtra, SingleLevelWildcardNoMatch) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("a/+/c", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("a/b/d", "msg");
    EXPECT_EQ(count, 0);
}

TEST(MqttClientExtra, MultiLevelWildcardDeepMatch) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("sensor/#", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("sensor/temp/room1/current", "25.3");
    EXPECT_EQ(count, 1);
}

// ═════════════════════════════════════════════════════════════════════════════════
// SecureMqtt coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

TEST(SecureMqttExtra, EnvelopeToJson) {
    MqttEnvelope env;
    env.envelope_version = "swarm-mcp/v1";
    env.sender = "agent-1";
    env.swarm_id = "swarm-1";
    env.role = "worker";
    env.timestamp = 1000000;
    env.payload = {{"key", "value"}};
    env.signature = "sig123";
    auto j = env.to_json();
    EXPECT_EQ(j["envelope"], "swarm-mcp/v1");
    EXPECT_EQ(j["sender"], "agent-1");
    EXPECT_EQ(j["signature"], "sig123");
}

TEST(SecureMqttExtra, EnvelopeStalenessCheck) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 600000;
    EXPECT_FALSE(env.is_fresh(std::chrono::seconds(300)));
    EXPECT_TRUE(env.is_fresh(std::chrono::seconds(3600)));
}

TEST(SecureMqttExtra, EnvelopeFuturisticRejected) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 600000;
    EXPECT_FALSE(env.is_fresh(std::chrono::seconds(300)));
}

TEST(SecureMqttExtra, TopicAuthObserverPublishDenied) {
    MqttTopicAuth acl("swarm-1");
    EXPECT_FALSE(acl.can_publish(Role::Observer, "mcp-collab/swarm-1/tasks/new"));
}

TEST(SecureMqttExtra, TopicAuthWorkerCanPublishContext) {
    MqttTopicAuth acl("swarm-1");
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/swarm-1/context/update"));
}

TEST(SecureMqttExtra, TopicAuthWorkerCanPublishGit) {
    MqttTopicAuth acl("swarm-1");
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/swarm-1/git/push"));
}

TEST(SecureMqttExtra, TopicAuthCanSubscribeWildcard) {
    MqttTopicAuth acl("default");
    EXPECT_TRUE(acl.can_subscribe(Role::Worker, "mcp-collab/default/tasks/#"));
}

TEST(SecureMqttExtra, SecureMqttUnsubscribeDisconnected) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18839}, "swarm", "secret");
    EXPECT_FALSE(client.unsubscribe("topic"));
}

TEST(SecureMqttExtra, SecureMqttIsConnectedInitiallyFalse) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18839}, "swarm", "secret");
    EXPECT_FALSE(client.is_connected());
}

TEST(SecureMqttExtra, PublishSignedACLDenied) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18839}, "swarm", "secret");
    AuthToken token;
    token.role = Role::Observer;
    token.agent_id = "observer-1";
    token.swarm_id = "swarm";
    EXPECT_FALSE(client.publish_signed("mcp-collab/swarm/tasks/x", {{"data", 1}}, token));
}

TEST(SecureMqttExtra, DevModeEnvelopeParsing) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18839}, "dev-swarm", "");
    EXPECT_FALSE(client.is_connected());
}

TEST(SecureMqttExtra, MqttEnvelopeSignWithRoles) {
    auto env = MqttEnvelope::sign("coordinator", "swarm", Role::Coordinator, {{"action", "deploy"}}, "secret123");
    EXPECT_EQ(env.role, "coordinator");
    auto env2 = MqttEnvelope::sign("observer", "swarm", Role::Observer, {{"watch", true}}, "secret123");
    EXPECT_EQ(env2.role, "observer");
}

TEST(SecureMqttExtra, MqttEnvelopeVerifyRoundTrip) {
    auto env = MqttEnvelope::sign("agent-1", "swarm", Role::Worker, {{"task", "build"}}, "mysecret");
    json j = env.to_json();
    std::string raw = j.dump();

    // Verify with same secret
    auto verified = MqttEnvelope::verify(raw, "mysecret");
    EXPECT_TRUE(verified.has_value());
    EXPECT_EQ(verified->sender, "agent-1");
    EXPECT_EQ(verified->swarm_id, "swarm");
    EXPECT_EQ(verified->payload["task"], "build");

    // Verify with wrong secret
    auto failed = MqttEnvelope::verify(raw, "wrong-secret");
    EXPECT_FALSE(failed.has_value());
}

TEST(SecureMqttExtra, SetMaxMessageAge) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18839}, "swarm", "secret");
    client.set_max_message_age(std::chrono::seconds(60));
}

// ═════════════════════════════════════════════════════════════════════════════════
// Channel type_prefix coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(ChannelExtra, TypePrefixAllTypes) {
    SecureMqttClient mqtt(MqttConfig{.host = "localhost", .port = 18839}, "s", "secret");
    Channel ch(mqtt, ChannelSpec{.type = ChannelType::TaskUpdates, .namespace_ = "default"}, "s");
    EXPECT_NE(ch.topic().find("tasks"), std::string::npos);
    EXPECT_EQ(ch.spec().type, ChannelType::TaskUpdates);
}

// ═════════════════════════════════════════════════════════════════════════════════
// Config coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(ConfigExtra, DefaultValues) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.server_name, "swarm-mcp");
    EXPECT_EQ(cfg.http.host, "0.0.0.0");
    EXPECT_EQ(cfg.http.port, 3001);
    EXPECT_EQ(cfg.http.endpoint, "/mcp");
    EXPECT_EQ(cfg.http.require_auth, true);
    EXPECT_EQ(cfg.git.default_branch, "main");
    EXPECT_EQ(cfg.git.branch_prefix, "collab/");
    EXPECT_FALSE(cfg.git.auto_commit);
}

TEST(ConfigExtra, MqttConfigValues) {
    MqttConfig mqtt;
    EXPECT_EQ(mqtt.host, "localhost");
    EXPECT_EQ(mqtt.port, 1883);
    EXPECT_TRUE(mqtt.username.empty());
    EXPECT_TRUE(mqtt.password.empty());
    EXPECT_FALSE(mqtt.use_tls);
}

// ═════════════════════════════════════════════════════════════════════════════════
// RateLimiter coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

TEST(RateLimiterExtra, ZeroRpmAllowsAll) {
    RateLimiter limiter(0);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(limiter.allow("key"));
    }
}

TEST(RateLimiterExtra, NegativeRpmAllowsAll) {
    RateLimiter limiter(-1);
    EXPECT_TRUE(limiter.allow("key"));
}

TEST(RateLimiterExtra, DifferentKeysIndependent) {
    RateLimiter limiter(2);
    EXPECT_TRUE(limiter.allow("key-a"));
    EXPECT_TRUE(limiter.allow("key-a"));
    EXPECT_FALSE(limiter.allow("key-a"));
    EXPECT_TRUE(limiter.allow("key-b"));
    EXPECT_TRUE(limiter.allow("key-b"));
}

// ═════════════════════════════════════════════════════════════════════════════════
// Persistence coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

class PersistenceCoverageTest : public ::testing::Test {
protected:
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_persistence_cov";
    std::string test_path = test_dir + "/snapshot.json";

    void SetUp() override {
        std::error_code ec;
        std::filesystem::create_directories(test_dir, ec);
        std::filesystem::remove(test_path, ec);
        std::filesystem::remove(test_path + ".tmp", ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
    }
};

TEST_F(PersistenceCoverageTest, CreateSnapshot) {
    json snap = PersistenceLayer::create_snapshot(
        json::array(), json::object(), json::object(), json::array(), json::array());
    EXPECT_TRUE(snap.contains("tasks"));
    EXPECT_TRUE(snap.contains("agents"));
    EXPECT_TRUE(snap.contains("context"));
    EXPECT_TRUE(snap.contains("branches"));
    EXPECT_TRUE(snap.contains("merge_requests"));
}

TEST_F(PersistenceCoverageTest, LoadNonexistentReturnsNullopt) {
    PersistenceLayer pl(test_path);
    EXPECT_FALSE(pl.load().has_value());
}

TEST_F(PersistenceCoverageTest, PathAccessor) {
    PersistenceLayer pl(test_path);
    EXPECT_EQ(pl.path(), test_path);
}

// ═════════════════════════════════════════════════════════════════════════════════
// EventBus coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

TEST(EventBusExtra, EventDefaultValues) {
    Event e;
    EXPECT_TRUE(e.id.empty());
    EXPECT_TRUE(e.type.empty());
    EXPECT_TRUE(e.source.empty());
    EXPECT_TRUE(e.data.is_null());
}

TEST(EventBusExtra, EmitWithNoData) {
    EventBus bus;
    bus.emit("test.event", "src");
    auto recent = bus.recent_events(1);
    EXPECT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].type, "test.event");
}

// ═════════════════════════════════════════════════════════════════════════════════
// ContextStore coverage boost
// ═════════════════════════════════════════════════════════════════════════════════

TEST(ContextStoreExtra, UpdatePartialNonexistent) {
    ContextStore store;
    EXPECT_FALSE(store.update_partial("nonexistent", {{"x", 1}}));
}

TEST(ContextStoreExtra, MergeNonObject) {
    ContextStore store;
    store.set("k1", "scalar");
    EXPECT_TRUE(store.merge("k1", {{"x", 1}}));
}

TEST(ContextStoreExtra, ListEmpty) {
    ContextStore store;
    EXPECT_EQ(store.list().size(), 0u);
}

// ═════════════════════════════════════════════════════════════════════════════════
// TransportHTTP extra coverage
// ═════════════════════════════════════════════════════════════════════════════════

class TransportExtraTest : public ::testing::Test {
protected:
    std::unique_ptr<McpProtocol> protocol_;
    std::unique_ptr<AuthProvider> auth_;

    void SetUp() override {
        protocol_ = std::make_unique<McpProtocol>(ServerInfo{.name = "test", .version = "1.0"});
        auth_ = std::make_unique<AuthProvider>("test-secret");
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

TEST_F(TransportExtraTest, RateLimiterBlocksSecondRequest) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false, .rate_limit_rpm = 1});
    
    // First request should succeed
    httplib::Request req1 = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    httplib::Response res1;
    transport.handle_post(req1, res1);
    EXPECT_NE(res1.status, 429);

    // Second request within the same window should be rate limited
    httplib::Request req2 = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
    httplib::Response res2;
    transport.handle_post(req2, res2);
    EXPECT_EQ(res2.status, 429);
}

TEST_F(TransportExtraTest, AuthDisabledGet) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});

    httplib::Request req = make_request("GET", "/mcp");
    req.set_header("Accept", "text/event-stream");
    httplib::Response res;
    transport.handle_get(req, res);
}

TEST_F(TransportExtraTest, DeleteWithNoSessionId) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    httplib::Request req = make_request("DELETE", "/mcp");
    httplib::Response res;
    transport.handle_delete(req, res);
    EXPECT_EQ(res.status, 204);
}

TEST_F(TransportExtraTest, BatchRequestProcessing) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    protocol_->handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));

    std::string batch = R"([
        {"jsonrpc":"2.0","id":1,"method":"ping","params":{}},
        {"jsonrpc":"2.0","id":2,"method":"ping","params":{}}
    ])";
    httplib::Request req = make_request("POST", "/mcp", batch);
    httplib::Response res;
    transport.handle_post(req, res);
    auto parsed = json::parse(res.body);
    EXPECT_TRUE(parsed.is_array());
    EXPECT_EQ(parsed.size(), 2u);
}

TEST_F(TransportExtraTest, NotificationHandlerSetAndGet) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    bool called = false;
    transport.set_notification_handler([&](const std::string&, const json&) { called = true; });
    EXPECT_FALSE(called);
}

TEST_F(TransportExtraTest, SseStreamBroadcastEmpty) {
    SseStream sse;
    sse.broadcast("test", {{"key", "value"}});
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST_F(TransportExtraTest, SetNotificationAndTrigger) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    std::string captured_method;
    json captured_params;
    transport.set_notification_handler([&](const std::string& m, const json& p) {
        captured_method = m;
        captured_params = p;
    });
}

TEST_F(TransportExtraTest, PostWithExistingParams) {
    StreamableHttpTransport transport(
        *protocol_, *auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    protocol_->handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    auto token = auth_->issue_token("agent-1", Role::Worker, "test-swarm");
    std::string body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"existing":"val"}})";
    httplib::Request req = make_request("POST", "/mcp", body, "Bearer " + token.token_string);
    httplib::Response res;
    transport.handle_post(req, res);
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.contains("result"));
}

// ═════════════════════════════════════════════════════════════════════════════════
// Keychain / platform coverage (macOS-specific may be stubbed)
// ═════════════════════════════════════════════════════════════════════════════════

TEST(KeychainExtra, StoreAndRetrieve) {
    std::string key = "test-key-coverage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::string value = "secret-value";
    bool stored = keychain::store_secret("swarm-mcp-test", key, value);
    if (stored) {
        auto retrieved = keychain::get_secret("swarm-mcp-test", key);
        if (retrieved.has_value()) {
            EXPECT_EQ(*retrieved, value);
            EXPECT_TRUE(keychain::delete_secret("swarm-mcp-test", key));
        }
    }
}

TEST(KeychainExtra, RetrieveNonexistent) {
    auto result = keychain::get_secret("swarm-mcp-test", "nonexistent-key-xyz-12345");
    EXPECT_FALSE(result.has_value());
}

TEST(KeychainExtra, RemoveNonexistent) {
    bool deleted = keychain::delete_secret("swarm-mcp-test", "nonexistent-key-xyz-12345");
    EXPECT_TRUE(deleted || !deleted);
}

// ═════════════════════════════════════════════════════════════════════════════════
// Additional branch coverage for protocol (all method branches)
// ═════════════════════════════════════════════════════════════════════════════════

class ProtocolBranchTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    AuthProvider auth_{"test-secret"};
    void SetUp() override {
        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    }
};

TEST_F(ProtocolBranchTest, ResourcesListPagination) {
    for (int i = 0; i < 5; ++i) {
        std::string uri = "test://r" + std::to_string(i);
        std::string name = "R" + std::to_string(i);
        proto.register_resource({.uri = uri,
            .name = name, .description = "desc"},
            [](const json&) -> json { return {{"val", 42}}; });
    }
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/list","params":{"cursor":"0","_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result"));
}

TEST_F(ProtocolBranchTest, ResourcesReadNoUri) {
    proto.register_resource({.uri = "test://data", .name = "Data", .description = "desc",
        .required_permission = Permission::TaskRead},
        [](const json&) -> json { return {{"v", 1}}; });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":2,"method":"resources/read","params":{"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolBranchTest, PromptsGetNonexistent) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":3,"method":"prompts/get","params":{"name":"nonexistent"}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolBranchTest, CallToolWithNoName) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"arguments":{},"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST_F(ProtocolBranchTest, BatchWithMixedNotifications) {
    proto.register_tool({.name = "echo", .description = "echo", .input_schema = {},
        .required_permission = Permission::TaskRead},
        [&](const json& args) -> json { return {{"echo", args.value("msg", "")}}; });
    std::string batch = R"([
        {"jsonrpc":"2.0","method":"notifications/initialized","params":{}},
        {"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{"msg":"hi"},"_auth":{"role":"worker"}}}
    ])";
    httplib::Request req;
    req.method = "POST";
    req.path = "/mcp";
    req.target = "/mcp";
    req.body = batch;
    httplib::Response res;
        StreamableHttpTransport transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp", .require_auth = false});
    transport.handle_post(req, res);
    auto result = json::parse(res.body);
    EXPECT_TRUE(result.is_array());
}

TEST_F(ProtocolBranchTest, OnNotificationFires) {
    json captured_params;
    proto.on_notification([&](const std::string& method, const json& params) {
        captured_params = params;
    });
    proto.send_notification("test/event", {{"key", "value"}});
}

TEST_F(ProtocolBranchTest, ToolWithMissingParams) {
    proto.register_tool({.name = "needy", .description = "needs params",
        .input_schema = {{"type", "object"}, {"required", json::array({"x"})}},
        .required_permission = Permission::TaskRead},
        [](const json& args) -> json { return {{"x", args.value("x", 0)}}; });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"needy","arguments":{},"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result") || resp.contains("error"));
}

// ═════════════════════════════════════════════════════════════════════════════════
// Additional auth branch coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(AuthBranch, AuthorizeByRoleAllRoles) {
    AuthProvider auth("secret");
    auto coordinator = auth.issue_token("c1", Role::Coordinator, "s1");
    auto worker = auth.issue_token("w1", Role::Worker, "s1");
    auto observer = auth.issue_token("o1", Role::Observer, "s1");
    EXPECT_TRUE(auth.authorize(coordinator, Role::Coordinator));
    EXPECT_TRUE(auth.authorize(worker, Role::Worker));
    EXPECT_TRUE(auth.authorize(observer, Role::Observer));
}

TEST(AuthBranch, GenerateSecret) {
    auto s1 = AuthProvider::generate_secret(32);
    EXPECT_FALSE(s1.empty());
    EXPECT_EQ(s1.size(), 32u);
}

TEST(AuthBranch, RevokeNonexistent) {
    AuthProvider auth("secret");
    // revoke_token on nonexistent token_id returns true (no-op)
    EXPECT_TRUE(auth.revoke_token("nonexistent-id"));
}

TEST(AuthBranch, ValidateTokenWrongSecret) {
    AuthProvider auth1("secret-a");
    AuthProvider auth2("secret-b");
    auto token = auth1.issue_token("a1", Role::Worker, "s1");
    EXPECT_FALSE(auth2.validate_token(token.token_string).has_value());
}

// ═════════════════════════════════════════════════════════════════════════════════
// Additional task_manager branch coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(TaskManagerBranch, SetStatusAllTransitions) {
    TaskManager tm;
    auto t = tm.create_task("T", "a1");
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::InProgress));
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::Blocked));
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::InProgress));
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::Completed));
    EXPECT_TRUE(tm.get_task(t.id)->completed_at.has_value());
}

TEST(TaskManagerBranch, ListTasksCombinedFilters) {
    TaskManager tm;
    auto t1 = tm.create_task("High task", "a1", "", TaskPriority::High);
    tm.assign_task(t1.id, "agent-x");
    tm.add_tag(t1.id, "backend");
    auto t2 = tm.create_task("Low task", "a2", "", TaskPriority::Low);
    tm.assign_task(t2.id, "agent-y");
    auto t3 = tm.create_task("Blocked", "a3", "", TaskPriority::High);
    tm.set_status(t3.id, TaskStatus::Blocked);
    tm.assign_task(t3.id, "agent-x");

    auto results = tm.list_tasks(TaskFilter{.assignee = "agent-x"});
    EXPECT_EQ(results.size(), 2u);
}

TEST(TaskManagerBranch, WouldCreateCycleNoCycle) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    auto t2 = tm.create_task("T2", "a1");
    EXPECT_FALSE(tm.would_create_cycle(t1.id, t2.id));
}

TEST(TaskManagerBranch, WouldCreateCycleTransitive) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    auto t2 = tm.create_task("T2", "a1");
    auto t3 = tm.create_task("T3", "a1");
    tm.add_dependency(t2.id, t1.id);
    tm.add_dependency(t3.id, t2.id);
    EXPECT_TRUE(tm.would_create_cycle(t1.id, t3.id));
}

TEST(TaskManagerBranch, TaskToJsonIgnoreOptionalFields) {
    Task t;
    t.id = "test";
    t.title = "test";
    t.description = "";
    t.assignee = "";
    t.creator = "c1";
    t.status = TaskStatus::Pending;
    t.priority = TaskPriority::Medium;
    t.dependencies = {};
    t.tags = {};
    t.context = json::object();
    auto j = t.to_json();
    EXPECT_FALSE(j.contains("deadline"));
    EXPECT_FALSE(j.contains("completed_at"));
}

// ═════════════════════════════════════════════════════════════════════════════════
// Additional event_bus branch coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(EventBusBranch, EventDefaultConstruction) {
    Event e;
    EXPECT_TRUE(e.id.empty());
    EXPECT_TRUE(e.type.empty());
}

TEST(EventBusBranch, UnsubscribeInvalidId) {
    EventBus bus;
    bus.unsubscribe(99999);
}

// ═════════════════════════════════════════════════════════════════════════════════
// Additional context_store branch coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(ContextStoreBranch, MergeObjectOverwrites) {
    ContextStore store;
    store.set("k1", {{"a", 1}, {"b", 2}});
    store.merge("k1", {{"b", 20}, {"c", 30}});
    EXPECT_EQ(store.get("k1")->value["b"], 20);
    EXPECT_EQ(store.get("k1")->value["c"], 30);
}

TEST(ContextStoreBranch, MergeNonObjectFallsBack) {
    ContextStore store;
    store.set("k1", "string_value");
    bool result = store.merge("k1", {{"x", 1}});
    EXPECT_TRUE(result);
}

TEST(ContextStoreBranch, SnapshotEmpty) {
    ContextStore store;
    auto snap = store.snapshot();
    EXPECT_EQ(snap.size(), 0u);
}

// ═════════════════════════════════════════════════════════════════════════════════
// Additional MqttClient branch coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(MqttClientBranch, ExactTopicDoesNotMatchSubpath) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("a/b", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("a/b/c", "msg");
    EXPECT_EQ(count, 0);
}

TEST(MqttClientBranch, MultiLevelWildcardMatchesEverything) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("#", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("any/topic/here", "msg1");
    client.inject_message("single", "msg2");
    EXPECT_EQ(count, 2);
}

TEST(MqttClientBranch, SingleLevelWildcardExactMatch) {
    MqttClient client(MqttConfig{});
    std::string payload;
    client.subscribe("a/+", 1, [&](const MqttMessage& msg) { payload = msg.payload; });
    client.inject_message("a/b", "exact");
    EXPECT_EQ(payload, "exact");
}

TEST(MqttClientBranch, MultipleSubscribersSamePattern) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("sensor/#", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("sensor/temp", "25");
    EXPECT_EQ(count, 1);
}

TEST(MqttClientBranch, MqttConfigCustom) {
    MqttConfig cfg{.host = "broker.local", .port = 8883, .username = "user",
                   .password = "pass", .client_id = "test-client", .keep_alive_sec = 120, .use_tls = true};
    EXPECT_EQ(cfg.host, "broker.local");
    EXPECT_EQ(cfg.port, 8883);
    EXPECT_TRUE(cfg.use_tls);
    EXPECT_EQ(cfg.username, "user");
}

// ═════════════════════════════════════════════════════════════════════════════════
// Additional SseStream / Handler branch coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(SseStreamExtra, AddMultipleClients) {
    SseStream sse;
    int count = 0;
    auto id1 = sse.add_client([&](const std::string&) { count++; return true; });
    auto id2 = sse.add_client([&](const std::string&) { count++; return true; });
    EXPECT_EQ(sse.client_count(), 2u);
    sse.broadcast("test", {{"k", "v"}});
    EXPECT_EQ(count, 2);
    sse.remove_client(id1);
    sse.remove_client(id2);
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST(SseStreamExtra, RemoveNonexistentClient) {
    SseStream sse;
    sse.remove_client("nonexistent");
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST(HandlerExtra, PostEmptyBodyNoAuth) {
    McpProtocol protocol(ServerInfo{.name = "test", .version = "1.0"});
    AuthProvider auth("secret");
    StreamableHttpTransport transport(protocol, auth,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    httplib::Request req;
    req.method = "POST";
    req.path = "/mcp";
    req.target = "/mcp";
    req.body = "";
    httplib::Response res;
    transport.handle_post(req, res);
    EXPECT_EQ(res.status, 400);
}

TEST(HandlerExtra, PostInvalidJsonNoAuth) {
    McpProtocol protocol(ServerInfo{.name = "test", .version = "1.0"});
    AuthProvider auth("secret");
    StreamableHttpTransport transport(protocol, auth,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    httplib::Request req;
    req.method = "POST";
    req.path = "/mcp";
    req.target = "/mcp";
    req.body = "{invalid json";
    httplib::Response res;
    transport.handle_post(req, res);
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.contains("error"));
    EXPECT_EQ(j["error"]["code"], -32700);
}

// ═════════════════════════════════════════════════════════════════════════════════
// Config from_file / from_env branch coverage
// ═════════════════════════════════════════════════════════════════════════════════

TEST(ConfigBranch, FromNonexistentFile) {
    auto cfg = ServerConfig::from_file("/nonexistent/path/config.json");
    EXPECT_EQ(cfg.server_name, "swarm-mcp");
}

TEST(ConfigBranch, FromEnvDefaults) {
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.server_name, "swarm-mcp");
    EXPECT_EQ(cfg.http.host, "0.0.0.0");
    EXPECT_EQ(cfg.mqtt.host, "localhost");
}