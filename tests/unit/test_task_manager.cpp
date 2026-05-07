#include <gtest/gtest.h>
#include "mcp_collab/task_manager.hpp"
#include <thread>

using namespace mcp_collab;

class TaskManagerTest : public ::testing::Test {
protected:
    TaskManager tm;
};

TEST_F(TaskManagerTest, CreateTask) {
    auto task = tm.create_task("Test task", "agent-1", "Description", TaskPriority::High);
    EXPECT_EQ(task.title, "Test task");
    EXPECT_EQ(task.creator, "agent-1");
    EXPECT_EQ(task.description, "Description");
    EXPECT_EQ(task.priority, TaskPriority::High);
    EXPECT_EQ(task.status, TaskStatus::Pending);
    EXPECT_FALSE(task.id.empty());
}

TEST_F(TaskManagerTest, GetTask) {
    auto created = tm.create_task("Get test", "agent-1");
    auto fetched = tm.get_task(created.id);
    EXPECT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->id, created.id);
    EXPECT_EQ(fetched->title, "Get test");
}

TEST_F(TaskManagerTest, GetNonexistentTask) {
    auto result = tm.get_task("nonexistent-id");
    EXPECT_FALSE(result.has_value());
}

TEST_F(TaskManagerTest, UpdateTask) {
    auto task = tm.create_task("Update test", "agent-1");
    task.description = "Updated description";
    EXPECT_TRUE(tm.update_task(task.id, task));
    auto fetched = tm.get_task(task.id);
    EXPECT_EQ(fetched->description, "Updated description");
}

TEST_F(TaskManagerTest, DeleteTask) {
    auto task = tm.create_task("Delete test", "agent-1");
    EXPECT_TRUE(tm.delete_task(task.id));
    EXPECT_FALSE(tm.get_task(task.id).has_value());
}

TEST_F(TaskManagerTest, DeleteNonexistent) {
    EXPECT_FALSE(tm.delete_task("nonexistent"));
}

TEST_F(TaskManagerTest, AssignTask) {
    auto task = tm.create_task("Assign test", "agent-1");
    EXPECT_TRUE(tm.assign_task(task.id, "agent-2"));
    auto fetched = tm.get_task(task.id);
    EXPECT_EQ(fetched->assignee, "agent-2");
    EXPECT_EQ(fetched->status, TaskStatus::InProgress);
}

TEST_F(TaskManagerTest, SetStatus) {
    auto task = tm.create_task("Status test", "agent-1");
    EXPECT_TRUE(tm.set_status(task.id, TaskStatus::InProgress));
    EXPECT_EQ(tm.get_task(task.id)->status, TaskStatus::InProgress);
    EXPECT_TRUE(tm.set_status(task.id, TaskStatus::Completed));
    EXPECT_EQ(tm.get_task(task.id)->status, TaskStatus::Completed);
    EXPECT_TRUE(tm.get_task(task.id)->completed_at.has_value());
}

TEST_F(TaskManagerTest, AddAndRemoveDependency) {
    auto t1 = tm.create_task("First", "agent-1");
    auto t2 = tm.create_task("Second", "agent-1");
    EXPECT_TRUE(tm.add_dependency(t2.id, t1.id));
    auto fetched = tm.get_task(t2.id);
    EXPECT_EQ(fetched->dependencies.size(), 1u);
    EXPECT_EQ(fetched->dependencies[0], t1.id);
    EXPECT_TRUE(tm.remove_dependency(t2.id, t1.id));
    EXPECT_EQ(tm.get_task(t2.id)->dependencies.size(), 0u);
}

TEST_F(TaskManagerTest, DuplicateDependency) {
    auto t1 = tm.create_task("Dep1", "agent-1");
    auto t2 = tm.create_task("Dep2", "agent-1");
    tm.add_dependency(t2.id, t1.id);
    tm.add_dependency(t2.id, t1.id); // duplicate
    EXPECT_EQ(tm.get_task(t2.id)->dependencies.size(), 1u);
}

TEST_F(TaskManagerTest, AddTag) {
    auto task = tm.create_task("Tag test", "agent-1");
    EXPECT_TRUE(tm.add_tag(task.id, "urgent"));
    EXPECT_TRUE(tm.add_tag(task.id, "backend"));
    auto fetched = tm.get_task(task.id);
    EXPECT_EQ(fetched->tags.size(), 2u);
    tm.add_tag(task.id, "urgent"); // duplicate
    EXPECT_EQ(tm.get_task(task.id)->tags.size(), 2u);
}

