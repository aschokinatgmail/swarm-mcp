#include "mcp_collab/branch_manager.hpp"
#include "mcp_collab/uuid.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

json BranchInfo::to_json() const {
    return {
        {"name", name},
        {"task_id", task_id},
        {"agent_id", agent_id},
        {"state", state == BranchState::Active ? "active" :
                 state == BranchState::Merged ? "merged" :
                 state == BranchState::Abandoned ? "abandoned" : "locked"},
        {"base_branch", base_branch},
        {"created_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            created_at.time_since_epoch()).count()},
    };
}

BranchInfo BranchInfo::from_json(const json& j) {
    BranchInfo b;
    b.name = j.value("name", "");
    b.task_id = j.value("task_id", "");
    b.agent_id = j.value("agent_id", "");
    auto s = j.value("state", "active");
    if (s == "merged") b.state = BranchState::Merged;
    else if (s == "abandoned") b.state = BranchState::Abandoned;
    else if (s == "locked") b.state = BranchState::Locked;
    else b.state = BranchState::Active;
    b.base_branch = j.value("base_branch", "main");
    auto ts = j.value("created_at", 0LL);
    b.created_at = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(ts);
    return b;
}

BranchManager::BranchManager(GitOperations& git, const std::string& prefix)
    : git_(git), prefix_(prefix) {}

std::string BranchManager::make_branch_name(const std::string& task_id) const {
    return std::format("{}{}", prefix_, task_id);
}

std::string BranchManager::create_branch(const std::string& task_id, const std::string& agent_id,
                                           const std::string& base) {
    std::string branch_name = make_branch_name(task_id);
    std::string base_branch = base.empty() ? git_.current_branch() : base;
    if (base_branch.empty()) base_branch = "main";

    if (!git_.checkout(branch_name, true)) {
        spdlog::error("Failed to create branch: {}", branch_name);
        return "";
    }

    BranchInfo info{
        .name = branch_name,
        .task_id = task_id,
        .agent_id = agent_id,
        .state = BranchState::Active,
        .base_branch = base_branch,
    };

    {
        std::unique_lock lock(mutex_);
        branches_[branch_name] = info;
    }

    spdlog::info("Branch created: {} from {} (task={} agent={})", branch_name, base_branch, task_id, agent_id);
    if (callback_) callback_("branch.created", info);
    return branch_name;
}

bool BranchManager::lock_branch(const std::string& branch) {
    std::unique_lock lock(mutex_);
    auto it = branches_.find(branch);
    if (it == branches_.end()) return false;
    it->second.state = BranchState::Locked;
    auto info = it->second;
    lock.unlock();
    if (callback_) callback_("branch.locked", info);
    return true;
}

bool BranchManager::unlock_branch(const std::string& branch) {
    std::unique_lock lock(mutex_);
    auto it = branches_.find(branch);
    if (it == branches_.end()) return false;
    it->second.state = BranchState::Active;
    auto info = it->second;
    lock.unlock();
    if (callback_) callback_("branch.unlocked", info);
    return true;
}

bool BranchManager::mark_merged(const std::string& branch) {
    std::unique_lock lock(mutex_);
    auto it = branches_.find(branch);
    if (it == branches_.end()) return false;
    it->second.state = BranchState::Merged;
    auto info = it->second;
    lock.unlock();
    if (callback_) callback_("branch.merged", info);
    return true;
}

bool BranchManager::mark_abandoned(const std::string& branch) {
    std::unique_lock lock(mutex_);
    auto it = branches_.find(branch);
    if (it == branches_.end()) return false;
    it->second.state = BranchState::Abandoned;
    auto info = it->second;
    lock.unlock();
    if (callback_) callback_("branch.abandoned", info);
    return true;
}

std::optional<BranchInfo> BranchManager::get_branch_info(const std::string& branch) const {
    std::shared_lock lock(mutex_);
    auto it = branches_.find(branch);
    if (it != branches_.end()) return it->second;
    return std::nullopt;
}

std::vector<BranchInfo> BranchManager::list_active() const {
    std::shared_lock lock(mutex_);
    std::vector<BranchInfo> result;
    for (const auto& [_, info] : branches_) {
        if (info.state == BranchState::Active) result.push_back(info);
    }
    return result;
}

std::vector<BranchInfo> BranchManager::list_by_agent(const std::string& agent_id) const {
    std::shared_lock lock(mutex_);
    std::vector<BranchInfo> result;
    for (const auto& [_, info] : branches_) {
        if (info.agent_id == agent_id) result.push_back(info);
    }
    return result;
}

std::optional<BranchInfo> BranchManager::find_by_task(const std::string& task_id) const {
    std::shared_lock lock(mutex_);
    for (const auto& [_, info] : branches_) {
        if (info.task_id == task_id) return info;
    }
    return std::nullopt;
}

bool BranchManager::commit_changes(const std::string& branch, const std::string& message,
                                      const std::string& agent_id) {
    if (!git_.checkout(branch)) {
        spdlog::error("Failed to checkout branch for commit: {}", branch);
        return false;
    }
    if (!git_.add("-A")) {
        spdlog::error("Failed to stage changes on branch: {}", branch);
        return false;
    }
    if (!git_.commit(message, agent_id)) {
        spdlog::error("Failed to commit on branch: {}", branch);
        return false;
    }
    spdlog::info("Committed on {}: \"{}\"", branch, message);
    return true;
}

std::string BranchManager::diff_to_base(const std::string& branch) const {
    std::shared_lock lock(mutex_);
    auto it = branches_.find(branch);
    if (it == branches_.end()) return "";
    return git_.diff(it->second.base_branch, branch);
}

void BranchManager::on_change(BranchCallback cb) {
    callback_ = std::move(cb);
}

}