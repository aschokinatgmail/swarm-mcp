#include <gtest/gtest.h>
#include "mcp_collab/git_operations.hpp"
#include "mcp_collab/branch_manager.hpp"
#include "mcp_collab/merge_coordinator.hpp"
#include <filesystem>
#include <fstream>

using namespace mcp_collab;

class GitWorkflowIntegrationTest : public ::testing::Test {
protected:
    std::string test_repo_path;
    std::unique_ptr<GitOperations> git;
    std::unique_ptr<BranchManager> branch_mgr;
    std::unique_ptr<MergeCoordinator> merge_coord;

    void SetUp() override {
        test_repo_path = std::filesystem::temp_directory_path().string() + "/swarm-mcp-git-integration";
        std::filesystem::remove_all(test_repo_path);
        std::filesystem::create_directories(test_repo_path);
        std::filesystem::current_path(test_repo_path);
        system("git init");
        system("git config user.email \"test@test.com\"");
        system("git config user.name \"Test\"");
        std::ofstream(test_repo_path + "/README.md") << "# Test Repo";
        system("git add .");
        system("git commit -m \"Initial commit\"");

        // Determine the default branch name (master or main)
        git = std::make_unique<GitOperations>(test_repo_path);
        std::string default_branch = git->current_branch();

        branch_mgr = std::make_unique<BranchManager>(*git, "collab/");
        merge_coord = std::make_unique<MergeCoordinator>(*git, *branch_mgr);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_repo_path);
    }
};

TEST_F(GitWorkflowIntegrationTest, FullBranchLifecycle) {
    // 1. Task created → branch created
    auto branch = branch_mgr->create_branch("TASK-001", "agent-1");
    EXPECT_FALSE(branch.empty());
    EXPECT_EQ(git->current_branch(), branch);

    // 2. Work on branch
    std::ofstream(test_repo_path + "/feature.txt") << "feature implementation";
    EXPECT_TRUE(branch_mgr->commit_changes(branch, "Implement feature for TASK-001", "agent-1"));

    // 3. Request merge
    git->checkout("master");
    auto merge_id = merge_coord->request_merge(branch, "master", "agent-1", MergeStrategy::Squash);
    EXPECT_FALSE(merge_id.empty());

    // 4. Approve and execute merge
    EXPECT_TRUE(merge_coord->approve_merge(merge_id, "reviewer-1"));
    EXPECT_TRUE(merge_coord->execute_merge(merge_id));

    // 5. Verify merge completed
    auto req = merge_coord->get_request(merge_id);
    EXPECT_EQ(req->status, "completed");
}

TEST_F(GitWorkflowIntegrationTest, BranchLockingSyncsState) {
    auto branch = branch_mgr->create_branch("TASK-002", "agent-2");
    git->checkout("master");

    EXPECT_TRUE(branch_mgr->lock_branch(branch));
    auto info = branch_mgr->get_branch_info(branch);
    EXPECT_EQ(info->state, BranchState::Locked);

    EXPECT_TRUE(branch_mgr->unlock_branch(branch));
    EXPECT_EQ(branch_mgr->get_branch_info(branch)->state, BranchState::Active);
}

TEST_F(GitWorkflowIntegrationTest, AbandonedBranch) {
    auto branch = branch_mgr->create_branch("TASK-003", "agent-3");
    git->checkout("master");

    EXPECT_TRUE(branch_mgr->mark_abandoned(branch));
    EXPECT_EQ(branch_mgr->get_branch_info(branch)->state, BranchState::Abandoned);

    // Abandoned branches should not show as active
    auto active = branch_mgr->list_active();
    for (const auto& a : active) {
        EXPECT_NE(a.name, branch);
    }
}

TEST_F(GitWorkflowIntegrationTest, DiffToBase) {
    auto branch = branch_mgr->create_branch("TASK-004", "agent-1");
    std::ofstream(test_repo_path + "/new-feature.txt") << "new feature";
    branch_mgr->commit_changes(branch, "Add new feature", "agent-1");

    auto diff = branch_mgr->diff_to_base(branch);
    EXPECT_FALSE(diff.empty());
}

TEST_F(GitWorkflowIntegrationTest, MergeRejection) {
    auto branch = branch_mgr->create_branch("TASK-005", "agent-1");
    std::ofstream(test_repo_path + "/feature5.txt") << "feature 5";
    branch_mgr->commit_changes(branch, "Feature 5", "agent-1");
    git->checkout("master");

    auto merge_id = merge_coord->request_merge(branch, "master", "agent-1");
    EXPECT_TRUE(merge_coord->reject_merge(merge_id, "reviewer-1", "not ready"));

    auto req = merge_coord->get_request(merge_id);
    EXPECT_EQ(req->status, "rejected");

    // Rejected merge should not be executable
    EXPECT_FALSE(merge_coord->execute_merge(merge_id));
}

TEST_F(GitWorkflowIntegrationTest, MergeRequestByDifferentRequesters) {
    auto branch = branch_mgr->create_branch("TASK-006", "agent-1");
    std::ofstream(test_repo_path + "/f6.txt") << "f6";
    branch_mgr->commit_changes(branch, "F6", "agent-1");
    git->checkout("master");

    auto id = merge_coord->request_merge(branch, "master", "requester-1");
    auto by_req = merge_coord->list_by_requester("requester-1");
    EXPECT_EQ(by_req.size(), 1u);
}

TEST_F(GitWorkflowIntegrationTest, MultipleBranchesForAgent) {
    branch_mgr->create_branch("TASK-007", "agent-x");
    branch_mgr->create_branch("TASK-008", "agent-x");

    auto branches = branch_mgr->list_by_agent("agent-x");
    EXPECT_GE(branches.size(), 2u);
}

TEST_F(GitWorkflowIntegrationTest, TaskToBranchToMergeWorkflow) {
    // Simulate a full workflow: task → branch → commit → merge
    auto branch = branch_mgr->create_branch("TASK-WF-001", "dev-1");
    EXPECT_EQ(branch, "collab/TASK-WF-001");

    // Make changes
    std::ofstream(test_repo_path + "/workflow.txt") << "workflow test";
    EXPECT_TRUE(branch_mgr->commit_changes(branch, "Implement TASK-WF-001", "dev-1"));

    // Go back to master and merge
    git->checkout("master");
    auto mid = merge_coord->request_merge(branch, "master", "dev-1", MergeStrategy::Squash);
    merge_coord->approve_merge(mid, "lead-dev");
    EXPECT_TRUE(merge_coord->execute_merge(mid));

    // Verify the file exists on master
    EXPECT_TRUE(std::filesystem::exists(test_repo_path + "/workflow.txt"));
}

TEST_F(GitWorkflowIntegrationTest, BranchInfoPersistence) {
    auto branch = branch_mgr->create_branch("TASK-INFO", "agent-test");
    auto info = branch_mgr->get_branch_info(branch);

    auto j = info->to_json();
    auto restored = BranchInfo::from_json(j);

    EXPECT_EQ(restored.name, info->name);
    EXPECT_EQ(restored.task_id, info->task_id);
    EXPECT_EQ(restored.agent_id, info->agent_id);
    EXPECT_EQ(restored.state, info->state);
    EXPECT_EQ(restored.base_branch, info->base_branch);
}