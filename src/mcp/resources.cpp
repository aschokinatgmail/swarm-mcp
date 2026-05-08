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
        .uri = "swarm://agents/capabilities",
        .name = "Agent Capabilities Matrix",
        .description = "Capability and model/environment introspection for all agents, enabling task assignment optimization",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        auto all = agents.list_agents();
        json result = json::array();
        for (const auto& a : all) {
            json entry;
            entry["id"] = a.id;
            entry["name"] = a.name;
            entry["role"] = role_to_str(a.role);
            entry["status"] = a.status == AgentStatus::Online ? "online" :
                              a.status == AgentStatus::Busy ? "busy" :
                              a.status == AgentStatus::Idle ? "idle" : "offline";
            entry["capabilities"] = a.capabilities;
            entry["model"] = a.model.to_json();
            entry["environment"] = a.environment.to_json();

            json effective_tools = json::array();
            for (const auto& [name, def] : proto.tool_definitions()) {
                if (has_permission(a.role, def.required_permission)) {
                    effective_tools.push_back(name);
                }
            }
            entry["effective_tools"] = effective_tools;
            result.push_back(entry);
        }
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

    proto.register_resource({
        .uri = "swarm://capabilities",
        .name = "Server Capabilities by Role",
        .description = "Maps each role to the tools, resources, and permissions available on this server",
        .mime_type = "application/json",
    }, [&](const json&) -> json {
        json roles = json::array();
        for (auto role : {Role::Coordinator, Role::Worker, Role::Observer}) {
            json role_entry;
            role_entry["role"] = role_to_str(role);
            role_entry["permissions"] = role_permissions(role);

            json tools_list = json::array();
            for (const auto& [name, def] : proto.tool_definitions()) {
                if (has_permission(role, def.required_permission)) {
                    json tool_info = {{"name", name}, {"description", def.description}};
                    tools_list.push_back(tool_info);
                }
            }
            role_entry["tools"] = tools_list;

            json resources_list = json::array();
            for (const auto& [uri, def] : proto.resource_definitions()) {
                if (has_permission(role, def.required_permission)) {
                    resources_list.push_back(uri);
                }
            }
            role_entry["resources"] = resources_list;

            roles.push_back(role_entry);
        }
        return roles;
    });
}

}