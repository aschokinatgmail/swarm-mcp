#pragma once

#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/branch_manager.hpp"
#include "mcp_collab/merge_coordinator.hpp"
#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/channel.hpp"

namespace mcp_collab {

// Helper: extract AuthToken from tool args (injected by transport as _auth)
inline AuthToken auth_from_args(const json& args) {
    AuthToken token;
    if (args.contains("_auth")) {
        auto& a = args["_auth"];
        token.agent_id = a.value("agent_id", "");
        token.role = role_from_str(a.value("role", "observer"));
        token.swarm_id = a.value("swarm_id", "");
    } else {
        token.role = Role::Observer;
    }
    return token;
}

void register_collab_tools(McpProtocol& proto, TaskManager& tasks, AgentRegistry& agents,
                           ContextStore& context, EventBus& events, BranchManager& branches,
                           MergeCoordinator& merges, MqttClient& mqtt, ChannelManager& channels);

void register_collab_resources(McpProtocol& proto, TaskManager& tasks, AgentRegistry& agents,
                               ContextStore& context, EventBus& events);

void register_collab_prompts(McpProtocol& proto);

}