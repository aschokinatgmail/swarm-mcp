#include "mcp_collab/collab_defs.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

void register_collab_tools(McpProtocol& proto, TaskManager& tasks, AgentRegistry& agents,
                           ContextStore& context, EventBus& events, BranchManager& branches,
                           MergeCoordinator& merges, MqttClient& mqtt, ChannelManager& channels) {

    // ── Task tools ───────────────────────────────────────────────────

    proto.register_tool({
        .name = "task_create",
        .description = "Create a new collaboration task that can be assigned to agents",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"title", {{"type", "string"}, {"description", "Task title"}}},
                {"description", {{"type", "string"}, {"description", "Task description"}}},
                {"creator", {{"type", "string"}, {"description", "Agent ID creating the task"}}},
                {"priority", {{"type", "string"}, {"enum", json::array({"low","medium","high","critical"})}, {"description", "Task priority"}}},
                {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Tags for categorization"}}},
                {"dependencies", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "IDs of tasks this depends on"}}},
            }},
            {"required", json::array({"title", "creator"})},
        },
        .required_permission = Permission::TaskCreate,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto title = args.value("title", "");
        auto creator = args.value("creator", "");
        auto desc = args.value("description", "");
        auto pri_str = args.value("priority", "medium");
        TaskPriority pri = task_priority_from_str(pri_str);
        auto task = tasks.create_task(title, creator, desc, pri);
        if (args.contains("tags")) {
            for (const auto& tag : args["tags"]) tasks.add_tag(task.id, tag.get<std::string>());
        }
        if (args.contains("dependencies")) {
            for (const auto& dep : args["dependencies"]) tasks.add_dependency(task.id, dep.get<std::string>());
        }
        channels.broadcast_event("task.created", task.to_json(), token);
        return task.to_json();
    });

    proto.register_tool({
        .name = "task_list",
        .description = "List tasks with optional filtering by status, assignee, or tags",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"status", {{"type", "string"}, {"enum", json::array({"pending","in_progress","blocked","completed","failed","cancelled"})}}},
                {"assignee", {{"type", "string"}}},
                {"tag", {{"type", "string"}}},
                {"search", {{"type", "string"}}},
            }},
        },
        .required_permission = Permission::TaskRead,
    }, [&](const json& args) -> json {
        TaskFilter filter;
        if (args.contains("status")) filter.status = task_status_from_str(args["status"].get<std::string>());
        if (args.contains("assignee")) filter.assignee = args["assignee"].get<std::string>();
        if (args.contains("tag")) filter.tag = args["tag"].get<std::string>();
        if (args.contains("search")) filter.search_term = args["search"].get<std::string>();
        auto task_list = tasks.list_tasks(filter);
        json result = json::array();
        for (const auto& t : task_list) result.push_back(t.to_json());
        return result;
    });

    proto.register_tool({
        .name = "task_assign",
        .description = "Assign a task to an agent",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"task_id", {{"type", "string"}}},
                {"agent_id", {{"type", "string"}}},
            }},
            {"required", json::array({"task_id", "agent_id"})},
        },
        .required_permission = Permission::TaskAssign,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto task_id = args.value("task_id", "");
        auto agent_id = args.value("agent_id", "");
        if (tasks.assign_task(task_id, agent_id)) {
            auto task = tasks.get_task(task_id);
            channels.broadcast_event("task.assigned", task->to_json(), token);
            return {{"success", true}, {"task", task->to_json()}};
        }
        return {{"success", false}, {"error", "Task not found"}};
    });

    proto.register_tool({
        .name = "task_update_status",
        .description = "Update the status of a task",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"task_id", {{"type", "string"}}},
                {"status", {{"type", "string"}, {"enum", json::array({"pending","in_progress","blocked","completed","failed","cancelled"})}}},
            }},
            {"required", json::array({"task_id", "status"})},
        },
        .required_permission = Permission::TaskUpdateStatus,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto task_id = args.value("task_id", "");
        auto status = task_status_from_str(args.value("status", ""));
        if (tasks.set_status(task_id, status)) {
            auto task = tasks.get_task(task_id);
            channels.broadcast_event("task.status_changed", task->to_json(), token);
            if (status == TaskStatus::Completed) channels.broadcast_event("task.completed", task->to_json(), token);
            return {{"success", true}, {"task", task->to_json()}};
        }
        return {{"success", false}, {"error", "Task not found"}};
    });

    proto.register_tool({
        .name = "task_get",
        .description = "Get details of a specific task by ID",
        .input_schema = {
            {"type", "object"},
            {"properties", {{"task_id", {{"type", "string"}}}}},
            {"required", json::array({"task_id"})},
        },
        .required_permission = Permission::TaskRead,
    }, [&](const json& args) -> json {
        auto task = tasks.get_task(args.value("task_id", ""));
        return task ? task->to_json() : json{{"error", "Task not found"}};
    });

    proto.register_tool({
        .name = "task_ready",
        .description = "List tasks whose dependencies are all completed and are ready to start",
        .input_schema = {{"type", "object"}, {"properties", json::object()}},
        .required_permission = Permission::TaskRead,
    }, [&](const json&) -> json {
        auto ready = tasks.get_ready_tasks();
        json result = json::array();
        for (const auto& t : ready) result.push_back(t.to_json());
        return result;
    });

    // ── Agent tools ───────────────────────────────────────────────────

    proto.register_tool({
        .name = "agent_register",
        .description = "Register a new agent in the collaboration swarm",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}}},
                {"platform", {{"type", "string"}}},
                {"capabilities", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"metadata", {{"type", "object"}}},
            }},
            {"required", json::array({"name"})},
        },
        .required_permission = Permission::AgentRegister,
    }, [&](const json& args) -> json {
        AgentInfo info;
        info.name = args.value("name", "");
        info.platform = args.value("platform", "");
        info.capabilities = args.value("capabilities", std::vector<std::string>{});
        info.metadata = args.value("metadata", json::object());
        return {{"info", info.to_json()}, {"note", "Token-issued registration required – use /auth endpoint"}};
    });

    proto.register_tool({
        .name = "agent_list",
        .description = "List all registered agents or filter by status",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"status", {{"type", "string"}, {"enum", json::array({"online","busy","idle","offline"})}}},
                {"swarm_id", {{"type", "string"}, {"description", "Filter by swarm ID"}}},
            }},
        },
        .required_permission = Permission::AgentRead,
    }, [&](const json& args) -> json {
        std::optional<AgentStatus> filter;
        if (args.contains("status")) {
            auto s = args["status"].get<std::string>();
            if (s == "online") filter = AgentStatus::Online;
            else if (s == "busy") filter = AgentStatus::Busy;
            else if (s == "idle") filter = AgentStatus::Idle;
            else if (s == "offline") filter = AgentStatus::Offline;
        }
        auto list = args.contains("swarm_id")
            ? agents.list_swarm_agents(args["swarm_id"].get<std::string>(), filter)
            : agents.list_agents(filter);
        json result = json::array();
        for (const auto& a : list) result.push_back(a.to_json());
        return result;
    });

    proto.register_tool({
        .name = "agent_find_capability",
        .description = "Find agents that have a specific capability",
        .input_schema = {
            {"type", "object"},
            {"properties", {{"capability", {{"type", "string"}}}}},
            {"required", json::array({"capability"})},
        },
        .required_permission = Permission::AgentRead,
    }, [&](const json& args) -> json {
        auto found = agents.find_by_capability(args.value("capability", ""));
        json result = json::array();
        for (const auto& a : found) result.push_back(a.to_json());
        return result;
    });

    proto.register_tool({
        .name = "agent_set_role",
        .description = "Change an agent's role (Coordinator only)",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"agent_id", {{"type", "string"}}},
                {"role", {{"type", "string"}, {"enum", json::array({"coordinator","worker","observer"})}}},
            }},
            {"required", json::array({"agent_id", "role"})},
        },
        .required_permission = Permission::AgentManage,
    }, [&](const json& args) -> json {
        return {{"success", false}, {"error", "Role changes require Coordinator token – use AuthProvider directly"}};
    });

    // ── Context tools ────────────────────────────────────────────────

    proto.register_tool({
        .name = "context_set",
        .description = "Set a shared context key-value pair accessible to all agents in the swarm",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}}},
                {"value", {{"description", "Value to store (any JSON type)"}}},
                {"owner", {{"type", "string"}}},
            }},
            {"required", json::array({"key", "value"})},
        },
        .required_permission = Permission::ContextWrite,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto key = args.value("key", "");
        auto owner = args.value("owner", "");
        context.set(key, args["value"], owner);
        channels.broadcast_event("context.set", {{"key", key}, {"owner", owner}}, token);
        return {{"success", true}, {"key", key}};
    });

    proto.register_tool({
        .name = "context_get",
        .description = "Get a shared context value by key",
        .input_schema = {
            {"type", "object"},
            {"properties", {{"key", {{"type", "string"}}}}},
            {"required", json::array({"key"})},
        },
        .required_permission = Permission::ContextRead,
    }, [&](const json& args) -> json {
        auto entry = context.get(args.value("key", ""));
        return entry ? entry->to_json() : json{{"error", "Key not found"}, {"key", args.value("key", "")}};
    });

    proto.register_tool({
        .name = "context_list",
        .description = "List all context entries, optionally filtered by prefix",
        .input_schema = {
            {"type", "object"},
            {"properties", {{"prefix", {{"type", "string"}}}}},
        },
        .required_permission = Permission::ContextRead,
    }, [&](const json& args) -> json {
        auto list = context.list(args.value("prefix", ""));
        json result = json::array();
        for (const auto& e : list) result.push_back(e.to_json());
        return result;
    });

    proto.register_tool({
        .name = "context_merge",
        .description = "Merge data into an existing context entry",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"key", {{"type", "string"}}},
                {"data", {{"description", "Data to merge"}}},
                {"owner", {{"type", "string"}}},
            }},
            {"required", json::array({"key", "data"})},
        },
        .required_permission = Permission::ContextWrite,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto key = args.value("key", "");
        auto owner = args.value("owner", "");
        if (context.merge(key, args["data"], owner)) {
            channels.broadcast_event("context.merged", {{"key", key}, {"owner", owner}}, token);
            return {{"success", true}, {"key", key}};
        }
        return {{"success", false}, {"error", "Merge failed"}};
    });

    // ── Git/Branch tools ─────────────────────────────────────────────

    proto.register_tool({
        .name = "branch_create",
        .description = "Create a Git branch for a task in the collaboration repository",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"task_id", {{"type", "string"}}},
                {"agent_id", {{"type", "string"}}},
                {"base", {{"type", "string"}}},
            }},
            {"required", json::array({"task_id", "agent_id"})},
        },
        .required_permission = Permission::BranchCreate,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto branch = branches.create_branch(args.value("task_id", ""), args.value("agent_id", ""), args.value("base", ""));
        if (!branch.empty()) {
            auto info = branches.get_branch_info(branch);
            channels.broadcast_event("branch.created", info->to_json(), token);
            return info->to_json();
        }
        return {{"error", "Failed to create branch"}};
    });

    proto.register_tool({
        .name = "branch_commit",
        .description = "Commit changes on a task branch",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"branch", {{"type", "string"}}},
                {"message", {{"type", "string"}}},
                {"agent_id", {{"type", "string"}}},
            }},
            {"required", json::array({"branch", "message", "agent_id"})},
        },
        .required_permission = Permission::BranchCommit,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        if (branches.commit_changes(args.value("branch", ""), args.value("message", ""), args.value("agent_id", ""))) {
            channels.broadcast_event("branch.committed", {{"branch", args.value("branch", "")}, {"agent", args.value("agent_id", "")}}, token);
            return {{"success", true}, {"branch", args.value("branch", "")}};
        }
        return {{"success", false}, {"error", "Commit failed"}};
    });

    // ── Merge tools ──────────────────────────────────────────────────

    proto.register_tool({
        .name = "merge_request",
        .description = "Request a merge of a branch into another branch",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"source", {{"type", "string"}}},
                {"target", {{"type", "string"}}},
                {"requester", {{"type", "string"}}},
                {"strategy", {{"type", "string"}, {"enum", json::array({"merge","rebase","squash"})}}},
            }},
            {"required", json::array({"source", "requester"})},
        },
        .required_permission = Permission::MergeRequest,
    }, [&](const json& args) -> json {
        MergeStrategy ms = MergeStrategy::Merge;
        auto s = args.value("strategy", "merge");
        if (s == "rebase") ms = MergeStrategy::Rebase;
        else if (s == "squash") ms = MergeStrategy::Squash;
        auto id = merges.request_merge(args.value("source", ""), args.value("target", "main"), args.value("requester", ""), ms);
        auto req = merges.get_request(id);
        return req->to_json();
    });

    proto.register_tool({
        .name = "merge_approve",
        .description = "Approve a pending merge request (Coordinator only)",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"merge_id", {{"type", "string"}}},
                {"reviewer", {{"type", "string"}}},
            }},
            {"required", json::array({"merge_id", "reviewer"})},
        },
        .required_permission = Permission::MergeApprove,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        if (merges.approve_merge(args.value("merge_id", ""), args.value("reviewer", ""))) {
            auto req = merges.get_request(args.value("merge_id", ""));
            channels.broadcast_event("merge.approved", req->to_json(), token);
            return req->to_json();
        }
        return {{"error", "Approval failed"}};
    });

    proto.register_tool({
        .name = "merge_execute",
        .description = "Execute an approved merge request",
        .input_schema = {
            {"type", "object"},
            {"properties", {{"merge_id", {{"type", "string"}}}}},
            {"required", json::array({"merge_id"})},
        },
        .required_permission = Permission::MergeExecute,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto merge_id = args.value("merge_id", "");
        if (merges.execute_merge(merge_id)) {
            auto req = merges.get_request(merge_id);
            channels.broadcast_event("merge.completed", req->to_json(), token);
            return req->to_json();
        }
        return {{"error", "Merge execution failed"}};
    });

    // ── Event tools ──────────────────────────────────────────────────

    proto.register_tool({
        .name = "event_publish",
        .description = "Publish an event to the collaboration network via MQTT",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"event_type", {{"type", "string"}}},
                {"data", {{"type", "object"}}},
                {"source", {{"type", "string"}}},
            }},
            {"required", json::array({"event_type", "source"})},
        },
        .required_permission = Permission::EventPublish,
    }, [&](const json& args) -> json {
        auto token = auth_from_args(args);
        auto event_type = args.value("event_type", "");
        auto source = args.value("source", "");
        auto data = args.value("data", json::object());
        channels.broadcast_event(event_type, data, token);
        events.emit(event_type, source, data);
        return {{"success", true}, {"event_type", event_type}};
    });

    proto.register_tool({
        .name = "event_recent",
        .description = "Get recent events from the event log",
        .input_schema = {
            {"type", "object"},
            {"properties", {
                {"count", {{"type", "integer"}}},
                {"type", {{"type", "string"}}},
            }},
        },
        .required_permission = Permission::EventRead,
    }, [&](const json& args) -> json {
        auto count = args.value("count", 50);
        if (args.contains("type")) {
            auto evts = events.query_events(args["type"].get<std::string>(), count);
            json result = json::array();
            for (const auto& e : evts) result.push_back(e.to_json());
            return result;
        }
        auto evts = events.recent_events(count);
        json result = json::array();
        for (const auto& e : evts) result.push_back(e.to_json());
        return result;
    });

    // ── Auth/heartbeat ────────────────────────────────────────────────

    proto.register_tool({
        .name = "heartbeat",
        .description = "Send a heartbeat to indicate an agent is still active",
        .input_schema = {
            {"type", "object"},
            {"properties", {{"agent_id", {{"type", "string"}}}}},
            {"required", json::array({"agent_id"})},
        },
        .required_permission = Permission::AgentRead,
    }, [&](const json& args) -> json {
        auto agent_id = args.value("agent_id", "");
        if (agents.heartbeat(agent_id)) return {{"success", true}, {"agent_id", agent_id}};
        return {{"success", false}, {"error", "Agent not found"}};
    });
}

}