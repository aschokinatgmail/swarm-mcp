#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/auth.hpp"
#include <spdlog/spdlog.h>
#include <chrono>

namespace mcp_collab {

// ── MqttEnvelope ──────────────────────────────────────────────────────

json MqttEnvelope::to_json() const {
    json j = {
        {"envelope", envelope_version},
        {"sender", sender},
        {"swarm", swarm_id},
        {"role", role},
        {"timestamp", timestamp},
        {"payload", payload},
        {"signature", signature},
    };
    return j;
}

MqttEnvelope MqttEnvelope::sign(const std::string& sender_id, const std::string& swarm_id,
                                 Role role, const json& payload, const std::string& secret) {
    MqttEnvelope env;
    env.sender = sender_id;
    env.swarm_id = swarm_id;
    env.role = role_to_str(role);
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    env.payload = payload;

    // signature covers: sender:swarm:timestamp:payload_json
    std::string payload_str = payload.dump();
    std::string signing_input = std::format("{}:{}:{}:{}", sender_id, swarm_id, env.timestamp, payload_str);

    // Use AuthProvider's HMAC signing (same secret)
    // Inline HMAC computation
    env.signature = compute_hmac(signing_input, secret);
    return env;
}

std::optional<MqttEnvelope> MqttEnvelope::verify(const std::string& raw, const std::string& secret) {
    try {
        json j = json::parse(raw);
        if (j.value("envelope", "") != "swarm-mcp/v1") {
            spdlog::warn("MQTT envelope: unknown version '{}'", j.value("envelope", ""));
            return std::nullopt;
        }

        MqttEnvelope env;
        env.envelope_version = j["envelope"].get<std::string>();
        env.sender = j["sender"].get<std::string>();
        env.swarm_id = j["swarm"].get<std::string>();
        env.role = j["role"].get<std::string>();
        env.timestamp = j["timestamp"].get<int64_t>();
        env.payload = j["payload"];
        env.signature = j["signature"].get<std::string>();

        // Recompute expected signature
        std::string payload_str = env.payload.dump();
        std::string signing_input = std::format("{}:{}:{}:{}", env.sender, env.swarm_id, env.timestamp, payload_str);
        std::string expected = compute_hmac(signing_input, secret);

        if (expected != env.signature) {
            spdlog::warn("MQTT envelope: signature verification failed from sender={}", env.sender);
            return std::nullopt;
        }

        return env;

    } catch (const json::parse_error& e) {
        spdlog::warn("MQTT envelope: JSON parse error: {}", e.what());
        return std::nullopt;
    }
}

bool MqttEnvelope::is_fresh(std::chrono::seconds max_age) const {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto age_ms = now_ms - timestamp;
    constexpr int64_t clock_skew_threshold_ms = 30000;
    return age_ms >= -clock_skew_threshold_ms && age_ms < max_age.count() * 1000;
}

// ── MqttTopicAuth ────────────────────────────────────────────────────

MqttTopicAuth::MqttTopicAuth(const std::string& swarm_id)
    : swarm_prefix_(std::format("mcp-collab/{}/", swarm_id.empty() ? "default" : swarm_id)) {
    init_default_rules();
}

void MqttTopicAuth::init_default_rules() {
    // Coordinators can publish everywhere
    rules_.push_back({
        .topic_prefix = swarm_prefix_ + "tasks",
        .allow_roles = {Role::Coordinator, Role::Worker},
        .allow_subscribe = true,
    });
    rules_.push_back({
        .topic_prefix = swarm_prefix_ + "events",
        .allow_roles = {Role::Coordinator, Role::Worker},
        .allow_subscribe = true,
    });
    rules_.push_back({
        .topic_prefix = swarm_prefix_ + "context",
        .allow_roles = {Role::Coordinator, Role::Worker},
        .allow_subscribe = true,
    });
    rules_.push_back({
        .topic_prefix = swarm_prefix_ + "git",
        .allow_roles = {Role::Coordinator, Role::Worker},
        .allow_subscribe = true,
    });
    rules_.push_back({
        .topic_prefix = swarm_prefix_ + "agents",
        .allow_roles = {Role::Coordinator, Role::Worker},
        .allow_subscribe = true,
    });
    rules_.push_back({
        .topic_prefix = swarm_prefix_ + "broadcast",
        .allow_roles = {Role::Coordinator, Role::Worker},
        .allow_subscribe = true,
    });
    // Coordinators-only topics (e.g. role changes, secret rotation)
    rules_.push_back({
        .topic_prefix = swarm_prefix_ + "admin",
        .allow_roles = {Role::Coordinator},
        .allow_subscribe = true,
    });
}

bool MqttTopicAuth::can_publish(Role role, const std::string& topic) const {
    // Coordinators can always publish
    if (role == Role::Coordinator) return true;

    for (const auto& rule : rules_) {
        if (topic.starts_with(rule.topic_prefix) || topic == rule.topic_prefix) {
            return rule.allow_roles.contains(role);
        }
    }

    // Unknown topic prefix — deny by default
    spdlog::debug("MQTT ACL: no matching rule for publish on topic={}", topic);
    return false;
}

bool MqttTopicAuth::can_subscribe(Role role, const std::string& topic) const {
    // Coordinators can always subscribe
    if (role == Role::Coordinator) return true;

    for (const auto& rule : rules_) {
        if (topic.starts_with(rule.topic_prefix) || topic == rule.topic_prefix) {
            // If subscribe_roles is set, check it; otherwise allow all
            if (!rule.subscribe_roles.empty()) {
                return rule.subscribe_roles.contains(role);
            }
            return rule.allow_subscribe;
        }
    }

    // Wildcard subscriptions on swarm prefix
    if (topic.starts_with(swarm_prefix_ + "#") || topic == swarm_prefix_ + "#") {
        return true;
    }

    return false;
}

void MqttTopicAuth::add_rule(MqttAclRule rule) {
    rules_.push_back(std::move(rule));
}

// ── SecureMqttClient ─────────────────────────────────────────────────

SecureMqttClient::SecureMqttClient(const MqttConfig& mqtt_cfg, const std::string& swarm_id,
                                     const std::string& swarm_secret)
    : mqtt_(mqtt_cfg)
    , topic_auth_(swarm_id)
    , swarm_id_(swarm_id.empty() ? "default" : swarm_id)
    , swarm_secret_(swarm_secret) {
}

bool SecureMqttClient::connect() {
    return mqtt_.connect();
}

void SecureMqttClient::disconnect() {
    mqtt_.disconnect();
}

bool SecureMqttClient::is_connected() const {
    return mqtt_.is_connected();
}

bool SecureMqttClient::publish_signed(const std::string& topic, const json& payload,
                                       const AuthToken& token, int qos, bool retained) {
    // ACL check
    if (!topic_auth_.can_publish(token.role, topic)) {
        spdlog::warn("MQTT publish denied by ACL: agent={} role={} topic={}",
            token.agent_id, role_to_str(token.role), topic);
        return false;
    }

    // Create signed envelope
    auto envelope = MqttEnvelope::sign(token.agent_id, swarm_id_, token.role, payload, swarm_secret_);
    json envelope_json = envelope.to_json();

    return mqtt_.publish_json(topic, envelope_json, qos, retained);
}

bool SecureMqttClient::subscribe_verified(const std::string& topic, int qos,
                                           std::function<void(const MqttEnvelope&)> callback) {
    {
        std::unique_lock lock(cb_mutex_);
        verified_callbacks_[topic] = std::move(callback);
    }

    return mqtt_.subscribe(topic, qos, [this](const MqttMessage& msg) {
        on_raw_message(msg);
    });
}

bool SecureMqttClient::unsubscribe(const std::string& topic) {
    {
        std::unique_lock lock(cb_mutex_);
        verified_callbacks_.erase(topic);
    }
    return mqtt_.unsubscribe(topic);
}

void SecureMqttClient::on_raw_message(const MqttMessage& msg) {
    // Verify envelope
    auto envelope = MqttEnvelope::verify(msg.payload, swarm_secret_);
    if (!envelope) {
        // If secret is empty (dev mode), try parsing without verification
        if (swarm_secret_.empty()) {
            try {
                json j = json::parse(msg.payload);
                MqttEnvelope env;
                env.sender = j.value("sender", "");
                env.swarm_id = j.value("swarm", "");
                env.role = j.value("role", "worker");
                env.timestamp = j.value("timestamp", 0LL);
                env.payload = j.value("payload", json::object());
                envelope = env;
            } catch (const json::parse_error&) {
                spdlog::warn("MQTT: unparseable message on topic={}", msg.topic);
                return;
            }
        } else {
            spdlog::warn("MQTT: dropping unverified message on topic={}", msg.topic);
            return;
        }
    }

    // Replay protection
    if (!envelope->is_fresh(max_message_age_)) {
        spdlog::warn("MQTT: dropping stale message from sender={} age={}s",
            envelope->sender,
            (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() - envelope->timestamp) / 1000);
        return;
    }

    // Swarm isolation — reject messages from other swarms
    if (envelope->swarm_id != swarm_id_ && envelope->swarm_id != "default" && swarm_id_ != "default") {
        spdlog::warn("MQTT: dropping cross-swarm message from swarm={} (expected={})",
            envelope->swarm_id, swarm_id_);
        return;
    }

    spdlog::debug("MQTT verified message: topic={} sender={} role={}", msg.topic, envelope->sender, envelope->role);

    // Dispatch to registered callbacks
    std::shared_lock lock(cb_mutex_);
    for (const auto& [pattern, cb] : verified_callbacks_) {
        if (pattern == msg.topic || pattern == "#" ||
            (pattern.ends_with("/#") && msg.topic.starts_with(pattern.substr(0, pattern.size() - 2)))) {
            cb(*envelope);
            return;
        }
    }

    spdlog::debug("MQTT: no verified callback for topic={}", msg.topic);
}

}