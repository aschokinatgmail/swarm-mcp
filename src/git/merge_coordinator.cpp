#include "mcp_collab/merge_coordinator.hpp"
#include "mcp_collab/uuid.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

json MergeRequest::to_json() const {
    return {
        {"id", id},
        {"source_branch", source_branch},
        {"target_branch", target_branch},
        {"requester", requester},
        {"strategy", strategy == MergeStrategy::Merge ? "merge" :
                    strategy == MergeStrategy::Rebase ? "rebase" : "squash"},
        {"status", status},
        {"review", review},
        {"result", result},
    };
}

MergeRequest MergeRequest::from_json(const json& j) {
    MergeRequest r;
    r.id = j.value("id", "");
    r.source_branch = j.value("source_branch", "");
    r.target_branch = j.value("target_branch", "");
    r.requester = j.value("requester", "");
    auto s = j.value("strategy", "merge");
    if (s == "rebase") r.strategy = MergeStrategy::Rebase;
    else if (s == "squash") r.strategy = MergeStrategy::Squash;
    else r.strategy = MergeStrategy::Merge;
    r.status = j.value("status", "pending");
    r.review = j.value("review", "");
    r.result = j.value("result", "");
    return r;
}

MergeCoordinator::MergeCoordinator(GitOperations& git, BranchManager& branch_mgr)
    : git_(git), branch_mgr_(branch_mgr) {}

std::string MergeCoordinator::request_merge(const std::string& source, const std::string& target,
                                              const std::string& requester, MergeStrategy strategy) {
    MergeRequest req{
        .id = generate_uuid(),
        .source_branch = source,
        .target_branch = target,
        .requester = requester,
        .strategy = strategy,
    };

    {
        std::unique_lock lock(mutex_);
        req.status = "pending";
        requests_[req.id] = req;
    }

    spdlog::info("Merge requested: {} -> {} (strategy={}) by {}", source, target,
        strategy == MergeStrategy::Merge ? "merge" : strategy == MergeStrategy::Rebase ? "rebase" : "squash",
        requester);
    if (callback_) callback_("merge.requested", req);
    return req.id;
}

bool MergeCoordinator::approve_merge(const std::string& merge_id, const std::string& reviewer) {
    std::unique_lock lock(mutex_);
    auto it = requests_.find(merge_id);
    if (it == requests_.end()) return false;
    if (it->second.status != "pending") return false;

    it->second.status = "approved";
    it->second.review = reviewer;
    auto req = it->second;
    lock.unlock();

    spdlog::info("Merge approved: {} by {}", merge_id, reviewer);
    if (callback_) callback_("merge.approved", req);
    return true;
}

bool MergeCoordinator::reject_merge(const std::string& merge_id, const std::string& reviewer,
                                      const std::string& reason) {
    std::unique_lock lock(mutex_);
    auto it = requests_.find(merge_id);
    if (it == requests_.end()) return false;
    if (it->second.status != "pending") return false;

    it->second.status = "rejected";
    it->second.review = reviewer;
    it->second.result = reason;
    auto req = it->second;
    lock.unlock();

    spdlog::info("Merge rejected: {} by {}: {}", merge_id, reviewer, reason);
    if (callback_) callback_("merge.rejected", req);
    return true;
}

bool MergeCoordinator::execute_merge(const std::string& merge_id) {
    MergeRequest req;
    {
        std::unique_lock lock(mutex_);
        auto it = requests_.find(merge_id);
        if (it == requests_.end()) return false;
        if (it->second.status != "approved") return false;
        req = it->second;
    }

    bool ok = perform_merge(req);

    std::unique_lock lock(mutex_);
    auto it = requests_.find(merge_id);
    if (it == requests_.end()) return false;

    it->second.status = ok ? "completed" : "failed";
    it->second.result = ok ? "merged successfully" : "merge conflict or error";
    auto updated = it->second;
    lock.unlock();

    if (ok) {
        branch_mgr_.mark_merged(req.source_branch);
        spdlog::info("Merge completed: {} -> {}", req.source_branch, req.target_branch);
    } else {
        spdlog::error("Merge failed: {} -> {}", req.source_branch, req.target_branch);
    }

    if (callback_) callback_(ok ? "merge.completed" : "merge.failed", updated);
    return ok;
}

bool MergeCoordinator::perform_merge(const MergeRequest& req) {
    if (!git_.checkout(req.target_branch)) return false;

    switch (req.strategy) {
        case MergeStrategy::Merge:
            return git_.merge(req.source_branch, true);
        case MergeStrategy::Rebase:
            if (!git_.checkout(req.source_branch)) return false;
            if (!git_.rebase(req.target_branch)) return false;
            return git_.checkout(req.target_branch) && git_.merge(req.source_branch);
        case MergeStrategy::Squash:
            if (!git_.merge_squash(req.source_branch)) return false;
            if (!git_.add()) return false;
            return git_.commit(std::format("squash: merge {} into {}", req.source_branch, req.target_branch));
    }
    return false;
}

std::optional<MergeRequest> MergeCoordinator::get_request(const std::string& id) const {
    std::unique_lock lock(mutex_);
    auto it = requests_.find(id);
    if (it != requests_.end()) return it->second;
    return std::nullopt;
}

std::vector<MergeRequest> MergeCoordinator::pending_requests() const {
    std::unique_lock lock(mutex_);
    std::vector<MergeRequest> result;
    for (const auto& entry : requests_) {
        if (entry.second.status == "pending") result.push_back(entry.second);
    }
    return result;
}

std::vector<MergeRequest> MergeCoordinator::list_by_branch(const std::string& branch) const {
    std::unique_lock lock(mutex_);
    std::vector<MergeRequest> result;
    for (const auto& entry : requests_) {
        if (entry.second.source_branch == branch || entry.second.target_branch == branch) result.push_back(entry.second);
    }
    return result;
}

std::vector<MergeRequest> MergeCoordinator::list_by_requester(const std::string& requester) const {
    std::unique_lock lock(mutex_);
    std::vector<MergeRequest> result;
    for (const auto& entry : requests_) {
        if (entry.second.requester == requester) result.push_back(entry.second);
    }
    return result;
}

bool MergeCoordinator::auto_merge_completed_tasks() {
    auto active = branch_mgr_.list_active();
    bool any = false;
    for (const auto& info : active) {
        std::string diff = branch_mgr_.diff_to_base(info.name);
        if (diff.empty()) continue;

        request_merge(info.name, info.base_branch, "auto-merger", MergeStrategy::Squash);
        any = true;  // Merge requests created but NOT auto-approved — requires human review
    }
    return any;
}

void MergeCoordinator::on_change(MergeCallback cb) {
    callback_ = std::move(cb);
}

}