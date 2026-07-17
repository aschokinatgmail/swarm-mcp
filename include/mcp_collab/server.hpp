#pragma once

#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <optional>

#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/channel.hpp"
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/git_operations.hpp"
#include "mcp_collab/branch_manager.hpp"
#include "mcp_collab/merge_coordinator.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
#include "mcp_collab/config.hpp"
#include "mcp_collab/persistence.hpp"

namespace mcp_collab {

class SwarmServer {
public:
    explicit SwarmServer(const ServerConfig& cfg = {});
    ~SwarmServer();

    bool start();
    void stop();
    bool is_running() const;

    AuthToken enroll_agent(const std::string& agent_id, Role role, const std::string& swarm_id);

    SecureMqttClient& mqtt() { return secure_mqtt_; }
    AuthProvider& auth() { return auth_; }

private:
    void setup_mqtt_bridges();
    void heartbeat_loop();
    void prune_loop();
    void load_persistence();
    void setup_auto_save();

    ServerConfig config_;
    AuthProvider auth_;
    SecureMqttClient secure_mqtt_;
    ChannelManager channels_;
    EventBus event_bus_;
    AgentRegistry agent_registry_;
    TaskManager task_manager_;
    ContextStore context_store_;
    GitOperations git_ops_;
    BranchManager branch_mgr_;
    MergeCoordinator merge_coordinator_;
    McpProtocol protocol_;
    StreamableHttpTransport http_transport_;

    std::optional<PersistenceLayer> persistence_;
    std::thread heartbeat_thread_;
    std::thread prune_thread_;
    std::atomic<bool> running_{false};
};

}