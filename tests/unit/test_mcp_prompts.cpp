#include <gtest/gtest.h>
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
#include "mcp_collab/collab_defs.hpp"

using namespace mcp_collab;

class CollabPromptsTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test-prompts", .version = "1.0.0"}};

    void SetUp() override {
        register_collab_prompts(proto);
        proto.handle_request({{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"}, {"params", {}}});
    }

    json list_prompts() {
        json req = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "prompts/list"}, {"params", {}}};
        auto resp = proto.handle_request(req);
        return resp["result"]["prompts"];
    }

    json get_prompt(const std::string& name, const json& args = {}) {
        json req = {
            {"jsonrpc", "2.0"},
            {"id", 3},
            {"method", "prompts/get"},
            {"params", {{"name", name}, {"arguments", args}}}
        };
        auto resp = proto.handle_request(req);
        return resp["result"];
    }
};

TEST_F(CollabPromptsTest, ListAllPrompts) {
    auto prompts = list_prompts();
    EXPECT_EQ(prompts.size(), 4u);

    std::vector<std::string> names;
    for (const auto& p : prompts) {
        names.push_back(p["name"].get<std::string>());
    }
    EXPECT_NE(std::find(names.begin(), names.end(), "delegate_task"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "review_and_merge"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "sync_context"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "plan_parallel_work"), names.end());
}

TEST_F(CollabPromptsTest, DelegateTaskWithAgent) {
    auto result = get_prompt("delegate_task", {
        {"task_title", "Write tests"},
        {"task_description", "Write unit tests for tools"},
        {"target_agent", "agent-1"},
        {"priority", "high"}
    });
    EXPECT_TRUE(result.contains("messages"));
    auto& msgs = result["messages"];
    EXPECT_GE(msgs.size(), 2u);
    EXPECT_EQ(msgs[0]["role"], "system");

    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("Write tests"), std::string::npos);
    EXPECT_NE(content.find("agent-1"), std::string::npos);
}

TEST_F(CollabPromptsTest, DelegateTaskAutoAssign) {
    auto result = get_prompt("delegate_task", {
        {"task_title", "Auto task"},
        {"task_description", "Auto assigned"}
    });
    auto& msgs = result["messages"];
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("best available agent"), std::string::npos);
}

TEST_F(CollabPromptsTest, ReviewAndMerge) {
    auto result = get_prompt("review_and_merge", {
        {"source_branch", "feature/x"},
        {"target_branch", "develop"},
        {"strategy", "squash"}
    });
    auto& msgs = result["messages"];
    EXPECT_GE(msgs.size(), 2u);
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("feature/x"), std::string::npos);
    EXPECT_NE(content.find("develop"), std::string::npos);
}

TEST_F(CollabPromptsTest, ReviewAndMergeDefaultTarget) {
    auto result = get_prompt("review_and_merge", {
        {"source_branch", "feature/y"}
    });
    auto& msgs = result["messages"];
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("main"), std::string::npos);
}

TEST_F(CollabPromptsTest, SyncContextRead) {
    auto result = get_prompt("sync_context", {
        {"context_key", "my.key"},
        {"action", "read"}
    });
    auto& msgs = result["messages"];
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("context_get"), std::string::npos);
}

TEST_F(CollabPromptsTest, SyncContextWrite) {
    auto result = get_prompt("sync_context", {
        {"context_key", "my.key"},
        {"action", "write"}
    });
    auto& msgs = result["messages"];
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("context_set"), std::string::npos);
}

TEST_F(CollabPromptsTest, SyncContextMerge) {
    auto result = get_prompt("sync_context", {
        {"context_key", "my.key"},
        {"action", "merge"}
    });
    auto& msgs = result["messages"];
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("context_merge"), std::string::npos);
}

TEST_F(CollabPromptsTest, PlanParallelWork) {
    auto result = get_prompt("plan_parallel_work", {
        {"goal", "Build the API"},
        {"agent_count", 4}
    });
    auto& msgs = result["messages"];
    EXPECT_GE(msgs.size(), 2u);
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("Build the API"), std::string::npos);
    EXPECT_NE(content.find("4"), std::string::npos);
}

TEST_F(CollabPromptsTest, PlanParallelWorkDefaultAgentCount) {
    auto result = get_prompt("plan_parallel_work", {
        {"goal", "Refactor everything"}
    });
    auto& msgs = result["messages"];
    std::string content = msgs[1]["content"].get<std::string>();
    EXPECT_NE(content.find("2"), std::string::npos);
}

TEST_F(CollabPromptsTest, GetNonexistentPrompt) {
    json req = {
        {"jsonrpc", "2.0"},
        {"id", 4},
        {"method", "prompts/get"},
        {"params", {{"name", "nonexistent_prompt"}}}
    };
    auto resp = proto.handle_request(req);
    EXPECT_TRUE(resp.contains("error"));
}
