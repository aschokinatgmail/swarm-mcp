#include <gtest/gtest.h>
#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/auth.hpp"

using namespace mcp_collab;

class MqttEnvelopeTest : public ::testing::Test {
protected:
    std::string secret = "swarm-hmac-secret-123";
};

TEST_F(MqttEnvelopeTest, SignAndVerify) {
    auto envelope = MqttEnvelope::sign("agent-1", "swarm-1", Role::Worker, {{"key", "value"}}, secret);
    auto json_str = envelope.to_json().dump();
    auto verified = MqttEnvelope::verify(json_str, secret);
    EXPECT_TRUE(verified.has_value());
    EXPECT_EQ(verified->sender, "agent-1");
    EXPECT_EQ(verified->swarm_id, "swarm-1");
    EXPECT_EQ(verified->role, "worker");
    EXPECT_EQ(verified->payload["key"], "value");
}

TEST_F(MqttEnvelopeTest, VerifyWrongSecret) {
    auto envelope = MqttEnvelope::sign("agent-1", "swarm-1", Role::Worker, {{"x", 1}}, secret);
    auto json_str = envelope.to_json().dump();
    auto verified = MqttEnvelope::verify(json_str, "wrong-secret");
    EXPECT_FALSE(verified.has_value());
}

TEST_F(MqttEnvelopeTest, VerifyTamperedPayload) {
    auto envelope = MqttEnvelope::sign("agent-1", "swarm-1", Role::Worker, {{"x", 1}}, secret);
    auto j = envelope.to_json();
    j["payload"]["x"] = 999;
    auto verified = MqttEnvelope::verify(j.dump(), secret);
    EXPECT_FALSE(verified.has_value());
}

TEST_F(MqttEnvelopeTest, VerifyTamperedSender) {
    auto envelope = MqttEnvelope::sign("agent-1", "swarm-1", Role::Worker, {{"x", 1}}, secret);
    auto j = envelope.to_json();
    j["sender"] = "agent-hacked";
    auto verified = MqttEnvelope::verify(j.dump(), secret);
    EXPECT_FALSE(verified.has_value());
}

TEST_F(MqttEnvelopeTest, VerifyInvalidJson) {
    auto verified = MqttEnvelope::verify("not json", secret);
    EXPECT_FALSE(verified.has_value());
}

TEST_F(MqttEnvelopeTest, VerifyWrongEnvelopeVersion) {
    json j = {
        {"envelope", "wrong-version"},
        {"sender", "a"}, {"swarm", "s"}, {"role", "worker"},
        {"timestamp", 0}, {"payload", {}}, {"signature", "x"}
    };
    auto verified = MqttEnvelope::verify(j.dump(), secret);
    EXPECT_FALSE(verified.has_value());
}

TEST_F(MqttEnvelopeTest, Freshness) {
    auto envelope = MqttEnvelope::sign("a", "s", Role::Worker, {}, secret);
    EXPECT_TRUE(envelope.is_fresh(std::chrono::seconds(60)));
    EXPECT_TRUE(envelope.is_fresh(std::chrono::hours(1)));
}

TEST_F(MqttEnvelopeTest, StalenessDetection) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 600000;
    EXPECT_FALSE(env.is_fresh(std::chrono::seconds(300)));
}

// ── Issue #82: default max_message_age is 60s ──────────────────────────

TEST_F(MqttEnvelopeTest, DefaultMaxAge60sRejects61sOld) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 61000;
    // Default argument is 60s; a 61s-old message should be stale.
    EXPECT_FALSE(env.is_fresh());
}

TEST_F(MqttEnvelopeTest, DefaultMaxAge60sAccepts30sOld) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 30000;
    // 30s old is within the 60s default window.
    EXPECT_TRUE(env.is_fresh());
}

TEST_F(MqttEnvelopeTest, DefaultMaxAge60sRejects300sOld) {
    // Regression: previously the default was 300s, so a 300s-old message was
    // accepted. With the new 60s default it must be rejected.
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 300000;
    EXPECT_FALSE(env.is_fresh());
}

// ── MqttTopicAuth tests ─────────────────────────────────────────────

class MqttTopicAuthTest : public ::testing::Test {
protected:
    MqttTopicAuth acl{"test-swarm"};
};

