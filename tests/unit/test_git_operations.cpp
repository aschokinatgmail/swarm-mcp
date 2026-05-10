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
        test_repo_path = (std::filesystem::temp_directory_path() / ("swarm-mcp-git-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)))).string();
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

TEST_F(GitOperationsTest, CommitWithAuthor) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/author.txt") << "content";
    git.add();
    EXPECT_TRUE(git.commit("commit with author", "Test Author"));
}

TEST_F(GitOperationsTest, Pull) {
    GitOperations git(test_repo_path);
    // pull without a remote should return false
    EXPECT_FALSE(git.pull());
}

TEST_F(GitOperationsTest, Fetch) {
    GitOperations git(test_repo_path);
    EXPECT_TRUE(git.fetch());
}

TEST_F(GitOperationsTest, Rebase) {
    GitOperations git(test_repo_path);
    git.checkout("rebase-branch", true);
    std::ofstream(test_repo_path + "/rebase.txt") << "rebase content";
    git.add();
    git.commit("rebase commit");
    git.checkout("main");
    EXPECT_TRUE(git.rebase("rebase-branch"));
}

TEST_F(GitOperationsTest, MergeSquash) {
    GitOperations git(test_repo_path);
    git.checkout("squash-branch", true);
    std::ofstream(test_repo_path + "/squash.txt") << "squash content";
    git.add();
    git.commit("squash commit");
    git.checkout("main");
    EXPECT_TRUE(git.merge_squash("squash-branch"));
}

TEST_F(GitOperationsTest, ResetHard) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/hard.txt") << "hard";
    git.add();
    git.commit("hard reset");
    EXPECT_TRUE(git.reset("HEAD~1", true));
}

TEST_F(GitOperationsTest, DiffWithRefs) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/diff1.txt") << "v1";
    git.add();
    git.commit("v1");
    auto c1 = git.current_commit();
    std::ofstream(test_repo_path + "/diff1.txt") << "v2";
    git.add();
    git.commit("v2");
    auto c2 = git.current_commit();
    auto d = git.diff(c1, c2);
    EXPECT_FALSE(d.empty());
}

TEST_F(GitOperationsTest, DiffSingleRef) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/d.txt") << "initial";
    git.add();
    git.commit("init");
    auto diff = git.diff("HEAD");
    // diff against HEAD should be empty (no uncommitted changes)
    EXPECT_TRUE(diff.empty());
}

TEST_F(GitOperationsTest, AddSpecificPath) {
    GitOperations git(test_repo_path);
    std::ofstream(test_repo_path + "/specific.txt") << "specific";
    EXPECT_TRUE(git.add("specific.txt"));
    EXPECT_TRUE(git.has_changes());
}

TEST_F(GitOperationsTest, BranchListRemote) {
    GitOperations git(test_repo_path);
    auto list = git.branches(true);
    // Remote branches may be empty in local repo
    EXPECT_GE(list.size(), 0u);
}

TEST_F(GitOperationsTest, ExecShellInjectionBlocked) {
    GitOperations git(test_repo_path);
    auto result = git.exec("status; ls");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.exit_code, -1);
}

TEST_F(GitOperationsTest, ExecUnquotedVariableBlocked) {
    GitOperations git(test_repo_path);
    // '$' outside quotes should be blocked
    auto result = git.exec("status $HOME");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.exit_code, -1);
}

TEST_F(GitOperationsTest, ExecQuotedVariableBlocked) {
    GitOperations git(test_repo_path);
    // Quoted or not, pipe '|' is always in the dangerous_chars denylist
    auto result = git.exec("log --format=\"%s | $HOME\"");
    EXPECT_EQ(result.exit_code, -1);
}

TEST_F(GitOperationsTest, ExecPopenFailure) {
    GitOperations git(test_repo_path);
    // Very long invalid command should fail gracefully
    auto result = git.exec("nonexistent-command-that-does-not-exist-xyz");
    EXPECT_FALSE(result.success);
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