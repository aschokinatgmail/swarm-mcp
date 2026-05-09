#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <chrono>
#include <vector>
#include <optional>
#include <atomic>

#include <nlohmann/json.hpp>
#include <MQTTAsync.h>

#include "mcp_collab/config.hpp"

namespace mcp_collab {

using json = nlohmann::json;

struct MqttMessage {
    std::string topic;
    std::string payload;
    int qos{1};
    bool retained{false};
    std::chrono::steady_clock::time_point received_at{std::chrono::steady_clock::now()};
};

using MqttCallback = std::function<void(const MqttMessage&)>;

class MqttClient {
public:
    explicit MqttClient(const MqttConfig& cfg);
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    bool connect();
    void disconnect();
    bool is_connected() const;

    bool publish(const std::string& topic, const std::string& payload, int qos = 1, bool retained = false);
    bool publish_json(const std::string& topic, const json& data, int qos = 1, bool retained = false);

    bool subscribe(const std::string& topic, int qos, MqttCallback callback);
    bool unsubscribe(const std::string& topic);

    std::vector<std::string> subscribed_topics() const;

    void set_on_connect(std::function<void()> cb);
    void set_on_disconnect(std::function<void()> cb);
    void set_on_message_lost(std::function<void(const std::string&)> cb);

private:
    static int on_message_arrived(void* context, char* topic_name, int topic_len, MQTTAsync_message* message);
    static void on_connection_lost(void* context, char* cause);
    static void on_connect_success(void* context, MQTTAsync_successData* response);
    static void on_connect_failure(void* context, MQTTAsync_failureData* response);
    static void on_disconnect_success(void* context, MQTTAsync_successData* response);
    static void on_subscribe_success(void* context, MQTTAsync_successData* response);
    static void on_subscribe_failure(void* context, MQTTAsync_failureData* response);

    void handle_message(const std::string& topic, const std::string& payload);
    void handle_reconnect();

public:
    // Test-only: entry point for message dispatch without broker
    void inject_message(const std::string& topic, const std::string& payload) {
        handle_message(topic, payload);
    }

    MqttConfig config_;
    MQTTAsync client_{nullptr};
    mutable std::shared_mutex topics_mutex_;
    mutable std::mutex pending_mutex_;
    std::unordered_map<std::string, MqttCallback> callbacks_;
    std::atomic<bool> connected_{false};
    std::function<void()> on_connect_cb_;
    std::function<void()> on_disconnect_cb_;
    std::function<void(const std::string&)> on_message_lost_cb_;
    int reconnect_delay_ms_{1000};
    static constexpr int max_reconnect_delay_ms_{30000};
};

}