TEST_F(MqttTopicAuthTest, CoordinatorCanPublishAnything) {
    EXPECT_TRUE(acl.can_publish(Role::Coordinator, "mcp-collab/test-swarm/tasks/123"));
    EXPECT_TRUE(acl.can_publish(Role::Coordinator, "mcp-collab/test-swarm/admin/secret"));
}

TEST_F(MqttTopicAuthTest, WorkerCanPublishTasks) {
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/test-swarm/tasks/123"));
}

TEST_F(MqttTopicAuthTest, WorkerCanPublishEvents) {
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/test-swarm/events/build.done"));
}

TEST_F(MqttTopicAuthTest, WorkerCannotPublishAdmin) {
    EXPECT_FALSE(acl.can_publish(Role::Worker, "mcp-collab/test-swarm/admin/roles"));
}

TEST_F(MqttTopicAuthTest, ObserverCannotPublishTasks) {
    EXPECT_FALSE(acl.can_publish(Role::Observer, "mcp-collab/test-swarm/tasks/new"));
}

 TEST_F(MqttTopicAuthTest, AllRolesCanSubscribe) {
    EXPECT_TRUE(acl.can_subscribe(Role::Observer, "mcp-collab/test-swarm/tasks/#"));
    EXPECT_TRUE(acl.can_subscribe(Role::Worker, "mcp-collab/test-swarm/events/#"));
    EXPECT_TRUE(acl.can_subscribe(Role::Coordinator, "mcp-collab/test-swarm/admin/#"));
}

TEST_F(MqttTopicAuthTest, UnknownTopicDenied) {
    EXPECT_FALSE(acl.can_publish(Role::Worker, "unknown-prefix/topics"));
}

TEST_F(MqttTopicAuthTest, AddCustomRule) {
    acl.add_rule({.topic_prefix = "mcp-collab/test-swarm/custom",
                  .allow_roles = {Role::Worker}, .allow_subscribe = true});
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/test-swarm/custom/1"));
    EXPECT_FALSE(acl.can_publish(Role::Observer, "mcp-collab/test-swarm/custom/1"));
}

// ── Verified-callback wildcard dispatch tests ──────────────────────────
// Regression: + single-level wildcard matching in SecureMqttClient's
// verified-callback dispatch (secure_mqtt.cpp topic_matches).

class VerifiedWildcardTest : public ::testing::Test {
protected:
    std::string secret = "wildcard-test-secret";
    std::unique_ptr<SecureMqttClient> client;

    void SetUp() override {
        client = std::make_unique<SecureMqttClient>(MqttConfig{}, "default", secret);
    }

    // Subscribe with a pattern, inject a signed envelope on the given topic,
    // return whether the verified callback was invoked.
    bool dispatch_invoked(const std::string& pattern, const std::string& topic) {
        bool invoked = false;
        client->subscribe_verified(pattern, 1, [&](const MqttEnvelope&) {
            invoked = true;
        });
        auto envelope = MqttEnvelope::sign("agent-1", "default", Role::Worker,
                                            {{"x", 1}}, secret);
        client->raw_client().inject_message(topic, envelope.to_json().dump());
        return invoked;
    }
};

TEST_F(VerifiedWildcardTest, PlusMatchesSingleLevel) {
    EXPECT_TRUE(dispatch_invoked("mcp-collab/+/events", "mcp-collab/swarm-1/events"));
}

TEST_F(VerifiedWildcardTest, PlusRejectsTooManyLevels) {
    EXPECT_FALSE(dispatch_invoked("mcp-collab/+/events", "mcp-collab/swarm-1/sub/events"));
}

TEST_F(VerifiedWildcardTest, PlusRejectsMissingLevel) {
    EXPECT_FALSE(dispatch_invoked("mcp-collab/+/events", "mcp-collab/events"));
}

TEST_F(VerifiedWildcardTest, PlusAtStart) {
    EXPECT_TRUE(dispatch_invoked("+/events", "swarm-1/events"));
}

TEST_F(VerifiedWildcardTest, PlusAtEnd) {
    EXPECT_TRUE(dispatch_invoked("mcp-collab/+", "mcp-collab/swarm-1"));
}

