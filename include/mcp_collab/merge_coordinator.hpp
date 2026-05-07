#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "mcp_collab/git_operations.hpp"
#include "mcp_collab/branch_manager.hpp"

namespace mcp_collab {

enum class MergeStrategy {
    Merge,
    Rebase,
    Squash
};

struct MergeRequest {
    std::string id;
    std::string source_branch;
    std::string target_branch;
    std::string requester;
    MergeStrategy strategy{MergeStrategy::Merge};
    std::string status{"pending"};
    std::string review;
    std::string result;

    json to_json() const;
    static MergeRequest from_json(const json& j);
};

class MergeCoordinator {
public:
    explicit MergeCoordinator(GitOperations& git, BranchManager& branch_mgr);

    std::string request_merge(const std::string& source, const std::string& target,
                              const std::string& requester, MergeStrategy strategy = MergeStrategy::Merge);
    bool approve_merge(const std::string& merge_id, const std::string& reviewer);
    bool reject_merge(const std::string& merge_id, const std::string& reviewer, const std::string& reason);
    bool execute_merge(const std::string& merge_id);

    std::optional<MergeRequest> get_request(const std::string& id) const;
    std::vector<MergeRequest> pending_requests() const;
    std::vector<MergeRequest> list_by_branch(const std::string& branch) const;
    std::vector<MergeRequest> list_by_requester(const std::string& requester) const;

    bool auto_merge_completed_tasks();

    using MergeCallback = std::function<void(const std::string& event, const MergeRequest& req)>;
    void on_change(MergeCallback cb);

private:
    bool perform_merge(const MergeRequest& req);

    GitOperations& git_;
    BranchManager& branch_mgr_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, MergeRequest> requests_;
    MergeCallback callback_;
};

}