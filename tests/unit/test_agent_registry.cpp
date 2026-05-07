#include <gtest/gtest.h>
#include "mcp_collab/agent_registry.hpp"

using namespace mcp_collab;

class AgentRegistryTest : public ::testing::Test {
protected:
    AgentRegistry registry{"test-swarm"};
    AuthToken coordinator_token{"tid-1", "coordinator-1", Role::Coordinator, "test-swarm"};
    AuthToken worker_token{"tid-2", "worker-1", Role::Worker, "test-swarm"};
    AuthToken observer_token{"tid-3", "observer-1", Role::Observer, "test-swarm"};
};

TEST_F(AgentRegistryTest, RegisterAgent) {
    AgentInfo info{.name = "TestAgent", .capabilities = {"build", "test"}};
    auto id = registry.register_agent(info, worker_token);
    EXPECT_FALSE(id.empty());
    auto agent = registry.get_agent(id);
    EXPECT_TRUE(agent.has_value());
    EXPECT_EQ(agent->name, "TestAgent");
    EXPECT_EQ(agent->swarm_id, "test-swarm");
    EXPECT_EQ(agent->role, Role::Worker);
}

TEST_F(AgentRegistryTest, RegisterAgentSwarmMismatch) {
    AgentInfo info{.name = "WrongSwarm"};
    AuthToken wrong_token{"tid", "agent", Role::Worker, "other-swarm"};
    auto id = registry.register_agent(info, wrong_token);
    EXPECT_TRUE(id.empty());
}

TEST_F(AgentRegistryTest, UpdateAgent) {
    AgentInfo info{.name = "Agent1"};
    auto id = registry.register_agent(info, worker_token);
    AgentInfo update;
    update.name = "Agent1-Updated";
    update.capabilities = {"new-cap"};
    EXPECT_TRUE(registry.update_agent(id, update, worker_token));
    EXPECT_EQ(registry.get_agent(id)->name, "Agent1-Updated");
}

TEST_F(AgentRegistryTest, UpdateAgentUnauthorized) {
    AgentInfo info{.name = "Agent1"};
    auto id = registry.register_agent(info, worker_token);
    AgentInfo update;
    update.name = "Hacked";
    EXPECT_FALSE(registry.update_agent(id, update, observer_token));
}

TEST_F(AgentRegistryTest, DeregisterAgent) {
    AgentInfo info{.name = "Agent1"};
    auto id = registry.register_agent(info, worker_token);
    EXPECT_TRUE(registry.deregister_agent(id, worker_token));
    EXPECT_FALSE(registry.get_agent(id).has_value());
}

TEST_F(AgentRegistryTest, DeregisterUnauthorized) {
    AgentInfo info{.name = "Agent1"};
    auto id = registry.register_agent(info, worker_token);
    EXPECT_FALSE(registry.deregister_agent(id, observer_token));
}

TEST_F(AgentRegistryTest, CoordinatorCanDeregister) {
    AgentInfo info{.name = "Agent1"};
    auto id = registry.register_agent(info, worker_token);
    EXPECT_TRUE(registry.deregister_agent(id, coordinator_token));
}

TEST_F(AgentRegistryTest, Heartbeat) {
    AgentInfo info{.name = "Agent1"};
    auto id = registry.register_agent(info, worker_token);
    EXPECT_TRUE(registry.heartbeat(id));
    EXPECT_TRUE(registry.get_agent(id)->status == AgentStatus::Online);
}

TEST_F(AgentRegistryTest, HeartbeatNonexistent) {
    EXPECT_FALSE(registry.heartbeat("nonexistent"));
}

TEST_F(AgentRegistryTest, ListAgents) {
    registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    registry.register_agent(AgentInfo{.name = "A2"}, coordinator_token);
    EXPECT_EQ(registry.list_agents().size(), 2u);
}

TEST_F(AgentRegistryTest, ListAgentsByStatus) {
    auto id = registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    EXPECT_EQ(registry.list_agents(AgentStatus::Online).size(), 1u);
    EXPECT_EQ(registry.list_agents(AgentStatus::Busy).size(), 0u);
}

