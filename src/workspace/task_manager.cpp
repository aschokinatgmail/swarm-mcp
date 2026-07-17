#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/uuid.hpp"
#include <algorithm>
#include <unordered_set>
#include <spdlog/spdlog.h>

namespace mcp_collab {

std::string task_status_str(TaskStatus s) {
    switch (s) {
        case TaskStatus::Pending:    return "pending";
        case TaskStatus::InProgress:  return "in_progress";
        case TaskStatus::Blocked:     return "blocked";
        case TaskStatus::Completed:   return "completed";
        case TaskStatus::Failed:      return "failed";
        case TaskStatus::Cancelled:   return "cancelled";
    }
    return "unknown";
}

TaskStatus task_status_from_str(const std::string& s) {
    if (s == "in_progress") return TaskStatus::InProgress;
    if (s == "blocked")     return TaskStatus::Blocked;
    if (s == "completed")   return TaskStatus::Completed;
    if (s == "failed")      return TaskStatus::Failed;
    if (s == "cancelled")   return TaskStatus::Cancelled;
    return TaskStatus::Pending;
}

std::string task_priority_str(TaskPriority p) {
    switch (p) {
        case TaskPriority::Low:      return "low";
        case TaskPriority::Medium:   return "medium";
        case TaskPriority::High:     return "high";
        case TaskPriority::Critical: return "critical";
    }
    return "medium";
}

TaskPriority task_priority_from_str(const std::string& p) {
    if (p == "low")      return TaskPriority::Low;
    if (p == "high")     return TaskPriority::High;
    if (p == "critical") return TaskPriority::Critical;
    return TaskPriority::Medium;
}

json Task::to_json() const {
    json j = {
        {"id", id},
        {"title", title},
        {"description", description},
        {"assignee", assignee},
        {"creator", creator},
        {"status", task_status_str(status)},
        {"priority", task_priority_str(priority)},
        {"dependencies", dependencies},
        {"tags", tags},
        {"context", context},
        {"created_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            created_at.time_since_epoch()).count()},
        {"updated_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            updated_at.time_since_epoch()).count()},
    };
    if (deadline) {
        j["deadline"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline->time_since_epoch()).count();
    }
    if (completed_at) {
        j["completed_at"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            completed_at->time_since_epoch()).count();
    }
    return j;
}

Task Task::from_json(const json& j) {
    Task t;
    t.id = j.value("id", "");
    t.title = j.value("title", "");
    t.description = j.value("description", "");
    t.assignee = j.value("assignee", "");
    t.creator = j.value("creator", "");
    t.status = task_status_from_str(j.value("status", "pending"));
    t.priority = task_priority_from_str(j.value("priority", "medium"));
    t.dependencies = j.value("dependencies", std::vector<std::string>{});
    t.tags = j.value("tags", std::vector<std::string>{});
    t.context = j.value("context", json::object());

    auto ts = j.value("created_at", 0LL);
    t.created_at = std::chrono::system_clock::time_point{} +
        std::chrono::milliseconds(ts);
    auto ts2 = j.value("updated_at", 0LL);
    t.updated_at = std::chrono::system_clock::time_point{} +
        std::chrono::milliseconds(ts2);

    if (j.contains("deadline") && j["deadline"].is_number()) {
        t.deadline = std::chrono::system_clock::time_point{} +
            std::chrono::milliseconds(j["deadline"].get<int64_t>());
    }
    if (j.contains("completed_at") && j["completed_at"].is_number()) {
        t.completed_at = std::chrono::system_clock::time_point{} +
            std::chrono::milliseconds(j["completed_at"].get<int64_t>());
    }
    return t;
}

Task TaskManager::create_task(const std::string& title, const std::string& creator,
                              const std::string& description, TaskPriority priority) {
    Task task;
    task.id = generate_uuid();
    task.title = title;
    task.description = description;
    task.creator = creator;
    task.priority = priority;

    {
        std::unique_lock lock(mutex_);
        tasks_[task.id] = task;
    }

    notify("task.created", task);
    spdlog::info("Task created: id={} title=\"{}\"", task.id, task.title);
    return task;
}

std::optional<Task> TaskManager::get_task(const std::string& id) const {
    std::shared_lock lock(mutex_);
    auto it = tasks_.find(id);
    if (it != tasks_.end()) return it->second;
    return std::nullopt;
}

bool TaskManager::update_task(const std::string& id, const Task& task) {
    Task snapshot;
    bool changed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;

        it->second = task;
        it->second.updated_at = std::chrono::system_clock::now();
        snapshot = it->second;
        changed = true;
    }
    if (changed) notify("task.updated", snapshot);
    return true;
}

bool TaskManager::delete_task(const std::string& id) {
    Task snapshot;
    bool changed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;

        snapshot = it->second;
        tasks_.erase(it);
        changed = true;
    }
    if (changed) notify("task.deleted", snapshot);
    return true;
}

bool TaskManager::assign_task(const std::string& id, const std::string& agent_id) {
    Task snapshot;
    bool changed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;

        it->second.assignee = agent_id;
        it->second.status = TaskStatus::InProgress;
        it->second.updated_at = std::chrono::system_clock::now();
        snapshot = it->second;
        changed = true;
    }
    if (changed) notify("task.assigned", snapshot);
    return true;
}