TEST_F(VerifiedWildcardTest, PlusAndHashCombined) {
    EXPECT_TRUE(dispatch_invoked("mcp-collab/+/events/#", "mcp-collab/swarm-1/events/foo"));
}

// ── Issue #42: "default" swarm is not exempt from isolation ─────────────

class SwarmIsolationTest : public ::testing::Test {
protected:
    std::string secret = "isolation-test-secret";
};

TEST_F(SwarmIsolationTest, DefaultSwarmRejectedByNonDefaultClient) {
    // A client configured for "my-swarm" must reject messages from "default".
    SecureMqttClient client(MqttConfig{}, "my-swarm", secret);
    bool invoked = false;
    client.subscribe_verified("mcp-collab/my-swarm/tasks", 1,
                              [&](const MqttEnvelope&) { invoked = true; });
    auto envelope = MqttEnvelope::sign("agent-1", "default", Role::Worker,
                                       {{"x", 1}}, secret);
    client.raw_client().inject_message("mcp-collab/my-swarm/tasks",
                                       envelope.to_json().dump());
    EXPECT_FALSE(invoked);
}

TEST_F(SwarmIsolationTest, NonDefaultSwarmRejectedByDefaultClient) {
    // A client configured for "default" must reject messages from "my-swarm".
    SecureMqttClient client(MqttConfig{}, "default", secret);
    bool invoked = false;
    client.subscribe_verified("mcp-collab/default/tasks", 1,
                              [&](const MqttEnvelope&) { invoked = true; });
    auto envelope = MqttEnvelope::sign("agent-1", "my-swarm", Role::Worker,
                                       {{"x", 1}}, secret);
    client.raw_client().inject_message("mcp-collab/default/tasks",
                                       envelope.to_json().dump());
    EXPECT_FALSE(invoked);
}

TEST_F(SwarmIsolationTest, SameSwarmAccepted) {
    SecureMqttClient client(MqttConfig{}, "my-swarm", secret);
    bool invoked = false;
    client.subscribe_verified("mcp-collab/my-swarm/tasks", 1,
                              [&](const MqttEnvelope&) { invoked = true; });
    auto envelope = MqttEnvelope::sign("agent-1", "my-swarm", Role::Worker,
                                       {{"x", 1}}, secret);
    client.raw_client().inject_message("mcp-collab/my-swarm/tasks",
                                       envelope.to_json().dump());
    EXPECT_TRUE(invoked);
}

TEST_F(SwarmIsolationTest, DefaultToDefaultAccepted) {
    SecureMqttClient client(MqttConfig{}, "default", secret);
    bool invoked = false;
    client.subscribe_verified("mcp-collab/default/tasks", 1,
                              [&](const MqttEnvelope&) { invoked = true; });
    auto envelope = MqttEnvelope::sign("agent-1", "default", Role::Worker,
                                       {{"x", 1}}, secret);
    client.raw_client().inject_message("mcp-collab/default/tasks",
                                       envelope.to_json().dump());
    EXPECT_TRUE(invoked);
}

// ── Issue #82: SecureMqttClient default max_message_age is 60s ───────────

TEST(SecureMqttDefaultAgeTest, DefaultAgeRejects300sOldMessage) {
    std::string sec = "age-test-secret";
    SecureMqttClient client(MqttConfig{}, "swarm-1", sec);
    bool invoked = false;
    client.subscribe_verified("mcp-collab/swarm-1/tasks", 1,
                              [&](const MqttEnvelope&) { invoked = true; });

    // Build an envelope with a 300s-old timestamp — should be rejected by
    // the 60s default (previously it would have been accepted).
    MqttEnvelope env;
    env.envelope_version = "swarm-mcp/v1";
    env.sender = "agent-1";
    env.swarm_id = "swarm-1";
    env.role = "worker";
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 300000;
    env.payload = {{"x", 1}};
    std::string signing_input = std::format("{}:{}:{}:{}", env.sender, env.swarm_id,
                                            env.timestamp, env.payload.dump());
    env.signature = compute_hmac(signing_input, sec);

    client.raw_client().inject_message("mcp-collab/swarm-1/tasks",
                                       env.to_json().dump());
    EXPECT_FALSE(invoked);
}