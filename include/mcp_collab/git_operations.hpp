#pragma once

#include <string>
#include <vector>
#include <optional>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <functional>

#include <nlohmann/json.hpp>

namespace mcp_collab {

using json = nlohmann/json;

struct GitResult {
    bool success{false};
    int exit_code{-1};
    std::string stdout_out;
    std::string stderr_out;
};

class GitOperations {
public:
    explicit GitOperations(const std::string& repo_path);

    GitResult exec(const std::string& args) const;

    bool is_repo() const;
    std::string current_branch() const;
    std::string current_commit() const;
    bool has_changes() const;

    bool init() const;
    bool fetch() const;
    bool pull() const;
    bool push() const;
    bool commit(const std::string& message, const std::string& author = "") const;
    bool add(const std::string& pathspec = ".") const;
    bool checkout(const std::string& branch, bool create = false) const;
    bool merge(const std::string& branch, bool no_ff = false) const;
    bool merge_squash(const std::string& branch) const;
    bool rebase(const std::string& branch) const;
    bool branch_delete(const std::string& branch, bool force = false) const;
    bool stash() const;
    bool stash_pop() const;
    bool reset(const std::string& ref, bool hard = false) const;
    bool cherry_pick(const std::string& commit_hash) const;

    std::vector<std::string> log(int count = 10, const std::string& format = "") const;
    std::vector<std::string> branches(bool remote = false) const;
    std::string diff(const std::string& from_ref = "", const std::string& to_ref = "") const;
    std::string show(const std::string& ref) const;

private:
    std::string repo_path_;
    mutable std::mutex exec_mutex_;  // Serialize git commands to prevent working tree corruption
};

}