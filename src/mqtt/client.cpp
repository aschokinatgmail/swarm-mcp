#include "mcp_collab/mqtt_client.hpp"
#include "mcp_collab/uuid.hpp"
#include "mcp_collab/platform.hpp"

#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace mcp_collab {

MqttClient::MqttClient(const MqttConfig& cfg)
    : config_(cfg) {
    if (config_.client_id.empty()) {
        config_.client_id = std::format("mcp-collab-{}-{}", platform::hostname(), platform::pid());
    }

    std::string uri = std::format("{}://{}:{}",
        config_.use_tls ? "ssl" : "tcp",
        config_.host, config_.port);

    int rc = MQTTAsync_create(&client_, uri.c_str(), config_.client_id.c_str(),
        MQTTCLIENT_PERSISTENCE_NONE, nullptr);

    if (rc != MQTTASYNC_SUCCESS) {
        spdlog::error("MQTT create failed: rc={}", rc);
        throw std::runtime_error("Failed to create MQTT client");
    }

    MQTTAsync_setCallbacks(client_, this, on_connection_lost, on_message_arrived, nullptr);
}

MqttClient::~MqttClient() {
    disconnect();
    if (client_) {
        MQTTAsync_destroy(&client_);
    }
}

bool MqttClient::connect() {
    MQTTAsync_connectOptions opts = MQTTAsync_connectOptions_initializer;
    opts.keepAliveInterval = config_.keep_alive_sec;
    opts.cleansession = 1;
    opts.automaticReconnect = 1;
    opts.minRetryInterval = 1;
    opts.maxRetryInterval = 30;
    opts.onSuccess = on_connect_success;
    opts.onFailure = on_connect_failure;
    opts.context = this;

    MQTTAsync_SSLOptions sslOpts = MQTTAsync_SSLOptions_initializer;
    if (config_.use_tls) {
        sslOpts.trustStore = config_.ca_cert_path.c_str();
        opts.ssl = &sslOpts;
    }

    if (!config_.username.empty()) {
        opts.username = config_.username.c_str();
        opts.password = config_.password.c_str();
    }

    int rc = MQTTAsync_connect(client_, &opts);
    if (rc != MQTTASYNC_SUCCESS) {
        spdlog::error("MQTT connect failed: rc={}", rc);
        return false;
    }
    return true;
}

void MqttClient::disconnect() {
    if (!client_ || !connected_.load()) return;

    MQTTAsync_disconnectOptions opts = MQTTAsync_disconnectOptions_initializer;
    opts.onSuccess = on_disconnect_success;
    opts.context = this;
    MQTTAsync_disconnect(client_, &opts);
    connected_.store(false);
}

bool MqttClient::is_connected() const {
    return connected_.load();
}

bool MqttClient::publish(const std::string& topic, const std::string& payload, int qos, bool retained) {
    if (!connected_.load()) {
        spdlog::warn("MQTT publish while disconnected: topic={}", topic);
        return false;
    }

    MQTTAsync_message msg = MQTTAsync_message_initializer;
    msg.payload = const_cast<char*>(payload.data());
    msg.payloadlen = static_cast<int>(payload.size());
    msg.qos = qos;
    msg.retained = retained ? 1 : 0;

    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    opts.context = this;

    int rc = MQTTAsync_sendMessage(client_, topic.c_str(), &msg, &opts);
    if (rc != MQTTASYNC_SUCCESS) {
        spdlog::error("MQTT publish failed: topic={} rc={}", topic, rc);
        return false;
    }

    spdlog::debug("MQTT published: topic={} size={}", topic, payload.size());
    return true;
}

bool MqttClient::publish_json(const std::string& topic, const json& data, int qos, bool retained) {
    return publish(topic, data.dump(), qos, retained);
}

bool MqttClient::subscribe(const std::string& topic, int qos, MqttCallback callback) {
    {
        std::unique_lock lock(topics_mutex_);
        callbacks_[topic] = std::move(callback);
    }

    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    opts.onSuccess = on_subscribe_success;
    opts.onFailure = on_subscribe_failure;
    opts.context = this;

    int rc = MQTTAsync_subscribe(client_, topic.c_str(), qos, &opts);
    if (rc != MQTTASYNC_SUCCESS) {
        spdlog::error("MQTT subscribe failed: topic={} rc={}", topic, rc);
        return false;
    }

    spdlog::info("MQTT subscribed: topic={} qos={}", topic, qos);
    return true;
}

bool MqttClient::unsubscribe(const std::string& topic) {
    {
        std::unique_lock lock(topics_mutex_);
        callbacks_.erase(topic);
    }

    MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
    opts.context = this;

    int rc = MQTTAsync_unsubscribe(client_, topic.c_str(), &opts);
    if (rc != MQTTASYNC_SUCCESS) {
        spdlog::error("MQTT unsubscribe failed: topic={} rc={}", topic, rc);
        return false;
    }
    return true;
}

