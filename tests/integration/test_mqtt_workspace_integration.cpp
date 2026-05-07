#include <gtest/gtest.h>
#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/context_store.hpp"

using namespace mcp_collab;

class MqttWorkspaceIntegrationTest : public ::testing::Test {
protected:
    std::string swarm_id = "integration-swarm";
    std::string secret = "integration-test-secret";
};

// Note: These tests verify the envelope/ACL logic without requiring a running MQTT broker.
// The MQTT client itself requires a broker and is tested separately in live integration tests.

TEST_F(MqttWorkspaceIntegrationTest, EnvelopeSignVerifyRoundTrip) {
    json payload = {{"type", "task.created"}, {"data", {{"task_id", "t-123"}}}};
    auto envelope = MqttEnvelope::sign("agent-1", swarm_id, Role::Worker, payload, secret);
    auto j = envelope.to_json();

    auto verified = MqttEnvelope::verify(j.dump(), secret);
    EXPECT_TRUE(verified.has_value());
    EXPECT_EQ(verified->sender, "agent-1");
    EXPECT_EQ(verified->swarm_id, swarm_id);
    EXPECT_EQ(verified->payload["type"], "task.created");
}

TEST_F(MqttWorkspaceIntegrationTest, EnvelopeRejectsTampering) {
    json payload = {{"action", "heartbeat"}};
    auto envelope = MqttEnvelope::sign("agent-1", swarm_id, Role::Worker, payload, secret);
    auto j = envelope.to_json();

    // Tamper with the payload
    j["payload"]["action"] = "malicious";
    EXPECT_FALSE(MqttEnvelope::verify(j.dump(), secret).has_value());
}

TEST_F(MqttWorkspaceIntegrationTest, EnvelopeRejectsWrongSecret) {
    json payload = {{"x", 1}};
    auto envelope = MqttEnvelope::sign("a", swarm_id, Role::Worker, payload, secret);
    EXPECT_FALSE(MqttEnvelope::verify(envelope.to_json().dump(), "wrong-secret").has_value());
}

TEST_F(MqttWorkspaceIntegrationTest, EnvelopeFreshness) {
    json payload = {{"test", true}};
    auto envelope = MqttEnvelope::sign("a", swarm_id, Role::Worker, payload, secret);
    EXPECT_TRUE(envelope.is_fresh(std::chrono::seconds(300)));
    EXPECT_TRUE(envelope.is_fresh(std::chrono::hours(1)));

    MqttEnvelope stale;
    stale.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 600000;
    EXPECT_FALSE(stale.is_fresh(std::chrono::seconds(300)));
}

TEST_F(MqttWorkspaceIntegrationTest, TopicAclWorkerCanPublishTasks) {
    MqttTopicAuth acl{swarm_id};
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/integration-swarm/tasks/new"));
}

TEST_F(MqttWorkspaceIntegrationTest, TopicAclWorkerCannotPublishAdmin) {
    MqttTopicAuth acl{swarm_id};
    EXPECT_FALSE(acl.can_publish(Role::Worker, "mcp-collab/integration-swarm/admin/roles"));
}

TEST_F(MqttWorkspaceIntegrationTest, TopicAclObserverCannotPublish) {
    MqttTopicAuth acl{swarm_id};
    EXPECT_FALSE(acl.can_publish(Role::Observer, "mcp-collab/integration-swarm/tasks/new"));
}

TEST_F(MqttWorkspaceIntegrationTest, TopicAclAllCanSubscribe) {
    MqttTopicAuth acl{swarm_id};
    EXPECT_TRUE(acl.can_subscribe(Role::Observer, "mcp-collab/integration-swarm/tasks/#"));
    EXPECT_TRUE(acl.can_subscribe(Role::Worker, "mcp-collab/integration-swarm/events/#"));
}

TEST_F(MqttWorkspaceIntegrationTest, TopicAclCustomRule) {
    MqttTopicAuth acl{swarm_id};
    acl.add_rule({.topic_prefix = "mcp-collab/integration-swarm/custom",
                  .allow_roles = {Role::Worker}, .allow_subscribe = true});
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/integration-swarm/custom/1"));
    EXPECT_FALSE(acl.can_publish(Role::Observer, "mcp-collab/integration-swarm/custom/1"));
}

TEST_F(MqttWorkspaceIntegrationTest, EnvelopeSwarmIsolation) {
    json payload = {{"type", "event"}};
    auto envelope = MqttEnvelope::sign("agent-1", "swarm-a", Role::Worker, payload, secret);
    auto j = envelope.to_json();

    // Should verify with same swarm's secret
    auto verified = MqttEnvelope::verify(j.dump(), secret);
    EXPECT_TRUE(verified.has_value());
    EXPECT_EQ(verified->swarm_id, "swarm-a");
}

TEST_F(MqttWorkspaceIntegrationTest, FullWorkspaceEventFlow) {
    TaskManager tasks;
    EventBus events;
    ContextStore context;

    // Set up event listener
    std::vector<std::string> event_log;
    events.subscribe("*", [&](const Event& e) {
        event_log.push_back(e.type + ":" + e.source);
    });

    // Create and progress a task
    auto t = tasks.create_task("Build", "architect");
    events.emit("task.created", "test", t.to_json());

    tasks.set_status(t.id, TaskStatus::InProgress);
    events.emit("task.started", "test", t.to_json());

    context.set("build.config", {{"threads", 8}}, "architect");
    events.emit("context.set", "test", {{"key", "build.config"}});

    tasks.set_status(t.id, TaskStatus::Completed);
    events.emit("task.completed", "test", t.to_json());

    EXPECT_EQ(event_log.size(), 4u);
    EXPECT_EQ(event_log[0], "task.created:test");
    EXPECT_EQ(event_log[3], "task.completed:test");
}

TEST_F(MqttWorkspaceIntegrationTest, CrossComponentTaskToContextToEvent) {
    TaskManager tasks;
    ContextStore context;
    EventBus events;

    // Track context changes
    std::string last_context_key;
    context.on_change([&](const std::string& key, const ContextEntry& entry, const std::string& action) {
        last_context_key = key;
        events.emit("context." + action, "context-store", {{"key", key}});
    });

    // Create task, set context
    auto t = tasks.create_task("Design API", "lead-dev");
    context.set("api.spec", {{"version", "2.0"}}, "lead-dev");
    EXPECT_EQ(last_context_key, "api.spec");

    auto recent = events.recent_events(10);
    EXPECT_GE(recent.size(), 1u);
}