#include "mcp_collab/collab_defs.hpp"

namespace mcp_collab {

void register_collab_resources(McpProtocol& proto, TaskManager& tasks, AgentRegistry& agents,
                               ContextStore& context, EventBus& events) {

    proto.register_resource({
        .uri = "swarm://tasks",
        .name = "All Tasks",
        .description = "Current state of all collaboration tasks",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        auto all = tasks.list_tasks();
        json result = json::array();
        for (const auto& t : all) result.push_back(t.to_json());
        return result;
    });

    proto.register_resource({
        .uri = "swarm://tasks/ready",
        .name = "Ready Tasks",
        .description = "Tasks whose dependencies are complete and are ready to start",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        auto ready = tasks.get_ready_tasks();
        json result = json::array();
        for (const auto& t : ready) result.push_back(t.to_json());
        return result;
    });

    proto.register_resource({
        .uri = "swarm://agents",
        .name = "Registered Agents",
        .description = "All agents in the collaboration network",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        auto all = agents.list_agents();
        json result = json::array();
        for (const auto& a : all) result.push_back(a.to_json());
        return result;
    });

    proto.register_resource({
        .uri = "swarm://agents/idle",
        .name = "Idle Agents",
        .description = "Agents currently available for task assignment",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        auto idle = agents.find_idle();
        json result = json::array();
        for (const auto& a : idle) result.push_back(a.to_json());
        return result;
    });

    proto.register_resource({
        .uri = "swarm://context",
        .name = "Shared Context",
        .description = "Full shared context store snapshot",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        return context.snapshot();
    });

    proto.register_resource({
        .uri = "swarm://events/recent",
        .name = "Recent Events",
        .description = "Recent events from the collaboration event log",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        auto evts = events.recent_events(100);
        json result = json::array();
        for (const auto& e : evts) result.push_back(e.to_json());
        return result;
    });
}

}