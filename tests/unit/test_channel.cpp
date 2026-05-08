#include <gtest/gtest.h>
#include "mcp_collab/channel.hpp"
#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/auth.hpp"
#include <format>

using namespace mcp_collab;

class ChannelTest : public ::testing::Test {
protected:
    SecureMqttClient mqtt{MqttConfig{.host = "localhost", .port = 18839}, "test-swarm", "secret"};
    ChannelManager channels{mqtt, "mcp-collab", "test-swarm"};
};

TEST_F(ChannelTest, GetPredefinedChannels) {
    auto& task_ch = channels.get(ChannelType::TaskUpdates);
    EXPECT_NE(task_ch.topic().find("tasks"), std::string::npos);

    auto& agent_ch = channels.get(ChannelType::AgentPresence);
    EXPECT_NE(agent_ch.topic().find("agents"), std::string::npos);

    auto& events_ch = channels.get(ChannelType::Events);
    EXPECT_NE(events_ch.topic().find("events"), std::string::npos);

    auto& ctx_ch = channels.get(ChannelType::ContextSync);
    EXPECT_NE(ctx_ch.topic().find("context"), std::string::npos);

    auto& git_ch = channels.get(ChannelType::GitCoordination);
    EXPECT_NE(git_ch.topic().find("git"), std::string::npos);

    auto& bc_ch = channels.get(ChannelType::Broadcast);
    EXPECT_NE(bc_ch.topic().find("broadcast"), std::string::npos);
}

TEST_F(ChannelTest, CreateCustomChannel) {
    auto& custom = channels.create(ChannelType::Custom, "my-channel");
    EXPECT_NE(custom.topic().find("custom"), std::string::npos);
    EXPECT_NE(custom.topic().find("my-channel"), std::string::npos);
}

TEST_F(ChannelTest, RemoveChannel) {
    channels.create(ChannelType::Custom, "to-remove");
    channels.remove(ChannelType::Custom, "to-remove");
    auto& recreated = channels.get(ChannelType::Custom, "to-remove");
    EXPECT_NE(recreated.topic().find("to-remove"), std::string::npos);
}

TEST_F(ChannelTest, TopicFormat) {
    auto& ch = channels.get(ChannelType::TaskUpdates);
    std::string topic = ch.topic();
    EXPECT_EQ(topic.substr(0, 10), "mcp-collab");
}

TEST_F(ChannelTest, ChannelSpecPreserved) {
    auto& ch = channels.get(ChannelType::TaskUpdates);
    EXPECT_EQ(ch.spec().type, ChannelType::TaskUpdates);
}

TEST_F(ChannelTest, BroadcastEvent) {
    AuthToken token;
    token.agent_id = "test-agent";
    token.role = Role::Coordinator;
    token.swarm_id = "test-swarm";
    EXPECT_NO_THROW(channels.broadcast_event("test.event", {{"key", "value"}}, token));
}

TEST_F(ChannelTest, Broadcast) {
    AuthToken token;
    token.agent_id = "test-agent";
    token.role = Role::Coordinator;
    token.swarm_id = "test-swarm";
    EXPECT_NO_THROW(channels.broadcast({{"message", "hello"}}, token));
}

TEST_F(ChannelTest, ChannelOnMessage) {
    auto& ch = channels.get(ChannelType::Events);
    bool callback_set = false;
    ch.on_message([&callback_set](const MqttEnvelope& env) {
        callback_set = true;
    });
}

TEST_F(ChannelTest, MultipleChannelTypesDistinctTopics) {
    auto& t1 = channels.get(ChannelType::TaskUpdates);
    auto& t2 = channels.get(ChannelType::AgentPresence);
    auto& t3 = channels.get(ChannelType::Events);
    EXPECT_NE(t1.topic(), t2.topic());
    EXPECT_NE(t2.topic(), t3.topic());
    EXPECT_NE(t1.topic(), t3.topic());
}
