#include <gtest/gtest.h>
#include "mcp_collab/git_operations.hpp"
#include <filesystem>
#include <fstream>
#include <thread>

using namespace mcp_collab;

class GitOperationsTest : public ::testing::Test {
protected:
    std::string test_repo_path;

    void SetUp() override {
        test_repo_path = std::filesystem::temp_directory_path().string() + "/swarm-mcp-git-test";
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
    }

    void TearDown() override {
        std::error_code ec;
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (std::filesystem::remove_all(test_repo_path, ec); !std::filesystem::exists(test_repo_path, ec)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

TEST_F(GitOperationsTest, IsRepo) {
    GitOperations git(test_repo_path);
    EXPECT_TRUE(git.is_repo());
}

TEST_F(GitOperationsTest, IsNotRepo) {
    GitOperations git("/nonexistent/path");
    EXPECT_FALSE(git.is_repo());
}

TEST_F(GitOperationsTest, CurrentBranch) {
    GitOperations git(test_repo_path);
    auto branch = git.current_branch();
    EXPECT_FALSE(branch.empty());
}

TEST_F(GitOperationsTest, CurrentCommit) {
    GitOperations git(test_repo_path);
    auto commit = git.current_commit();
    EXPECT_FALSE(commit.empty());
    EXPECT_EQ(commit.size(), 40);
}

TEST_F(GitOperationsTest, HasChanges) {
    GitOperations git(test_repo_path);
    EXPECT_FALSE(git.has_changes());
    std::ofstream(test_repo_path + "/new.txt") << "change";
    EXPECT_TRUE(git.has_changes());
}

TEST_F(GitOperationsTest, AddAndCommit) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/file1.txt") << "content";
    EXPECT_TRUE(git.add());
    EXPECT_TRUE(git.commit("test commit"));
}

TEST_F(GitOperationsTest, BranchAndCheckout) {
    GitOperations git(test_repo_path);
    EXPECT_TRUE(git.checkout("feature-test", true));
    EXPECT_EQ(git.current_branch(), "feature-test");
    EXPECT_TRUE(git.checkout("main"));
    EXPECT_EQ(git.current_branch(), "main");
}

TEST_F(GitOperationsTest, BranchList) {
    GitOperations git(test_repo_path);
    git.checkout("branch-a", true);
    git.checkout("main");
    auto list = git.branches();
    EXPECT_GE(list.size(), 2u);
}

TEST_F(GitOperationsTest, BranchDelete) {
    GitOperations git(test_repo_path);
    git.checkout("to-delete", true);
    git.checkout("main");
    EXPECT_TRUE(git.branch_delete("to-delete"));
}

TEST_F(GitOperationsTest, Log) {
    GitOperations git(test_repo_path);
    auto log = git.log(5);
    EXPECT_GE(log.size(), 1u);
}

TEST_F(GitOperationsTest, Diff) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/file2.txt") << "diff content";
    git.add();
    git.commit("for diff");
    auto diff = git.diff();
    EXPECT_TRUE(diff.empty()); // no uncommitted changes
}

TEST_F(GitOperationsTest, Stash) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/stash.txt") << "stash me";
    git.add();
    EXPECT_TRUE(git.stash());
    EXPECT_TRUE(git.stash_pop());
}

TEST_F(GitOperationsTest, Merge) {
    GitOperations git(test_repo_path);
    git.checkout("merge-branch", true);
    std::ofstream(test_repo_path + "/merged.txt") << "from branch";
    git.add();
    git.commit("branch commit");
    git.checkout("main");
    EXPECT_TRUE(git.merge("merge-branch", true));
}

TEST_F(GitOperationsTest, MergeFastForward) {
    GitOperations git(test_repo_path);
    git.checkout("ff-branch", true);
    std::ofstream(test_repo_path + "/ff.txt") << "ff";
    git.add();
    git.commit("ff commit");
    git.checkout("main");
    EXPECT_TRUE(git.merge("ff-branch", false));
}

TEST_F(GitOperationsTest, ExecWithInvalidArgs) {
    GitOperations git(test_repo_path);
    auto result = git.exec("nonexistent-command-xyz");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.exit_code, 0);
}

TEST_F(GitOperationsTest, CherryPick) {
    GitOperations git(test_repo_path);
    git.checkout("cp-branch", true);
    std::ofstream(test_repo_path + "/cp.txt") << "cherry";
    git.add();
    git.commit("cherry commit");
    auto log = git.log(1);
    git.checkout("main");
}

TEST_F(GitOperationsTest, ResetSoft) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/reset.txt") << "reset";
    git.add();
    git.commit("to reset");
    auto head = git.current_commit();
    EXPECT_TRUE(git.reset("HEAD~1", false));
}

TEST_F(GitOperationsTest, Show) {
    GitOperations git(test_repo_path);
    auto commit = git.current_commit();
    auto result = git.show(commit);
    EXPECT_FALSE(result.empty());
}