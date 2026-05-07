#include "mcp_collab/collab_defs.hpp"

namespace mcp_collab {

void register_collab_prompts(McpProtocol& proto) {

    proto.register_prompt({
        .name = "delegate_task",
        .description = "Prompt template for delegating a task to an agent with full context",
        .arguments = {
            {{"name", "task_title"}, {"description", "Title of the task to delegate"}, {"required", true}},
            {{"name", "task_description"}, {"description", "Detailed task description"}, {"required", true}},
            {{"name", "target_agent"}, {"description", "Agent ID to delegate to (empty for auto-assign)"}, {"required", false}},
            {{"name", "priority"}, {"description", "Task priority: low, medium, high, critical"}, {"required", false}},
        },
    }, [](const json& args) -> json {
        auto title = args.value("task_title", "");
        auto desc = args.value("task_description", "");
        auto agent = args.value("target_agent", "");
        auto pri = args.value("priority", "medium");

        std::string assignment = agent.empty()
            ? "Find the best available agent for this task based on capabilities and workload."
            : std::format("Assign this task to agent `{}`.", agent);

        return json::array({
            json{{"role", "system"}, {"content", "You are a task coordinator in a multi-agent collaboration network. Create and delegate tasks clearly."}},
            json{{"role", "user"}, {"content", std::format(
                "Create a new task with:\n"
                "- Title: {}\n"
                "- Description: {}\n"
                "- Priority: {}\n\n"
                "{}\n"
                "Use the task_create tool to create the task, then task_assign to assign it.",
                title, desc, pri, assignment)}},
        });
    });

    proto.register_prompt({
        .name = "review_and_merge",
        .description = "Prompt template for reviewing a branch and requesting a merge",
        .arguments = {
            {{"name", "source_branch"}, {"description", "Branch to merge from"}, {"required", true}},
            {{"name", "target_branch"}, {"description", "Branch to merge into (default: main)"}, {"required", false}},
            {{"name", "strategy"}, {"description", "Merge strategy: merge, rebase, squash"}, {"required", false}},
        },
    }, [](const json& args) -> json {
        auto source = args.value("source_branch", "");
        auto target = args.value("target_branch", "main");
        auto strategy = args.value("strategy", "squash");

        return json::array({
            json{{"role", "system"}, {"content", "You are a code review coordinator. Review branch changes and manage the merge process."}},
            json{{"role", "user"}, {"content", std::format(
                "Review the changes on branch `{}` targeting `{}`:\n\n"
                "1. Use the branch_diff tool to examine the changes\n"
                "2. If the changes look good, use merge_request with strategy '{}' to create a merge request\n"
                "3. Use merge_approve and merge_execute to complete the merge\n\n"
                "Report any issues found during review.",
                source, target, strategy)}},
        });
    });

    proto.register_prompt({
        .name = "sync_context",
        .description = "Prompt template for syncing shared context between agents",
        .arguments = {
            {{"name", "context_key"}, {"description", "Context key or prefix to sync"}, {"required", true}},
            {{"name", "action"}, {"description", "Action: read, write, or merge"}, {"required", true}},
        },
    }, [](const json& args) -> json {
        auto key = args.value("context_key", "");
        auto action = args.value("action", "read");

        std::string instruction;
        if (action == "read") {
            instruction = std::format("Read the shared context at key '{}' using context_get, then summarize the current state for the agent.", key);
        } else if (action == "write") {
            instruction = std::format("Write new context data to key '{}' using context_set. Include all relevant information for other agents.", key);
        } else {
            instruction = std::format("Merge new data into existing context at key '{}' using context_merge. Preserve existing data while adding new information.", key);
        }

        return json::array({
            json{{"role", "system"}, {"content", "You are managing shared context in a multi-agent system. Ensure data consistency across agents."}},
            json{{"role", "user"}, {"content", instruction}},
        });
    });

    proto.register_prompt({
        .name = "plan_parallel_work",
        .description = "Prompt template for planning parallel work across multiple agents",
        .arguments = {
            {{"name", "goal"}, {"description", "High-level goal to accomplish"}, {"required", true}},
            {{"name", "agent_count"}, {"description", "Number of available agents"}, {"required", false}},
        },
    }, [](const json& args) -> json {
        auto goal = args.value("goal", "");
        auto count = args.value("agent_count", 2);

        return json::array({
            json{{"role", "system"}, {"content", "You are a task planner for a multi-agent system. Break down work into independent tasks that can run in parallel, with clear dependencies."}},
            json{{"role", "user"}, {"content", std::format(
                "Plan parallel work for the following goal:\n\n{}\n\n"
                "Create a task breakdown with:\n"
                "1. {} independent tasks using task_create\n"
                "2. Set dependencies using task_add_dependency\n"
                "3. Assign tasks to idle agents using task_assign and agent_list\n"
                "4. Use context_set to share planning context\n\n"
                "Ensure tasks are granular enough for parallel execution but have proper dependency ordering.",
                goal, count)}},
        });
    });
}

}