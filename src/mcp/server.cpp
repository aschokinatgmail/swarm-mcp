#include "mcp_collab/server.hpp"
#include "mcp_collab/collab_defs.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

SwarmServer::SwarmServer(const ServerConfig& cfg)
    : config_(cfg)
    , auth_(cfg.swarm.secret)
    , secure_mqtt_(cfg.mqtt, cfg.swarm.id, cfg.swarm.secret)
    , channels_(secure_mqtt_, cfg.swarm.id.empty() ? "default" : cfg.swarm.id, cfg.swarm.id.empty() ? "default" : cfg.swarm.id)
    , git_ops_(cfg.git.repo_path)
    , branch_mgr_(git_ops_, cfg.git.branch_prefix)
    , merge_coordinator_(git_ops_, branch_mgr_)
    , agent_registry_(cfg.swarm.id)
    , protocol_(ServerInfo{.name = cfg.server_name, .version = cfg.server_version})
    , http_transport_(protocol_, auth_, StreamableHttpConfig{
        .host = cfg.http.host,
        .port = cfg.http.port,
        .endpoint = cfg.http.endpoint,
        .cors_origin = cfg.http.cors_origin,
        .thread_pool_size = cfg.http.thread_pool_size,
        .require_auth = cfg.http.require_auth,
    }) {
    register_collab_tools(protocol_, task_manager_, agent_registry_,
        context_store_, event_bus_, branch_mgr_, merge_coordinator_,
        secure_mqtt_.raw_client(), channels_);
    register_collab_resources(protocol_, task_manager_, agent_registry_, context_store_, event_bus_);
    register_collab_prompts(protocol_);
}

SwarmServer::~SwarmServer() {
    stop();
}

bool SwarmServer::start() {
    if (running_.load()) return true;

    spdlog::info("Starting Swarm MCP server: {} v{} (swarm={})",
        config_.server_name, config_.server_version,
        config_.swarm.id.empty() ? "default" : config_.swarm.id);

    if (!secure_mqtt_.connect()) {
        spdlog::warn("MQTT not available, running in standalone mode");
    } else {
        setup_mqtt_bridges();
    }

    task_manager_.on_task_event([this](const std::string& event, const Task& task) {
        event_bus_.emit(event, "task-manager", task.to_json());
    });

    agent_registry_.on_change([this](const std::string& event, const AgentInfo& agent) {
        event_bus_.emit(event, "agent-registry", agent.to_json());
    });

    context_store_.on_change([this](const std::string& key, const ContextEntry& entry, const std::string& action) {
        event_bus_.emit(std::format("context.{}", action), "context-store",
            {{"key", key}, {"entry", entry.to_json()}, {"action", action}});
    });

    running_.store(true);

    heartbeat_thread_ = std::thread([this]() { heartbeat_loop(); });
    prune_thread_ = std::thread([this]() { prune_loop(); });

    try {
        http_transport_.start();
    } catch (const std::exception& e) {
        spdlog::error("Failed to start HTTP transport: {}", e.what());
        running_.store(false);
        return false;
    }

    return true;
}

void SwarmServer::stop() {
    if (!running_.load()) return;
    running_.store(false);

    http_transport_.stop();
    secure_mqtt_.disconnect();

    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    if (prune_thread_.joinable()) prune_thread_.join();

    spdlog::info("Swarm MCP server stopped");
}

bool SwarmServer::is_running() const {
    return running_.load();
}

AuthToken SwarmServer::enroll_agent(const std::string& agent_id, Role role, const std::string& swarm_id) {
    std::string sid = swarm_id.empty() ? config_.swarm.id : swarm_id;
    if (sid.empty()) sid = "default";
    return auth_.issue_token(agent_id, role, sid, config_.swarm.token_ttl);
}

void SwarmServer::setup_mqtt_bridges() {
    AuthToken server_token;
    server_token.token_id = "swarm-mcp-server";
    server_token.agent_id = "swarm-mcp-server";
    server_token.role = Role::Coordinator;
    server_token.swarm_id = config_.swarm.id.empty() ? "default" : config_.swarm.id;
    server_token.expires_at = std::chrono::system_clock::now() + std::chrono::hours(87600);

    auto& task_channel = channels_.get(ChannelType::TaskUpdates);
    task_channel.on_message([this](const MqttEnvelope& env) {
        try {
            if (env.payload.contains("type")) {
                std::string type = env.payload["type"].get<std::string>();
                event_bus_.emit(type, "mqtt", env.payload.value("data", json::object()));
            }
        } catch (const json::parse_error& e) {
            spdlog::warn("Failed to process MQTT task envelope: {}", e.what());
        }
    });

    auto& event_channel = channels_.get(ChannelType::Events);
    event_channel.on_message([this](const MqttEnvelope& env) {
        try {
            event_bus_.emit(env.payload.value("type", "mqtt-event"), "mqtt", env.payload.value("data", json::object()));
        } catch (const json::parse_error& e) {
            spdlog::warn("Failed to process MQTT event envelope: {}", e.what());
        }
    });

    auto& agent_channel = channels_.get(ChannelType::AgentPresence);
    agent_channel.on_message([this](const MqttEnvelope& env) {
        try {
            if (env.payload.value("action", "") == "heartbeat") {
                agent_registry_.heartbeat(env.payload.value("agent_id", ""));
            }
        } catch (const json::parse_error& e) {
            spdlog::warn("Failed to process MQTT agent envelope: {}", e.what());
        }
    });
}

void SwarmServer::heartbeat_loop() {
    AuthToken server_token;
    server_token.token_id = "swarm-mcp-server";
    server_token.agent_id = "swarm-mcp-server";
    server_token.role = Role::Coordinator;
    server_token.swarm_id = config_.swarm.id.empty() ? "default" : config_.swarm.id;
    server_token.expires_at = std::chrono::system_clock::now() + std::chrono::hours(87600);

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(30));

        if (secure_mqtt_.is_connected()) {
            auto& channel = channels_.get(ChannelType::AgentPresence);
            channel.publish({
                {"action", "server_heartbeat"},
                {"server", config_.server_name},
                {"swarm", config_.swarm.id.empty() ? "default" : config_.swarm.id},
                {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()},
            }, server_token);
        }
    }
}

void SwarmServer::prune_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        auto pruned = agent_registry_.prune_stale(config_.swarm.heartbeat_timeout);
        if (pruned > 0) {
            spdlog::info("Pruned {} stale agents", pruned);
        }
    }
}

}