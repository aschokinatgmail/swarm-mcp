#include "mcp_collab/channel.hpp"
#include <spdlog/spdlog.h>
#include <format>

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

Channel::Channel(SecureMqttClient& mqtt, const ChannelSpec& spec)
    : mqtt_(mqtt), spec_(spec) {
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
    // Fallback for internal server messages where no token is available
    // Uses a synthetic coordinator token
    static AuthToken internal_token;
    internal_token.token_id = "swarm-mcp-internal";
    internal_token.agent_id = "swarm-mcp-server";
    internal_token.role = Role::Coordinator;
    internal_token.swarm_id = "internal";
    internal_token.expires_at = std::chrono::system_clock::now() + std::chrono::hours(24);
    return mqtt_.publish_signed(full_topic_, data, internal_token, spec_.qos, spec_.retained);
}

void Channel::on_message(std::function<void(const MqttEnvelope&)> cb) {
    message_cb_ = std::move(cb);
}

std::string Channel::topic() const { return full_topic_; }
ChannelSpec Channel::spec() const { return spec_; }

ChannelManager::ChannelManager(SecureMqttClient& mqtt, const std::string& ns)
    : mqtt_(mqtt), namespace_(ns) {
    create(ChannelType::TaskUpdates);
    create(ChannelType::AgentPresence);
    create(ChannelType::Events);
    create(ChannelType::ContextSync);
    create(ChannelType::GitCoordination);
    create(ChannelType::Broadcast);
}

Channel& ChannelManager::get(ChannelType type, const std::string& name) {
    auto k = key(type, name);
    auto it = channels_.find(k);
    if (it == channels_.end()) {
        return create(type, name);
    }
    return *it->second;
}

Channel& ChannelManager::create(ChannelType type, const std::string& name, int qos) {
    auto k = key(type, name);
    auto spec = ChannelSpec{.type = type, .namespace_ = namespace_, .name = name, .qos = qos};
    channels_[k] = std::make_unique<Channel>(mqtt_, spec);
    return *channels_[k];
}

void ChannelManager::remove(ChannelType type, const std::string& name) {
    auto k = key(type, name);
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