TEST_F(AgentRegistryTest, ListSwarmAgents) {
    registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    registry.register_agent(AgentInfo{.name = "A2"}, coordinator_token);
    EXPECT_EQ(registry.list_swarm_agents("test-swarm").size(), 2u);
    EXPECT_EQ(registry.list_swarm_agents("other-swarm").size(), 0u);
}

TEST_F(AgentRegistryTest, FindByCapability) {
    AgentInfo info{.name = "Builder", .capabilities = {"build", "compile"}};
    registry.register_agent(info, worker_token);
    auto found = registry.find_by_capability("build");
    EXPECT_EQ(found.size(), 1u);
    found = registry.find_by_capability("deploy");
    EXPECT_EQ(found.size(), 0u);
}

TEST_F(AgentRegistryTest, FindIdle) {
    registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    auto idle = registry.find_idle();
    EXPECT_EQ(idle.size(), 1u);
}

TEST_F(AgentRegistryTest, FindIdleInSwarm) {
    registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    EXPECT_EQ(registry.find_idle_in_swarm("test-swarm").size(), 1u);
    EXPECT_EQ(registry.find_idle_in_swarm("other").size(), 0u);
}

TEST_F(AgentRegistryTest, FindByRole) {
    registry.register_agent(AgentInfo{.name = "Coord"}, coordinator_token);
    registry.register_agent(AgentInfo{.name = "Work"}, worker_token);
    EXPECT_EQ(registry.find_by_role(Role::Coordinator).size(), 1u);
    EXPECT_EQ(registry.find_by_role(Role::Worker).size(), 1u);
}

TEST_F(AgentRegistryTest, PruneStale) {
    AgentInfo info{.name = "Stale"};
    auto id = registry.register_agent(info, worker_token);
    auto pruned = registry.prune_stale(std::chrono::seconds(0));
    EXPECT_GE(pruned, 1u);
    EXPECT_FALSE(registry.get_agent(id).has_value());
}

TEST_F(AgentRegistryTest, Count) {
    EXPECT_EQ(registry.count(), 0u);
    registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    EXPECT_EQ(registry.count(), 1u);
    registry.register_agent(AgentInfo{.name = "A2"}, coordinator_token);
    EXPECT_EQ(registry.count(), 2u);
}

TEST_F(AgentRegistryTest, SwarmCount) {
    registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    registry.register_agent(AgentInfo{.name = "A2"}, coordinator_token);
    EXPECT_EQ(registry.swarm_count("test-swarm"), 2u);
    EXPECT_EQ(registry.swarm_count("other"), 0u);
}

TEST_F(AgentRegistryTest, SetAgentRole) {
    auto id = registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    EXPECT_TRUE(registry.set_agent_role(id, Role::Coordinator, coordinator_token));
    EXPECT_EQ(registry.get_agent(id)->role, Role::Coordinator);
}

TEST_F(AgentRegistryTest, SetAgentRoleUnauthorized) {
    auto id = registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    EXPECT_FALSE(registry.set_agent_role(id, Role::Coordinator, worker_token));
}

TEST_F(AgentRegistryTest, Callback) {
    std::string last_event;
    registry.on_change([&](const std::string& ev, const AgentInfo& a) { last_event = ev; });
    registry.register_agent(AgentInfo{.name = "A1"}, worker_token);
    EXPECT_EQ(last_event, "agent.registered");
}

TEST_F(AgentRegistryTest, AgentInfoJsonRoundTrip) {
    AgentInfo info;
    info.id = "a-1";
    info.name = "Test";
    info.swarm_id = "sw-1";
    info.role = Role::Worker;
    info.capabilities = {"x", "y"};
    info.status = AgentStatus::Online;
    auto j = info.to_json();
    auto restored = AgentInfo::from_json(j);
    EXPECT_EQ(restored.id, info.id);
    EXPECT_EQ(restored.name, info.name);
    EXPECT_EQ(restored.swarm_id, info.swarm_id);
    EXPECT_EQ(restored.role, Role::Worker);
    EXPECT_EQ(restored.status, AgentStatus::Online);
    EXPECT_EQ(restored.capabilities.size(), 2u);
}