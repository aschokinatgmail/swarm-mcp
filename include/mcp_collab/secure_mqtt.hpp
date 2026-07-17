#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <optional>
#include <chrono>
#include <mutex>
#include <shared_mutex>

#include <nlohmann/json.hpp>
#include "mcp_collab/mqtt_client.hpp"
#include "mcp_collab/auth.hpp"

namespace mcp_collab {

using json = nlohmann::json;

// ── Signed MQTT envelope ──────────────────────────────────────────────
// Every payload published to MQTT is wrapped in:
// {
//   "envelope": "swarm-mcp/v1",
//   "sender": "<agent_id>",
//   "swarm": "<swarm_id>",
//   "role": "<role>",
//   "timestamp": <epoch_ms>,
//   "payload": { ... actual data ... },
//   "signature": "<hmac_sha256_hex>"
// }
//
// signature = HMAC-SHA256(swarm_secret, sender:swarm:timestamp:payload_json)

struct MqttEnvelope {
    std::string envelope_version{"swarm-mcp/v1"};
    std::string sender;
    std::string swarm_id;
    std::string role;
    int64_t timestamp{0};
    json payload;
    std::string signature;

    json to_json() const;

    // Create a signed envelope
    static MqttEnvelope sign(const std::string& sender_id, const std::string& swarm_id,
                             Role role, const json& payload, const std::string& secret);

    // Verify and parse an envelope; returns nullopt if verification fails
    static std::optional<MqttEnvelope> verify(const std::string& raw, const std::string& secret);

    // Check staleness (reject messages older than max_age)
    bool is_fresh(std::chrono::seconds max_age = std::chrono::seconds(60)) const;
};

// ── Topic authorization ──────────────────────────────────────────────
// Maps agent roles to permitted MQTT topic prefixes:
//   Coordinator: can publish/subscribe to everything
//   Worker: can publish tasks, context, git, events; subscribe to all
//   Observer: can only subscribe (no publish)

struct MqttAclRule {
    std::string topic_prefix;                          // e.g. "mcp-collab/swarm1/tasks"
    std::unordered_set<Role> allow_roles;               // which roles can publish
    std::unordered_set<Role> subscribe_roles;           // which roles can subscribe (empty = all)
    bool allow_subscribe{true};                         // deprecated, use subscribe_roles
};

class MqttTopicAuth {
public:
    explicit MqttTopicAuth(const std::string& swarm_id);

    bool can_publish(Role role, const std::string& topic) const;
    bool can_subscribe(Role role, const std::string& topic) const;

    void add_rule(MqttAclRule rule);

private:
    std::string swarm_prefix_;
    std::vector<MqttAclRule> rules_;

    void init_default_rules();
};

// ── SecureMqttClient ──────────────────────────────────────────────────
// Wraps MqttClient with:
//   1. TLS-encrypted connections (configured in MqttConfig)
//   2. HMAC-SHA256 signed envelopes for all payloads
//   3. Verification of all incoming messages
//   4. Topic-level ACL enforcement per agent role
//   5. Replay protection via timestamp freshness checks

class SecureMqttClient {
public:
    SecureMqttClient(const MqttConfig& mqtt_cfg, const std::string& swarm_id,
                     const std::string& swarm_secret);

    bool connect();
    void disconnect();
    bool is_connected() const;

    // Publish a signed envelope (verifies role has permission for the topic)
    bool publish_signed(const std::string& topic, const json& payload,
                        const AuthToken& token, int qos = 1, bool retained = false);

    // Subscribe with automatic envelope verification
    bool subscribe_verified(const std::string& topic, int qos,
                            std::function<void(const MqttEnvelope&)> callback);

    bool unsubscribe(const std::string& topic);

    MqttClient& raw_client() { return mqtt_; }
    const MqttTopicAuth& acl() const { return topic_auth_; }

    void set_max_message_age(std::chrono::seconds age) { max_message_age_ = age; }

private:
    void on_raw_message(const MqttMessage& msg);

    MqttClient mqtt_;
    MqttTopicAuth topic_auth_;
    std::string swarm_id_;
    std::string swarm_secret_;
    std::chrono::seconds max_message_age_{60};

    mutable std::shared_mutex cb_mutex_;
    std::unordered_map<std::string, std::function<void(const MqttEnvelope&)>> verified_callbacks_;
};

}