std::vector<std::string> MqttClient::subscribed_topics() const {
    std::shared_lock lock(topics_mutex_);
    std::vector<std::string> topics;
    topics.reserve(callbacks_.size());
    for (const auto& [k, _] : callbacks_) {
        topics.push_back(k);
    }
    return topics;
}

void MqttClient::set_on_connect(std::function<void()> cb) { on_connect_cb_ = std::move(cb); }
void MqttClient::set_on_disconnect(std::function<void()> cb) { on_disconnect_cb_ = std::move(cb); }
void MqttClient::set_on_message_lost(std::function<void(const std::string&)> cb) { on_message_lost_cb_ = std::move(cb); }

int MqttClient::on_message_arrived(void* context, char* topic_name, int topic_len, MQTTAsync_message* message) {
    auto* self = static_cast<MqttClient*>(context);
    std::string topic(topic_name, topic_len > 0 ? topic_len : static_cast<int>(std::strlen(topic_name)));
    std::string payload(static_cast<const char*>(message->payload), message->payloadlen);

    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topic_name);

    self->handle_message(topic, payload);
    return 1;
}

void MqttClient::on_connection_lost(void* context, char* cause) {
    auto* self = static_cast<MqttClient*>(context);
    self->connected_.store(false);
    spdlog::warn("MQTT connection lost: {}", cause ? cause : "unknown");

    if (self->on_disconnect_cb_) self->on_disconnect_cb_();
}

void MqttClient::on_connect_success(void* context, MQTTAsync_successData*) {
    auto* self = static_cast<MqttClient*>(context);
    self->connected_.store(true);
    self->reconnect_delay_ms_ = 1000;
    spdlog::info("MQTT connected: client_id={}", self->config_.client_id);

    std::shared_lock lock(self->topics_mutex_);
    for (const auto& [topic, cb] : self->callbacks_) {
        MQTTAsync_responseOptions opts = MQTTAsync_responseOptions_initializer;
        opts.context = self;
        MQTTAsync_subscribe(self->client_, topic.c_str(), 1, &opts);
    }

    if (self->on_connect_cb_) self->on_connect_cb_();
}

void MqttClient::on_connect_failure(void* context, MQTTAsync_failureData* response) {
    auto* self = static_cast<MqttClient*>(context);
    spdlog::error("MQTT connect failed: rc={} msg={}",
        response->code, response->message ? response->message : "");

    if (auto delay = std::min(self->reconnect_delay_ms_ * 2, max_reconnect_delay_ms_);
        self->on_connect_cb_) {
        spdlog::info("MQTT retrying in {}ms", delay);
    }
    self->reconnect_delay_ms_ = std::min(self->reconnect_delay_ms_ * 2, max_reconnect_delay_ms_);
}

void MqttClient::on_disconnect_success(void* context, MQTTAsync_successData*) {
    auto* self = static_cast<MqttClient*>(context);
    self->connected_.store(false);
    spdlog::info("MQTT disconnected");
}

void MqttClient::on_subscribe_success(void*, MQTTAsync_successData*) {
    spdlog::debug("MQTT subscribe acknowledged");
}

void MqttClient::on_subscribe_failure(void* context, MQTTAsync_failureData* response) {
    spdlog::error("MQTT subscribe acknowledged with failure: rc={}", response->code);
}

void MqttClient::handle_message(const std::string& topic, const std::string& payload) {
    std::shared_lock lock(topics_mutex_);

    auto mqtt_match = [](const std::string& pattern, const std::string& t) -> bool {
        if (pattern == t || pattern == "#") return true;

        // Split both by '/'
        auto split = [](const std::string& s) -> std::vector<std::string> {
            std::vector<std::string> parts;
            std::istringstream iss(s);
            std::string part;
            while (std::getline(iss, part, '/')) parts.push_back(part);
            return parts;
        };

        auto pat_parts = split(pattern);
        auto top_parts = split(t);

        size_t pi = 0, ti = 0;
        while (pi < pat_parts.size() && ti < top_parts.size()) {
            if (pat_parts[pi] == "#") return true; // # matches rest
            if (pat_parts[pi] == "+") {            // + matches one level
                pi++; ti++;
                continue;
            }
            if (pat_parts[pi] != top_parts[ti]) return false;
            pi++; ti++;
        }

        if (pi == pat_parts.size() && ti == top_parts.size()) return true;
        // Allow trailing /# to match end
        if (pi < pat_parts.size() && pat_parts[pi] == "#" && ti == top_parts.size()) return true;
        return false;
    };

    for (const auto& [pattern, cb] : callbacks_) {
        if (mqtt_match(pattern, topic)) {
            MqttMessage msg{.topic = topic, .payload = payload};
            cb(msg);
            return;
        }
    }

    spdlog::debug("MQTT no handler for topic: {}", topic);
}

}