TEST_F(TaskManagerTest, ListTasks) {
    tm.create_task("Task 1", "a1", "", TaskPriority::Low);
    tm.create_task("Task 2", "a2", "", TaskPriority::High);
    auto list = tm.list_tasks();
    EXPECT_EQ(list.size(), 2u);
}

TEST_F(TaskManagerTest, ListTasksFilterByStatus) {
    auto t1 = tm.create_task("Pending", "a1");
    auto t2 = tm.create_task("In progress", "a2");
    tm.set_status(t2.id, TaskStatus::InProgress);
    auto list = tm.list_tasks(TaskFilter{.status = TaskStatus::InProgress});
    EXPECT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, t2.id);
}

TEST_F(TaskManagerTest, ListTasksFilterByAssignee) {
    auto t1 = tm.create_task("A1 task", "agent-1");
    tm.assign_task(t1.id, "agent-1");
    tm.create_task("Unassigned", "agent-2");
    auto list = tm.list_tasks(TaskFilter{.assignee = "agent-1"});
    EXPECT_EQ(list.size(), 1u);
}

TEST_F(TaskManagerTest, GetReadyTasks) {
    auto t1 = tm.create_task("Prerequisite", "a1");
    auto t2 = tm.create_task("Dependent", "a2");
    tm.add_dependency(t2.id, t1.id);

    auto ready = tm.get_ready_tasks();
    EXPECT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].id, t1.id);

    tm.set_status(t1.id, TaskStatus::Completed);
    ready = tm.get_ready_tasks();
    EXPECT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].id, t2.id);
}

TEST_F(TaskManagerTest, GetAgentTasks) {
    auto t1 = tm.create_task("T1", "a1");
    auto t2 = tm.create_task("T2", "a1");
    tm.create_task("T3", "a2");
    tm.assign_task(t1.id, "agent-x");
    tm.assign_task(t2.id, "agent-x");
    auto list = tm.get_agent_tasks("agent-x");
    EXPECT_EQ(list.size(), 2u);
}

TEST_F(TaskManagerTest, TaskCallback) {
    std::string last_event;
    tm.on_task_event([&](const std::string& ev, const Task& t) {
        last_event = ev;
    });
    tm.create_task("Callback test", "a1");
    EXPECT_EQ(last_event, "task.created");
}

TEST_F(TaskManagerTest, TaskToJsonRoundTrip) {
    auto task = tm.create_task("JSON test", "agent-1", "desc", TaskPriority::Critical);
    auto json = task.to_json();
    auto restored = Task::from_json(json);
    EXPECT_EQ(restored.id, task.id);
    EXPECT_EQ(restored.title, task.title);
    EXPECT_EQ(restored.priority, task.priority);
}

TEST_F(TaskManagerTest, StatusStringConversion) {
    EXPECT_EQ(task_status_str(TaskStatus::Pending), "pending");
    EXPECT_EQ(task_status_str(TaskStatus::InProgress), "in_progress");
    EXPECT_EQ(task_status_str(TaskStatus::Blocked), "blocked");
    EXPECT_EQ(task_status_str(TaskStatus::Completed), "completed");
    EXPECT_EQ(task_status_str(TaskStatus::Failed), "failed");
    EXPECT_EQ(task_status_str(TaskStatus::Cancelled), "cancelled");
    EXPECT_EQ(task_status_from_str("in_progress"), TaskStatus::InProgress);
    EXPECT_EQ(task_status_from_str("unknown"), TaskStatus::Pending);
}

TEST_F(TaskManagerTest, PriorityStringConversion) {
    EXPECT_EQ(task_priority_str(TaskPriority::Low), "low");
    EXPECT_EQ(task_priority_str(TaskPriority::Medium), "medium");
    EXPECT_EQ(task_priority_str(TaskPriority::High), "high");
    EXPECT_EQ(task_priority_str(TaskPriority::Critical), "critical");
    EXPECT_EQ(task_priority_from_str("low"), TaskPriority::Low);
    EXPECT_EQ(task_priority_from_str("critical"), TaskPriority::Critical);
    EXPECT_EQ(task_priority_from_str("unknown"), TaskPriority::Medium);
}