bool TaskManager::set_status(const std::string& id, TaskStatus status) {
    Task snapshot;
    bool changed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;

        it->second.status = status;
        it->second.updated_at = std::chrono::system_clock::now();

        if (status == TaskStatus::Completed) {
            it->second.completed_at = std::chrono::system_clock::now();
        }

        snapshot = it->second;
        changed = true;
    }
    if (changed) notify("task.status_changed", snapshot);
    return true;
}

bool TaskManager::add_dependency(const std::string& id, const std::string& dep_id) {
    Task snapshot;
    bool changed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;
        if (tasks_.find(dep_id) == tasks_.end()) return false; // dependency must exist

        // Self-dependency
        if (id == dep_id) {
            spdlog::warn("Cannot add self-dependency: task={} dep={}", id, dep_id);
            return false;
        }

        // Circular dependency check
        if (would_create_cycle(id, dep_id)) {
            spdlog::warn("Circular dependency detected: task={} dep={}", id, dep_id);
            return false;
        }

        auto& deps = it->second.dependencies;
        if (std::ranges::find(deps, dep_id) == deps.end()) {
            deps.push_back(dep_id);
            it->second.updated_at = std::chrono::system_clock::now();
            snapshot = it->second;
            changed = true;
        }
    }
    if (changed) notify("task.dependency_added", snapshot);
    return true;
}

bool TaskManager::remove_dependency(const std::string& id, const std::string& dep_id) {
    Task snapshot;
    bool changed = false;
    {
        std::unique_lock lock(mutex_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return false;

        auto& deps = it->second.dependencies;
        auto dep_it = std::ranges::find(deps, dep_id);
        if (dep_it != deps.end()) {
            deps.erase(dep_it);
            it->second.updated_at = std::chrono::system_clock::now();
            snapshot = it->second;
            changed = true;
        }
    }
    if (changed) notify("task.dependency_removed", snapshot);
    return true;
}

bool TaskManager::add_tag(const std::string& id, const std::string& tag) {
    std::unique_lock lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return false;

    auto& tags = it->second.tags;
    if (std::ranges::find(tags, tag) == tags.end()) {
        tags.push_back(tag);
        it->second.updated_at = std::chrono::system_clock::now();
    }
    return true;
}

std::vector<Task> TaskManager::list_tasks(const TaskFilter& filter) const {
    std::shared_lock lock(mutex_);
    std::vector<Task> result;
    for (const auto& entry : tasks_) {
        const auto& task = entry.second;
        if (filter.status && task.status != *filter.status) continue;
        if (filter.priority && task.priority != *filter.priority) continue;
        if (filter.assignee && task.assignee != *filter.assignee) continue;
        if (filter.tag) {
            if (std::ranges::find(task.tags, *filter.tag) == task.tags.end()) continue;
        }
        if (!filter.search_term.empty()) {
            auto term = filter.search_term;
            auto lower = [](std::string s) { std::ranges::transform(s, s.begin(), ::tolower); return s; };
            if (lower(task.title).find(lower(term)) == std::string::npos &&
                lower(task.description).find(lower(term)) == std::string::npos) continue;
        }
        result.push_back(task);
    }
    return result;
}

std::vector<Task> TaskManager::get_ready_tasks() const {
    std::shared_lock lock(mutex_);
    std::vector<Task> result;
    for (const auto& entry : tasks_) {
        const auto& task = entry.second;
        if (task.status != TaskStatus::Pending) continue;

        bool all_deps_complete = true;
        for (const auto& dep_id : task.dependencies) {
            auto dep_it = tasks_.find(dep_id);
            if (dep_it == tasks_.end() || dep_it->second.status != TaskStatus::Completed) {
                all_deps_complete = false;
                break;
            }
        }
        if (all_deps_complete) result.push_back(task);
    }
    return result;
}

std::vector<Task> TaskManager::get_agent_tasks(const std::string& agent_id) const {
    TaskFilter f;
    f.assignee = agent_id;
    return list_tasks(f);
}

void TaskManager::on_task_event(TaskCallback cb) {
    callback_ = std::move(cb);
}

bool TaskManager::has_dependency(const std::string& id, const std::string& dep_id) const {
    std::shared_lock lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return false;
    const auto& deps = it->second.dependencies;
    return std::ranges::find(deps, dep_id) != deps.end();
}

bool TaskManager::would_create_cycle(const std::string& id, const std::string& dep_id) const {
    // DFS from dep_id: if we can reach id, adding dep_id→id creates a cycle
    std::unordered_set<std::string> visited;
    std::vector<std::string> stack;
    stack.push_back(dep_id);

    while (!stack.empty()) {
        std::string current = stack.back();
        stack.pop_back();

        if (current == id) return true;
        if (visited.contains(current)) continue;
        visited.insert(current);

        auto it = tasks_.find(current);
        if (it == tasks_.end()) continue;
        for (const auto& dep : it->second.dependencies) {
            stack.push_back(dep);
        }
    }
    return false;
}

void TaskManager::notify(const std::string& event, const Task& task) {
    if (callback_) callback_(event, task);
}

std::string TaskManager::find_available_assignee() const {
    return "";
}

}