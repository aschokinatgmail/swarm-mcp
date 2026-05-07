#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <unordered_map>

#include "mcp_collab/secure_mqtt.hpp"

namespace mcp_collab {

enum class ChannelType {
    TaskUpdates,
    AgentPresence,
    Events,
    ContextSync,
    GitCoordination,
    Broadcast,
    Custom
};

struct ChannelSpec {
    ChannelType type;
    std::string namespace_;
    std::string name;
    int qos{1};
    bool retained{false};
};

class Channel {
public:
    Channel(SecureMqttClient& mqtt, const ChannelSpec& spec);
    ~Channel() = default;

    bool publish(const json& data, const AuthToken& token);
    bool publish_unsigned(const json& data);
    void on_message(std::function<void(const MqttEnvelope&)> cb);

    std::string topic() const;
    ChannelSpec spec() const;

private:
    static std::string type_prefix(ChannelType type);

    SecureMqttClient& mqtt_;
    ChannelSpec spec_;
    std::string full_topic_;
    std::function<void(const MqttEnvelope&)> message_cb_;
};

class ChannelManager {
public:
    explicit ChannelManager(SecureMqttClient& mqtt, const std::string& ns = "mcp-collab");

    Channel& get(ChannelType type, const std::string& name = "");
    Channel& create(ChannelType type, const std::string& name = "", int qos = 1);
    void remove(ChannelType type, const std::string& name = "");

    void broadcast(const json& message, const AuthToken& token);
    void broadcast_event(const std::string& event_type, const json& data, const AuthToken& token);

private:
    std::string key(ChannelType type, const std::string& name) const;

    SecureMqttClient& mqtt_;
    std::string namespace_;
    std::unordered_map<std::string, std::unique_ptr<Channel>> channels_;
};

}