#include "mcp_collab/channel.hpp"
#include <spdlog/spdlog.h>
#include <format>
#include <shared_mutex>

namespace mcp_collab {

std::string Channel::type_prefix(ChannelType type) {
    switch (type) {
        case ChannelType::TaskUpdates:     return "tasks";
        case ChannelType::AgentPresence:   return "agents";
        case ChannelType::Events:          return "events";
        case ChannelType::ContextSync:     return "context";
        case ChannelType::GitCoordination: return "git";
        case ChannelType::Broadcast:       return "broadcast";
        case ChannelType::Custom:          return "custom";
    }
    return "unknown";
}

Channel::Channel(SecureMqttClient& mqtt, const ChannelSpec& spec, const std::string& swarm_id)
    : mqtt_(mqtt), spec_(spec), swarm_id_(swarm_id) {
    full_topic_ = std::format("mcp-collab/{}/{}{}",
        spec_.namespace_.empty() ? "default" : spec_.namespace_,
        type_prefix(spec_.type),
        spec_.name.empty() ? "" : std::format("/{}", spec_.name));

    mqtt_.subscribe_verified(full_topic_ + "/#", spec_.qos,
        [this](const MqttEnvelope& env) {
            if (message_cb_) message_cb_(env);
        });
}

bool Channel::publish(const json& data, const AuthToken& token) {
    return mqtt_.publish_signed(full_topic_, data, token, spec_.qos, spec_.retained);
}

bool Channel::publish_unsigned(const json& data) {
    thread_local AuthToken internal_token = [] {
        AuthToken t;
        t.token_id = "swarm-mcp-internal";
        t.agent_id = "swarm-mcp-server";
        t.role = Role::Coordinator;
        t.expires_at = std::chrono::system_clock::now() + std::chrono::hours(87600);
        return t;
    }();
    AuthToken token = internal_token;
    token.swarm_id = swarm_id_;
    return mqtt_.publish_signed(full_topic_, data, token, spec_.qos, spec_.retained);
}

void Channel::on_message(std::function<void(const MqttEnvelope&)> cb) {
    message_cb_ = std::move(cb);
}

std::string Channel::topic() const { return full_topic_; }
ChannelSpec Channel::spec() const { return spec_; }

ChannelManager::ChannelManager(SecureMqttClient& mqtt, const std::string& ns,
                                 const std::string& swarm_id)
    : mqtt_(mqtt), namespace_(ns), swarm_id_(swarm_id) {
    create(ChannelType::TaskUpdates);
    create(ChannelType::AgentPresence);
    create(ChannelType::Events);
    create(ChannelType::ContextSync);
    create(ChannelType::GitCoordination);
    create(ChannelType::Broadcast);
}

Channel& ChannelManager::get(ChannelType type, const std::string& name) {
    auto k = key(type, name);
    std::unique_lock lock(channels_mutex_);
    auto it = channels_.find(k);
    if (it == channels_.end()) {
        auto spec = ChannelSpec{.type = type, .namespace_ = namespace_, .name = name, .qos = 1};
        auto [inserted, ok] = channels_.emplace(k, std::make_unique<Channel>(mqtt_, spec, swarm_id_));
        return *inserted->second;
    }
    return *it->second;
}

Channel& ChannelManager::create(ChannelType type, const std::string& name, int qos) {
    auto k = key(type, name);
    std::unique_lock lock(channels_mutex_);
    auto spec = ChannelSpec{.type = type, .namespace_ = namespace_, .name = name, .qos = qos};
    channels_[k] = std::make_unique<Channel>(mqtt_, spec, swarm_id_);
    return *channels_[k];
}

void ChannelManager::remove(ChannelType type, const std::string& name) {
    auto k = key(type, name);
    std::unique_lock lock(channels_mutex_);
    channels_.erase(k);
}

void ChannelManager::broadcast(const json& message, const AuthToken& token) {
    auto& ch = get(ChannelType::Broadcast);
    ch.publish(message, token);
}

void ChannelManager::broadcast_event(const std::string& event_type, const json& data, const AuthToken& token) {
    json msg = {
        {"type", event_type},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
        {"data", data}
    };
    broadcast(msg, token);
}

std::string ChannelManager::key(ChannelType type, const std::string& name) const {
    return std::format("{}:{}", static_cast<int>(type), name);
}

}