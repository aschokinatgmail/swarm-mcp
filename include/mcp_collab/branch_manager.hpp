#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <optional>

#include "mcp_collab/git_operations.hpp"

namespace mcp_collab {

enum class BranchState {
    Active,
    Merged,
    Abandoned,
    Locked
};

struct BranchInfo {
    std::string name;
    std::string task_id;
    std::string agent_id;
    BranchState state{BranchState::Active};
    std::string base_branch;
    std::chrono::system_clock::time_point created_at{std::chrono::system_clock::now()};

    json to_json() const;
    static BranchInfo from_json(const json& j);
};

class BranchManager {
public:
    explicit BranchManager(GitOperations& git, const std::string& prefix = "collab/");

    std::string create_branch(const std::string& task_id, const std::string& agent_id,
                               const std::string& base = "");
    bool lock_branch(const std::string& branch);
    bool unlock_branch(const std::string& branch);
    bool mark_merged(const std::string& branch);
    bool mark_abandoned(const std::string& branch);

    std::optional<BranchInfo> get_branch_info(const std::string& branch) const;
    std::vector<BranchInfo> list_active() const;
    std::vector<BranchInfo> list_by_agent(const std::string& agent_id) const;
    std::optional<BranchInfo> find_by_task(const std::string& task_id) const;

    bool commit_changes(const std::string& branch, const std::string& message,
                        const std::string& agent_id);
    std::string diff_to_base(const std::string& branch) const;

    using BranchCallback = std::function<void(const std::string& event, const BranchInfo& info)>;
    void on_change(BranchCallback cb);

private:
    std::string make_branch_name(const std::string& task_id) const;

    GitOperations& git_;
    std::string prefix_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, BranchInfo> branches_;
    BranchCallback callback_;
};

}