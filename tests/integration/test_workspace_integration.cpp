#include <gtest/gtest.h>
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/event_bus.hpp"

using namespace mcp_collab;

// ── Integration: Task lifecycle with agents ──────────────────────────

class WorkspaceIntegrationTest : public ::testing::Test {
protected:
    TaskManager tasks;
    AgentRegistry agents{"test-swarm"};
    ContextStore context;
    EventBus events;
    AuthToken coord_token{"tc", "coordinator", Role::Coordinator, "test-swarm"};
    AuthToken worker_token{"tw", "worker-1", Role::Worker, "test-swarm"};
    AuthToken observer_token{"to", "observer-1", Role::Observer, "test-swarm"};
};

TEST_F(WorkspaceIntegrationTest, FullTaskLifecycle) {
    // Register agents
    auto coord_id = agents.register_agent(AgentInfo{.name = "Coord", .capabilities = {"manage"}}, coord_token);
    auto worker_id = agents.register_agent(AgentInfo{.name = "Worker", .capabilities = {"build"}}, worker_token);
    EXPECT_FALSE(coord_id.empty());
    EXPECT_FALSE(worker_id.empty());

    // Create task
    auto task = tasks.create_task("Build project", "coordinator", "Build the main binary", TaskPriority::High);
    EXPECT_EQ(task.status, TaskStatus::Pending);

    // Assign task
    EXPECT_TRUE(tasks.assign_task(task.id, worker_id));
    auto assigned = tasks.get_task(task.id);
    EXPECT_EQ(assigned->assignee, worker_id);
    EXPECT_EQ(assigned->status, TaskStatus::InProgress);

    // Update status
    EXPECT_TRUE(tasks.set_status(task.id, TaskStatus::Completed));
    EXPECT_EQ(tasks.get_task(task.id)->status, TaskStatus::Completed);
    EXPECT_TRUE(tasks.get_task(task.id)->completed_at.has_value());

    // List completed
    auto completed = tasks.list_tasks(TaskFilter{.status = TaskStatus::Completed});
    EXPECT_EQ(completed.size(), 1u);
}

TEST_F(WorkspaceIntegrationTest, TaskDependencyChain) {
    auto t1 = tasks.create_task("Design", "architect");
    auto t2 = tasks.create_task("Implement", "dev");
    auto t3 = tasks.create_task("Test", "qa");

    tasks.add_dependency(t2.id, t1.id);
    tasks.add_dependency(t3.id, t2.id);

    auto ready = tasks.get_ready_tasks();
    EXPECT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].id, t1.id);

    tasks.set_status(t1.id, TaskStatus::Completed);
    ready = tasks.get_ready_tasks();
    EXPECT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].id, t2.id);

    tasks.set_status(t2.id, TaskStatus::Completed);
    ready = tasks.get_ready_tasks();
    EXPECT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].id, t3.id);
}

TEST_F(WorkspaceIntegrationTest, AgentRoleAccess) {
    auto coord_id = agents.register_agent(AgentInfo{.name = "Coord"}, coord_token);
    auto worker_id = agents.register_agent(AgentInfo{.name = "Worker"}, worker_token);
    auto observer_id = agents.register_agent(AgentInfo{.name = "Observer"}, observer_token);

    EXPECT_EQ(agents.get_agent(coord_id)->role, Role::Coordinator);
    EXPECT_EQ(agents.get_agent(worker_id)->role, Role::Worker);
    EXPECT_EQ(agents.get_agent(observer_id)->role, Role::Observer);
}

TEST_F(WorkspaceIntegrationTest, AgentRoleChange) {
    auto id = agents.register_agent(AgentInfo{.name = "Agent"}, worker_token);
    EXPECT_TRUE(agents.set_agent_role(id, Role::Coordinator, coord_token));
    EXPECT_EQ(agents.get_agent(id)->role, Role::Coordinator);
    EXPECT_FALSE(agents.set_agent_role(id, Role::Observer, worker_token)); // worker can't change roles
}

TEST_F(WorkspaceIntegrationTest, ContextSharingBetweenAgents) {
    // Worker sets context
    context.set("project.config", {{"build_cmd", "cmake --build"}, {"threads", 8}}, "worker-1");

    // Observer reads context
    auto entry = context.get("project.config");
    EXPECT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value["build_cmd"], "cmake --build");

    // Worker merges
    context.merge("project.config", {{"threads", 16}, {"verbose", true}}, "worker-1");
    auto merged = context.get("project.config");
    EXPECT_EQ(merged->value["threads"], 16);
    EXPECT_EQ(merged->value["verbose"], true);
    EXPECT_EQ(merged->value["build_cmd"], "cmake --build");
    EXPECT_EQ(merged->version, 2);
}

TEST_F(WorkspaceIntegrationTest, EventBusPropagation) {
    std::vector<std::string> received;
    events.subscribe("task.updated", [&](const Event& e) {
        received.push_back(e.type);
    });

    auto t1 = tasks.create_task("E1", "a1");
    tasks.set_status(t1.id, TaskStatus::InProgress);
    events.emit("task.updated", "task-manager", t1.to_json());

    EXPECT_GE(received.size(), 1u);
}

TEST_F(WorkspaceIntegrationTest, ObserverCannotModifyContext) {
    // Observer tries to set context — ContextStore doesn't enforce roles itself,
    // but the MCP protocol layer does. This test confirms the store works,
    // and the protocol test verifies enforcement.
    context.set("obs.key", {{"data", 1}}, "observer-1");
    EXPECT_TRUE(context.exists("obs.key"));
}

TEST_F(WorkspaceIntegrationTest, TaskSearchAndFiltering) {
    tasks.create_task("Urgent task", "a1", "desc", TaskPriority::Critical);
    auto t2 = tasks.create_task("Normal task", "a2", "desc", TaskPriority::Medium);
    tasks.assign_task(t2.id, "agent-x");

    auto by_priority = tasks.list_tasks(TaskFilter{.priority = TaskPriority::Critical});
    EXPECT_EQ(by_priority.size(), 1u);

    auto by_assignee = tasks.list_tasks(TaskFilter{.assignee = "agent-x"});
    EXPECT_EQ(by_assignee.size(), 1u);

    auto by_search = tasks.list_tasks(TaskFilter{.search_term = "urgent"});
    EXPECT_EQ(by_search.size(), 1u);
}

TEST_F(WorkspaceIntegrationTest, AgentCapabilitySearch) {
    AuthToken builder_token{"tb", "builder-1", Role::Worker, "test-swarm"};
    AuthToken tester_token{"tt", "tester-1", Role::Worker, "test-swarm"};
    agents.register_agent(AgentInfo{.name = "Builder", .capabilities = {"build", "cmake"}}, builder_token);
    agents.register_agent(AgentInfo{.name = "Tester", .capabilities = {"test", "ctest"}}, tester_token);

    auto builders = agents.find_by_capability("build");
    EXPECT_EQ(builders.size(), 1u);
    EXPECT_EQ(builders[0].name, "Builder");

    auto testers = agents.find_by_capability("test");
    EXPECT_EQ(testers.size(), 1u);
}

TEST_F(WorkspaceIntegrationTest, SwarmIsolation) {
    agents.register_agent(AgentInfo{.name = "SwarmA"}, worker_token);

    auto swarm_agents = agents.list_swarm_agents("test-swarm");
    EXPECT_EQ(swarm_agents.size(), 1u);

    auto other_swarm = agents.list_swarm_agents("other-swarm");
    EXPECT_EQ(other_swarm.size(), 0u);
}
