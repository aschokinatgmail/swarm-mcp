#include <gtest/gtest.h>
#include "mcp_collab/merge_coordinator.hpp"
#include "mcp_collab/git_operations.hpp"
#include "mcp_collab/branch_manager.hpp"
#include <filesystem>
#include <fstream>
#include <thread>

using namespace mcp_collab;

class MergeCoordinatorTest : public ::testing::Test {
protected:
    std::string test_repo_path;
    std::unique_ptr<GitOperations> git;
    std::unique_ptr<BranchManager> branch_mgr;
    std::unique_ptr<MergeCoordinator> coordinator;

    void SetUp() override {
        test_repo_path = std::filesystem::temp_directory_path().string() + "/swarm-mcp-merge-test";
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
        branch_mgr = std::make_unique<BranchManager>(*git, "collab/");
        coordinator = std::make_unique<MergeCoordinator>(*git, *branch_mgr);
    }

    void TearDown() override {
        coordinator.reset();
        branch_mgr.reset();
        git.reset();
        std::error_code ec;
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (std::filesystem::remove_all(test_repo_path, ec); !std::filesystem::exists(test_repo_path, ec)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

TEST_F(MergeCoordinatorTest, RequestMerge) {
    auto branch = branch_mgr->create_branch("t1", "agent-1");
    std::ofstream(test_repo_path + "/file1.txt") << "content";
    branch_mgr->commit_changes(branch, "change", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(branch, "main", "agent-1");
    EXPECT_FALSE(id.empty());
    auto req = coordinator->get_request(id);
    EXPECT_TRUE(req.has_value());
    EXPECT_EQ(req->status, "pending");
    EXPECT_EQ(req->source_branch, branch);
    EXPECT_EQ(req->target_branch, "main");
}

TEST_F(MergeCoordinatorTest, ApproveMerge) {
    auto branch = branch_mgr->create_branch("t2", "agent-1");
    std::ofstream(test_repo_path + "/file2.txt") << "content";
    branch_mgr->commit_changes(branch, "change", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(branch, "main", "agent-1");
    EXPECT_TRUE(coordinator->approve_merge(id, "reviewer-1"));
    auto req = coordinator->get_request(id);
    EXPECT_EQ(req->status, "approved");
    EXPECT_EQ(req->review, "reviewer-1");
}

TEST_F(MergeCoordinatorTest, RejectMerge) {
    auto branch = branch_mgr->create_branch("t3", "agent-1");
    std::ofstream(test_repo_path + "/file3.txt") << "content";
    branch_mgr->commit_changes(branch, "change", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(branch, "main", "agent-1");
    EXPECT_TRUE(coordinator->reject_merge(id, "reviewer-1", "not ready"));
    auto req = coordinator->get_request(id);
    EXPECT_EQ(req->status, "rejected");
}

TEST_F(MergeCoordinatorTest, ApproveAlreadyApproved) {
    auto branch = branch_mgr->create_branch("t4", "agent-1");
    std::ofstream(test_repo_path + "/file4.txt") << "c";
    branch_mgr->commit_changes(branch, "c", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(branch, "main", "agent-1");
    EXPECT_TRUE(coordinator->approve_merge(id, "r1"));
    EXPECT_FALSE(coordinator->approve_merge(id, "r2")); // already approved
}

TEST_F(MergeCoordinatorTest, ExecuteMerge) {
    auto branch = branch_mgr->create_branch("t5", "agent-1");
    std::ofstream(test_repo_path + "/file5.txt") << "merge me";
    branch_mgr->commit_changes(branch, "merge content", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(branch, "main", "agent-1", MergeStrategy::Squash);
    coordinator->approve_merge(id, "r1");
    EXPECT_TRUE(coordinator->execute_merge(id));
    auto req = coordinator->get_request(id);
    EXPECT_EQ(req->status, "completed");
}

TEST_F(MergeCoordinatorTest, ExecuteUnapprovedFails) {
    auto branch = branch_mgr->create_branch("t6", "agent-1");
    std::ofstream(test_repo_path + "/file6.txt") << "content";
    branch_mgr->commit_changes(branch, "c", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(branch, "main", "agent-1");
    EXPECT_FALSE(coordinator->execute_merge(id));
}

TEST_F(MergeCoordinatorTest, PendingRequests) {
    auto b1 = branch_mgr->create_branch("t7", "agent-1");
    std::ofstream(test_repo_path + "/f7.txt") << "c";
    branch_mgr->commit_changes(b1, "c", "agent-1");
    git->checkout("main");

    coordinator->request_merge(b1, "main", "agent-1");
    EXPECT_EQ(coordinator->pending_requests().size(), 1u);
}

TEST_F(MergeCoordinatorTest, ListByBranch) {
    auto b1 = branch_mgr->create_branch("t8", "agent-1");
    std::ofstream(test_repo_path + "/f8.txt") << "c";
    branch_mgr->commit_changes(b1, "c", "agent-1");
    git->checkout("main");

    coordinator->request_merge(b1, "main", "agent-1");
    auto by_branch = coordinator->list_by_branch(b1);
    EXPECT_EQ(by_branch.size(), 1u);
}

TEST_F(MergeCoordinatorTest, ListByRequester) {
    auto b1 = branch_mgr->create_branch("t9", "agent-1");
    std::ofstream(test_repo_path + "/f9.txt") << "c";
    branch_mgr->commit_changes(b1, "c", "agent-1");
    git->checkout("main");

    coordinator->request_merge(b1, "main", "agent-x");
    auto by_req = coordinator->list_by_requester("agent-x");
    EXPECT_EQ(by_req.size(), 1u);
}

TEST_F(MergeCoordinatorTest, GetNonexistent) {
    EXPECT_FALSE(coordinator->get_request("nonexistent").has_value());
}

TEST_F(MergeCoordinatorTest, MergeRequestJsonRoundTrip) {
    MergeRequest req;
    req.id = "mr-1";
    req.source_branch = "collab/t1";
    req.target_branch = "main";
    req.requester = "agent-1";
    req.strategy = MergeStrategy::Squash;
    req.status = "approved";
    auto j = req.to_json();
    auto restored = MergeRequest::from_json(j);
    EXPECT_EQ(restored.id, req.id);
    EXPECT_EQ(restored.source_branch, req.source_branch);
    EXPECT_EQ(restored.strategy, MergeStrategy::Squash);
}

TEST_F(MergeCoordinatorTest, Callback) {
    std::string last_event;
    coordinator->on_change([&](const std::string& ev, const MergeRequest&) { last_event = ev; });

    auto b1 = branch_mgr->create_branch("t10", "agent-1");
    std::ofstream(test_repo_path + "/f10.txt") << "c";
    branch_mgr->commit_changes(b1, "c", "agent-1");
    git->checkout("main");

    auto id = coordinator->request_merge(b1, "main", "agent-1");
    EXPECT_EQ(last_event, "merge.requested");

    coordinator->approve_merge(id, "r1");
    EXPECT_EQ(last_event, "merge.approved");
}