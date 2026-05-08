#include <gtest/gtest.h>
#include "mcp_collab/branch_manager.hpp"
#include "mcp_collab/git_operations.hpp"
#include <filesystem>
#include <fstream>
#include <thread>

using namespace mcp_collab;

class BranchManagerTest : public ::testing::Test {
protected:
    std::string test_repo_path;
    std::unique_ptr<GitOperations> git;
    std::unique_ptr<BranchManager> mgr;

    void SetUp() override {
        test_repo_path = std::filesystem::temp_directory_path().string() + "/swarm-mcp-branch-test";
        std::error_code ec;
        std::filesystem::remove_all(test_repo_path, ec);
        std::filesystem::create_directories(test_repo_path);
        std::filesystem::current_path(test_repo_path);
        system("git init");
        system("git config user.email \"test@test.com\"");
        system("git config user.name \"Test\"");
        std::ofstream(test_repo_path + "/initial.txt") << "init";
        system("git add .");
        system("git commit -m \"initial\"");
        git = std::make_unique<GitOperations>(test_repo_path);
        mgr = std::make_unique<BranchManager>(*git, "collab/");
    }

    void TearDown() override {
        mgr.reset();
        git.reset();
        std::error_code ec;
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (std::filesystem::remove_all(test_repo_path, ec); !std::filesystem::exists(test_repo_path, ec)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

TEST_F(BranchManagerTest, CreateBranch) {
    auto branch = mgr->create_branch("task-1", "agent-1");
    EXPECT_FALSE(branch.empty());
    EXPECT_EQ(branch, "collab/task-1");
    EXPECT_EQ(git->current_branch(), "collab/task-1");
}

TEST_F(BranchManagerTest, CreateBranchInfoStored) {
    mgr->create_branch("task-2", "agent-1");
    auto info = mgr->get_branch_info("collab/task-2");
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->task_id, "task-2");
    EXPECT_EQ(info->agent_id, "agent-1");
    EXPECT_EQ(info->state, BranchState::Active);
}

TEST_F(BranchManagerTest, CreateBranchWithBase) {
    auto branch = mgr->create_branch("task-3", "agent-1", "main");
    EXPECT_FALSE(branch.empty());
    auto info = mgr->get_branch_info(branch);
    EXPECT_EQ(info->base_branch, "main");
}

TEST_F(BranchManagerTest, LockBranch) {
    auto branch = mgr->create_branch("task-4", "agent-1");
    git->checkout("main");
    EXPECT_TRUE(mgr->lock_branch(branch));
    auto info = mgr->get_branch_info(branch);
    EXPECT_EQ(info->state, BranchState::Locked);
}

TEST_F(BranchManagerTest, UnlockBranch) {
    auto branch = mgr->create_branch("task-5", "agent-1");
    git->checkout("main");
    mgr->lock_branch(branch);
    EXPECT_TRUE(mgr->unlock_branch(branch));
    EXPECT_EQ(mgr->get_branch_info(branch)->state, BranchState::Active);
}

TEST_F(BranchManagerTest, MarkMerged) {
    auto branch = mgr->create_branch("task-6", "agent-1");
    git->checkout("main");
    EXPECT_TRUE(mgr->mark_merged(branch));
    EXPECT_EQ(mgr->get_branch_info(branch)->state, BranchState::Merged);
}

TEST_F(BranchManagerTest, MarkAbandoned) {
    auto branch = mgr->create_branch("task-7", "agent-1");
    git->checkout("main");
    EXPECT_TRUE(mgr->mark_abandoned(branch));
    EXPECT_EQ(mgr->get_branch_info(branch)->state, BranchState::Abandoned);
}

TEST_F(BranchManagerTest, ListActive) {
    mgr->create_branch("task-8", "agent-1");
    auto branch2 = mgr->create_branch("task-9", "agent-1");
    git->checkout("main");
    mgr->mark_abandoned(branch2);
    auto active = mgr->list_active();
    EXPECT_EQ(active.size(), 1u);
}

TEST_F(BranchManagerTest, ListByAgent) {
    mgr->create_branch("task-10", "agent-1");
    mgr->create_branch("task-11", "agent-2");
    auto a1 = mgr->list_by_agent("agent-1");
    EXPECT_EQ(a1.size(), 1u);
}

TEST_F(BranchManagerTest, FindByTask) {
    mgr->create_branch("task-12", "agent-1");
    auto info = mgr->find_by_task("task-12");
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->task_id, "task-12");
}

TEST_F(BranchManagerTest, FindByTaskNonexistent) {
    EXPECT_FALSE(mgr->find_by_task("nonexistent").has_value());
}

TEST_F(BranchManagerTest, CommitChanges) {
    auto branch = mgr->create_branch("task-13", "agent-1");
    std::ofstream(test_repo_path + "/newfile.txt") << "content";
    EXPECT_TRUE(mgr->commit_changes(branch, "test commit", "agent-1"));
}

TEST_F(BranchManagerTest, BranchInfoJsonRoundTrip) {
    mgr->create_branch("task-14", "agent-1");
    auto info = mgr->get_branch_info("collab/task-14");
    auto j = info->to_json();
    auto restored = BranchInfo::from_json(j);
    EXPECT_EQ(restored.name, info->name);
    EXPECT_EQ(restored.task_id, info->task_id);
    EXPECT_EQ(restored.agent_id, info->agent_id);
}

TEST_F(BranchManagerTest, Callback) {
    std::string last_event;
    mgr->on_change([&](const std::string& ev, const BranchInfo&) { last_event = ev; });
    mgr->create_branch("task-15", "agent-1");
    EXPECT_EQ(last_event, "branch.created");
}