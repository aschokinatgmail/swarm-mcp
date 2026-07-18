#pragma once

#include <string>
#include <chrono>
#include <vector>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <functional>

#include <nlohmann/json.hpp>

namespace mcp_collab {

using json = nlohmann::json;

enum class TaskStatus {
    Pending,
    InProgress,
    Blocked,
    Completed,
    Failed,
    Cancelled
};

enum class TaskPriority {
    Low,
    Medium,
    High,
    Critical
};

struct Task {
    std::string id;
    std::string title;
    std::string description;
    std::string assignee;
    std::string creator;
    TaskStatus status{TaskStatus::Pending};
    TaskPriority priority{TaskPriority::Medium};
    std::vector<std::string> dependencies;
    std::vector<std::string> tags;
    json context;
    std::chrono::system_clock::time_point created_at{std::chrono::system_clock::now()};
    std::chrono::system_clock::time_point updated_at{std::chrono::system_clock::now()};
    std::optional<std::chrono::system_clock::time_point> deadline;
    std::optional<std::chrono::system_clock::time_point> completed_at;

    json to_json() const;
    static Task from_json(const json& j);
};

struct TaskFilter {
    std::optional<TaskStatus> status;
    std::optional<TaskPriority> priority;
    std::optional<std::string> assignee;
    std::optional<std::string> tag;
    std::string search_term;
};

std::string task_status_str(TaskStatus s);
TaskStatus task_status_from_str(const std::string& s);
std::string task_priority_str(TaskPriority p);
TaskPriority task_priority_from_str(const std::string& p);

class TaskManager {
public:
    TaskManager() = default;

    Task create_task(const std::string& title,
                    const std::string& creator,
                    const std::string& description = "",
                    TaskPriority priority = TaskPriority::Medium);

    // Restore a task with its existing ID (used by persistence load).
    // Unlike create_task, this does NOT generate a new UUID — it inserts the
    // task under its original id, preserving dependency references across
    // restarts. Emits a "task.restored" event.
    void restore_task(const Task& task);

    std::optional<Task> get_task(const std::string& id) const;
    bool update_task(const std::string& id, const Task& task);
    bool delete_task(const std::string& id);

    bool assign_task(const std::string& id, const std::string& agent_id);
    bool set_status(const std::string& id, TaskStatus status);
    bool add_dependency(const std::string& id, const std::string& dep_id);
    bool remove_dependency(const std::string& id, const std::string& dep_id);
    bool add_tag(const std::string& id, const std::string& tag);
    bool has_dependency(const std::string& id, const std::string& dep_id) const;
    bool would_create_cycle(const std::string& id, const std::string& dep_id) const;

    std::vector<Task> list_tasks(const TaskFilter& filter = {}) const;
    std::vector<Task> get_ready_tasks() const;
    std::vector<Task> get_agent_tasks(const std::string& agent_id) const;

    using TaskCallback = std::function<void(const std::string& event, const Task& task)>;
    void on_task_event(TaskCallback cb);

private:
    std::string find_available_assignee() const;
    void notify(const std::string& event, const Task& task);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Task> tasks_;
    TaskCallback callback_;
};

}