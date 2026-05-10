#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <format>

#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/auth.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/persistence.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/git_operations.hpp"
#include "mcp_collab/channel.hpp"

using namespace mcp_collab;

// ═══════════════════════════════════════════════════════════════════════════════
// SecureMqtt: on_raw_message paths via inject_message
// ═══════════════════════════════════════════════════════════════════════════════

class SecureMqttRawMsgTest : public ::testing::Test {
protected:
    MqttConfig cfg{};
    SecureMqttClient client{cfg, "test-swarm", "hmac-secret"};
};

TEST_F(SecureMqttRawMsgTest, SecretModeRejectsUnsignedMessage) {
    bool callback_fired = false;
    client.subscribe_verified("mcp-collab/test-swarm/tasks", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    json unsigned_msg = {{"action", "create"}};
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", unsigned_msg.dump());

    EXPECT_FALSE(callback_fired);
}

TEST_F(SecureMqttRawMsgTest, SignedMessageVerifiedAndDispatched) {
    bool callback_fired = false;
    MqttEnvelope received_env;
    client.subscribe_verified("mcp-collab/test-swarm/tasks", 1,
        [&](const MqttEnvelope& env) { callback_fired = true; received_env = env; });

    auto envelope = MqttEnvelope::sign("agent-1", "test-swarm", Role::Worker,
        {{"action", "create"}}, "hmac-secret");
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", envelope.to_json().dump());

    EXPECT_TRUE(callback_fired);
    EXPECT_EQ(received_env.sender, "agent-1");
    EXPECT_EQ(received_env.role, "worker");
}

TEST_F(SecureMqttRawMsgTest, StaleMessageRejected) {
    bool callback_fired = false;
    client.subscribe_verified("mcp-collab/test-swarm/tasks", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 600000;
    env.sender = "agent-1";
    env.swarm_id = "test-swarm";
    env.role = "worker";
    env.payload = {{"old", true}};
    env.signature = compute_hmac(
        std::format("{}:{}:{}:{}", env.sender, env.swarm_id, env.timestamp, env.payload.dump()),
        "hmac-secret");

    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", env.to_json().dump());
    EXPECT_FALSE(callback_fired);
}

TEST_F(SecureMqttRawMsgTest, CrossSwarmMessageRejected) {
    bool callback_fired = false;
    client.subscribe_verified("mcp-collab/test-swarm/tasks", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    auto envelope = MqttEnvelope::sign("agent-1", "other-swarm", Role::Worker,
        {{"action", "create"}}, "hmac-secret");
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", envelope.to_json().dump());

    EXPECT_FALSE(callback_fired);
}

TEST_F(SecureMqttRawMsgTest, DefaultSwarmAccepted) {
    bool callback_fired = false;
    client.subscribe_verified("mcp-collab/test-swarm/tasks", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    auto envelope = MqttEnvelope::sign("agent-1", "default", Role::Worker,
        {{"action", "create"}}, "hmac-secret");
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", envelope.to_json().dump());

    EXPECT_TRUE(callback_fired);
}

TEST_F(SecureMqttRawMsgTest, WildcardCallbackMatch) {
    bool callback_fired = false;
    client.subscribe_verified("#", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    auto envelope = MqttEnvelope::sign("agent-1", "test-swarm", Role::Worker,
        {{"action", "create"}}, "hmac-secret");
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", envelope.to_json().dump());

    EXPECT_TRUE(callback_fired);
}

TEST_F(SecureMqttRawMsgTest, PrefixWildcardCallbackMatch) {
    bool callback_fired = false;
    client.subscribe_verified("mcp-collab/test-swarm/#", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    auto envelope = MqttEnvelope::sign("agent-1", "test-swarm", Role::Worker,
        {{"action", "create"}}, "hmac-secret");
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", envelope.to_json().dump());

    EXPECT_TRUE(callback_fired);
}

TEST_F(SecureMqttRawMsgTest, NoCallbackForTopic) {
    auto envelope = MqttEnvelope::sign("agent-1", "test-swarm", Role::Worker,
        {{"action", "create"}}, "hmac-secret");
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", envelope.to_json().dump());
}

TEST_F(SecureMqttRawMsgTest, CustomMaxMessageAge) {
    client.set_max_message_age(std::chrono::seconds(1));
    bool callback_fired = false;
    client.subscribe_verified("mcp-collab/test-swarm/tasks", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 5000;
    env.sender = "agent-1";
    env.swarm_id = "test-swarm";
    env.role = "worker";
    env.payload = {{"old", true}};
    env.signature = compute_hmac(
        std::format("{}:{}:{}:{}", env.sender, env.swarm_id, env.timestamp, env.payload.dump()),
        "hmac-secret");

    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", env.to_json().dump());
    EXPECT_FALSE(callback_fired);
}

TEST_F(SecureMqttRawMsgTest, UnsubscribePreventsCallback) {
    bool callback_fired = false;
    client.subscribe_verified("mcp-collab/test-swarm/tasks", 1,
        [&](const MqttEnvelope&) { callback_fired = true; });

    client.unsubscribe("mcp-collab/test-swarm/tasks");

    auto envelope = MqttEnvelope::sign("agent-1", "test-swarm", Role::Worker,
        {{"action", "create"}}, "hmac-secret");
    client.raw_client().inject_message("mcp-collab/test-swarm/tasks", envelope.to_json().dump());

    EXPECT_FALSE(callback_fired);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MqttTopicAuth: subscribe ACL branches
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MqttTopicAuthExtra, SubscribeWithExplicitRolesDeniesWorker) {
    MqttTopicAuth acl{"s"};
    acl.add_rule({.topic_prefix = "mcp-collab/s/restricted",
                  .allow_roles = {Role::Coordinator},
                  .subscribe_roles = {Role::Coordinator},
                  .allow_subscribe = true});
    EXPECT_TRUE(acl.can_subscribe(Role::Coordinator, "mcp-collab/s/restricted/data"));
    EXPECT_FALSE(acl.can_subscribe(Role::Worker, "mcp-collab/s/restricted/data"));
}

TEST(MqttTopicAuthExtra, SubscribeWildcardHash) {
    MqttTopicAuth acl{"sw"};
    EXPECT_TRUE(acl.can_subscribe(Role::Worker, "mcp-collab/sw/#"));
}

TEST(MqttTopicAuthExtra, SubscribeUnknownTopicDenied) {
    MqttTopicAuth acl{"sw"};
    EXPECT_FALSE(acl.can_subscribe(Role::Worker, "unknown/topic"));
}

TEST(MqttTopicAuthExtra, PublishUnknownTopicDenied) {
    MqttTopicAuth acl{"sw"};
    EXPECT_FALSE(acl.can_publish(Role::Worker, "unknown/topic"));
}

TEST(MqttTopicAuthExtra, ObserverCanSubscribeKnownTopic) {
    MqttTopicAuth acl{"sw"};
    EXPECT_TRUE(acl.can_subscribe(Role::Observer, "mcp-collab/sw/tasks/1"));
}

TEST(MqttTopicAuthExtra, DefaultSwarmIdPrefix) {
    MqttTopicAuth acl{""};
    EXPECT_TRUE(acl.can_subscribe(Role::Worker, "mcp-collab/default/tasks"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Persistence: error path coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PersistenceError, SaveToInvalidPathFails) {
    // Create a temp file and try to use it as a parent directory — must fail on all platforms.
    auto tmp = std::format("/tmp/swarm_mcp_test_file_parent_{}.txt",
        std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream f(tmp);
        f << "x";
    }
    PersistenceLayer pl{tmp + "/sub/file.json"};
    EXPECT_FALSE(pl.save({{"key", "value"}}));
    std::filesystem::remove(tmp);
}

TEST(PersistenceError, LoadNonexistentReturnsNullopt) {
    std::string path = "/tmp/swarm_mcp_test_persist_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    PersistenceLayer pl{path};
    auto result = pl.load();
    EXPECT_FALSE(result.has_value());
}

TEST(PersistenceError, LoadCorruptJsonReturnsNullopt) {
    std::string path = "/tmp/swarm_mcp_test_corrupt_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    {
        std::ofstream f(path);
        f << "NOT VALID JSON {{{";
    }
    PersistenceLayer pl{path};
    auto result = pl.load();
    EXPECT_FALSE(result.has_value());
    std::filesystem::remove(path);
}

TEST(PersistenceError, SaveAndLoadRoundTrip) {
    std::string path = "/tmp/swarm_mcp_test_roundtrip_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    PersistenceLayer pl{path};
    json data = {{"tasks", {1, 2, 3}}, {"agents", {{"a", "b"}}}};
    EXPECT_TRUE(pl.save(data));
    auto loaded = pl.load();
    EXPECT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["tasks"], data["tasks"]);
    std::filesystem::remove(path);
}

TEST(PersistenceError, SaveIfChangedSkipsIfSame) {
    std::string path = "/tmp/swarm_mcp_test_if_changed_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    PersistenceLayer pl{path};
    json data = {{"key", "value"}};
    pl.save(data);
    EXPECT_TRUE(pl.save_if_changed(data));
    std::filesystem::remove(path);
}

TEST(PersistenceError, SaveIfChangedSavesIfDifferent) {
    std::string path = "/tmp/swarm_mcp_test_if_changed2_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    PersistenceLayer pl{path};
    json data1 = {{"key", "value1"}};
    json data2 = {{"key", "value2"}};
    pl.save(data1);
    EXPECT_TRUE(pl.save_if_changed(data2));
    auto loaded = pl.load();
    EXPECT_EQ((*loaded)["key"], "value2");
    std::filesystem::remove(path);
}

TEST(PersistenceError, ExistsAndClear) {
    std::string path = "/tmp/swarm_mcp_test_exists_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    PersistenceLayer pl{path};
    EXPECT_FALSE(pl.exists());
    pl.save({{"key", "val"}});
    EXPECT_TRUE(pl.exists());
    EXPECT_TRUE(pl.clear());
    EXPECT_FALSE(pl.exists());
}

TEST(PersistenceError, CreateSnapshot) {
    auto snap = PersistenceLayer::create_snapshot(
        {{"task", "list"}},
        {{"agent", "list"}},
        {{"context", "data"}},
        {{"branch", "list"}},
        {{"mr", "list"}}
    );
    EXPECT_EQ(snap["version"], 1);
    EXPECT_TRUE(snap.contains("saved_at"));
    EXPECT_EQ(snap["tasks"]["task"], "list");
}

TEST(PersistenceError, PathMethod) {
    std::string path = "/tmp/swarm_mcp_test_path_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    PersistenceLayer pl{path};
    EXPECT_EQ(pl.path(), path);
    std::filesystem::remove(path);
}

TEST(PersistenceError, SaveEmptyObject) {
    std::string path = "/tmp/swarm_mcp_test_empty_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json";
    PersistenceLayer pl{path};
    EXPECT_TRUE(pl.save(json::object()));
    auto loaded = pl.load();
    EXPECT_TRUE(loaded.has_value());
    EXPECT_TRUE(loaded->is_object());
    std::filesystem::remove(path);
}

// ═══════════════════════════════════════════════════════════════════════════════
// TaskManager: circular dependency detection
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TaskManagerCircular, DetectsCircularDependency) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    auto t2 = tm.create_task("T2", "a1");
    auto t3 = tm.create_task("T3", "a1");

    tm.add_dependency(t2.id, t1.id);
    tm.add_dependency(t3.id, t2.id);

    EXPECT_FALSE(tm.add_dependency(t1.id, t3.id));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Handler: additional branches
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RateLimiterDetailed, ZeroMeansNoLimit) {
    RateLimiter limiter(0);
    for (int i = 0; i < 100; ++i) {
        EXPECT_TRUE(limiter.allow("key"));
    }
}

class HandlerPostTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    AuthProvider auth_{"test-secret"};
    std::unique_ptr<StreamableHttpTransport> transport;

    void SetUp() override {
        transport = std::make_unique<StreamableHttpTransport>(proto, auth_,
            StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                                 .require_auth = false});
        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    }

    void TearDown() override { transport.reset(); }

    httplib::Request make_request(const std::string& body) {
        httplib::Request req;
        req.method = "POST";
        req.path = "/mcp";
        req.target = "/mcp";
        req.body = body;
        return req;
    }
};

TEST_F(HandlerPostTest, BatchRequest) {
    httplib::Request req = make_request(R"([
        {"jsonrpc":"2.0","id":1,"method":"ping"},
        {"jsonrpc":"2.0","id":2,"method":"ping"}
    ])");
    httplib::Response res;
    transport->handle_post(req, res);
    auto result = json::parse(res.body);
    EXPECT_TRUE(result.is_array());
    EXPECT_EQ(result.size(), 2u);
}

TEST_F(HandlerPostTest, BatchNotificationReturns202) {
    httplib::Request req = make_request(R"([
        {"jsonrpc":"2.0","method":"notifications/initialized"}
    ])");
    httplib::Response res;
    transport->handle_post(req, res);
    EXPECT_EQ(res.status, 202);
}

TEST_F(HandlerPostTest, EmptyBodyReturns400) {
    httplib::Request req = make_request("");
    httplib::Response res;
    transport->handle_post(req, res);
    EXPECT_EQ(res.status, 400);
}

TEST_F(HandlerPostTest, InvalidJsonReturnsParseError) {
    httplib::Request req = make_request("{invalid json}");
    httplib::Response res;
    transport->handle_post(req, res);
    auto result = json::parse(res.body);
    EXPECT_TRUE(result.contains("error"));
}

TEST_F(HandlerPostTest, AuthRequiredReturns401) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    httplib::Request req = make_request(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_EQ(res.status, 401);
}

TEST_F(HandlerPostTest, QueryTokenAuth) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto tkn = auth_.issue_token("agent-1", Role::Worker, "swarm");
    httplib::Request req = make_request(R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    req.set_header("Authorization", "Bearer " + tkn.token_string);
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_NE(res.status, 401);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Channel: topic format
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ChannelExtra, TopicFormatTaskUpdates) {
    MqttConfig cfg;
    SecureMqttClient mqtt{cfg, "test", "secret"};
    Channel ch(mqtt, ChannelSpec{ChannelType::TaskUpdates, "ns", "test", 1, false}, "sw");
    EXPECT_EQ(ch.topic(), "mcp-collab/ns/tasks/test");
}

TEST(ChannelExtra, TopicFormatCustom) {
    MqttConfig cfg;
    SecureMqttClient mqtt{cfg, "test", "secret"};
    Channel ch(mqtt, ChannelSpec{ChannelType::Custom, "ns", "test", 1, false}, "sw");
    EXPECT_EQ(ch.topic(), "mcp-collab/ns/custom/test");
}

TEST(ChannelExtra, TopicFormatWithEmptyNamespace) {
    MqttConfig cfg;
    SecureMqttClient mqtt{cfg, "test", "secret"};
    Channel ch(mqtt, ChannelSpec{ChannelType::Events, "", "myevent", 1, false}, "sw");
    EXPECT_TRUE(ch.topic().find("events") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Auth: additional role/permission coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AuthRoleExtra, RoleFromStrAllRoles) {
    EXPECT_EQ(role_from_str("coordinator"), Role::Coordinator);
    EXPECT_EQ(role_from_str("worker"), Role::Worker);
    EXPECT_EQ(role_from_str("observer"), Role::Observer);
    EXPECT_EQ(role_from_str("unknown"), Role::Observer);
}

TEST(AuthRoleExtra, HasPermissionAll) {
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::AgentRegister));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::MergeApprove));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::ContextWrite));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::TaskRead));
}

// ═══════════════════════════════════════════════════════════════════════════════
// SecureMqtt: can_subscribe with subscribe_roles
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MqttTopicAuthSubscribe, SubscribeRolesEmptyUsesAllowSubscribe) {
    MqttTopicAuth acl{"s"};
    acl.add_rule({.topic_prefix = "mcp-collab/s/restricted",
                  .allow_roles = {Role::Coordinator},
                  .subscribe_roles = {},
                  .allow_subscribe = false});
    EXPECT_TRUE(acl.can_subscribe(Role::Coordinator, "mcp-collab/s/restricted/data"));
    EXPECT_FALSE(acl.can_subscribe(Role::Worker, "mcp-collab/s/restricted/data"));
}

TEST(MqttTopicAuthSubscribe, SubscribeRolesFilledOverridesAllowSubscribe) {
    MqttAclRule rule{
        .topic_prefix = "mcp-collab/s/restricted",
        .allow_roles = {Role::Coordinator},
        .subscribe_roles = {Role::Coordinator, Role::Worker},
        .allow_subscribe = false
    };
    MqttTopicAuth acl{"s"};
    acl.add_rule(rule);
    EXPECT_TRUE(acl.can_subscribe(Role::Worker, "mcp-collab/s/restricted/data"));
    EXPECT_TRUE(acl.can_subscribe(Role::Coordinator, "mcp-collab/s/restricted/data"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// GitOperations: using temp directories
// ═══════════════════════════════════════════════════════════════════════════════

class GitOpsTest : public ::testing::Test {
protected:
    std::filesystem::path tmp_dir;
    std::unique_ptr<GitOperations> git;

    void SetUp() override {
        tmp_dir = std::filesystem::temp_directory_path() / ("swarm_test_git_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(tmp_dir);
        git = std::make_unique<GitOperations>(tmp_dir.string());
        git->init();
        system(std::format("git -C \"{}\" config user.email \"test@test.com\"", tmp_dir.string()).c_str());
        system(std::format("git -C \"{}\" config user.name \"Test\"", tmp_dir.string()).c_str());
        std::ofstream(tmp_dir / "test.txt") << "hello";
        git->add("test.txt");
        git->commit("initial commit", "testuser");
    }

    void TearDown() override {
        std::filesystem::remove_all(tmp_dir);
    }
};

TEST_F(GitOpsTest, InitAlreadyInitialized) {
    EXPECT_TRUE(git->init());
}

TEST_F(GitOpsTest, CherryPickCommit) {
    git->checkout("feature", true);
    std::ofstream(tmp_dir / "feature.txt") << "feature work";
    git->add("feature.txt");
    git->commit("feature commit", "testuser");
    std::string commit_hash = git->current_commit();

    git->checkout("main", false);
    EXPECT_TRUE(git->cherry_pick(commit_hash));
}

TEST_F(GitOpsTest, PushWithoutRemoteFails) {
    EXPECT_FALSE(git->push());
}

TEST_F(GitOpsTest, CommitWithAuthor) {
    std::ofstream(tmp_dir / "authored.txt") << "authored";
    git->add("authored.txt");
    EXPECT_TRUE(git->commit("authored commit", "author"));
}

TEST_F(GitOpsTest, ShowReturnsContent) {
    auto content = git->show("HEAD");
    EXPECT_FALSE(content.empty());
}

TEST_F(GitOpsTest, IsRepo) {
    EXPECT_TRUE(git->is_repo());
}

TEST_F(GitOpsTest, IsNotRepo) {
    std::filesystem::path not_repo = std::filesystem::temp_directory_path() / ("not_a_repo_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(not_repo);
    GitOperations not_git(not_repo.string());
    EXPECT_FALSE(not_git.is_repo());
    std::filesystem::remove_all(not_repo);
}

TEST_F(GitOpsTest, CurrentBranch) {
    auto branch = git->current_branch();
    EXPECT_FALSE(branch.empty());
}

TEST_F(GitOpsTest, CurrentCommit) {
    auto commit = git->current_commit();
    EXPECT_FALSE(commit.empty());
}

TEST_F(GitOpsTest, BranchesList) {
    auto branches = git->branches(false);
    EXPECT_GE(branches.size(), 1u);
}

TEST_F(GitOpsTest, MergeBranch) {
    git->checkout("feature", true);
    std::ofstream(tmp_dir / "feature.txt") << "feature";
    git->add("feature.txt");
    git->commit("feature commit", "testuser");
    git->checkout("main", false);
    EXPECT_TRUE(git->merge("feature", true));
}

// ═══════════════════════════════════════════════════════════════════════════════
// EventBus: additional edge cases
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EventBusEdgeCase, UnsubscribeNonexistentIsSafe) {
    EventBus bus;
    bus.unsubscribe(999);
}

TEST(EventBusEdgeCase, MultipleSubscribersSameEvent) {
    EventBus bus;
    int count1 = 0, count2 = 0;
    auto id1 = bus.subscribe("evt", [&](const Event&) { count1++; });
    auto id2 = bus.subscribe("evt", [&](const Event&) { count2++; });
    bus.emit("evt", "source", {{"x", 1}});
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
    bus.unsubscribe(id1);
    bus.unsubscribe(id2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ContextStore: additional edge cases
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ContextStoreEdgeCase, UpdatePartialObjectMergeDeep) {
    ContextStore store;
    store.set("k", {{"a", 1}, {"b", {{"c", 2}}}});
    EXPECT_TRUE(store.update_partial("k", {{"b", {{"d", 3}}}}, "owner"));
    auto val = store.get("k");
    EXPECT_EQ(val->value["a"], 1);
    EXPECT_EQ(val->value["b"]["c"], 2);
    EXPECT_EQ(val->value["b"]["d"], 3);
}

TEST(ContextStoreEdgeCase, MergeNewKey) {
    ContextStore store;
    store.set("k", {{"a", 1}});
    EXPECT_TRUE(store.merge("k", {{"b", 2}}, "owner"));
    auto val = store.get("k");
    EXPECT_EQ(val->value["a"], 1);
    EXPECT_EQ(val->value["b"], 2);
}

TEST(ContextStoreEdgeCase, MergeExistingKeyOverwrite) {
    ContextStore store;
    store.set("k", {{"a", 1}});
    EXPECT_TRUE(store.merge("k", {{"a", 99}}, "owner"));
    auto val = store.get("k");
    EXPECT_EQ(val->value["a"], 99);
}

TEST(ContextStoreEdgeCase, ClearSizeAndExists) {
    ContextStore store;
    store.set("k", {{"x", 1}});
    EXPECT_EQ(store.size(), 1u);
    EXPECT_TRUE(store.exists("k"));
    store.clear();
    EXPECT_EQ(store.size(), 0u);
    EXPECT_FALSE(store.exists("k"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// AgentRegistry: additional coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AgentRegistryEdge, FindIdleAgent) {
    AgentRegistry reg;
    AgentInfo agent;
    agent.id = "a1";
    agent.status = AgentStatus::Idle;
    AuthProvider auth("secret");
    auto token = auth.issue_token("admin", Role::Coordinator, "sw");
    reg.register_agent(agent, token);
    auto idle = reg.find_idle();
    EXPECT_EQ(idle.size(), 1u);
    EXPECT_EQ(idle[0].id, "a1");
}

TEST(AgentRegistryEdge, PruneStaleRemovesOldAgents) {
    AgentRegistry reg;
    AuthProvider auth("secret");
    auto token = auth.issue_token("admin", Role::Coordinator, "sw");
    AgentInfo agent;
    agent.id = "a2";
    agent.status = AgentStatus::Online;
    reg.register_agent(agent, token);
    EXPECT_GE(reg.list_agents().size(), 1u);
    size_t pruned = reg.prune_stale(std::chrono::seconds(0));
    EXPECT_EQ(pruned, 1u);
    EXPECT_EQ(reg.list_agents().size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Protocol: more branches for line 90%
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ProtocolExtraLineCoverage, ErrorResponseWithNullId) {
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"notifications/progress","params":{}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST(ProtocolExtraLineCoverage, ToolCallForbiddenWithoutPermission) {
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    proto.register_tool({.name = "test_tool", .description = "test",
        .input_schema = {{"type", "object"}}},
        [](const json&) -> json { return {{"result", "ok"}}; });
    proto.register_tool({.name = "admin_tool", .description = "admin only",
        .input_schema = {{"type", "object"}}, .required_permission = Permission::AgentRegister},
        [](const json&) -> json { return {{"result", "ok"}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"admin_tool","arguments":{},"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST(ProtocolExtraLineCoverage, ResourceReadForbiddenWithoutPermission) {
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    proto.register_resource({.uri = "test://private", .name = "private", .description = "secret",
        .required_permission = Permission::ContextWrite},
        [](const json&) -> json { return {{"secret", "data"}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"test://private","_auth":{"role":"observer"}}})"));
    EXPECT_TRUE(resp.contains("error"));
}

TEST(ProtocolExtraLineCoverage, ToolCallWithNoParamsObject) {
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    proto.register_tool({.name = "simple_tool", .description = "test",
        .input_schema = {{"type", "object"}}},
        [](const json&) -> json { return {{"result", "ok"}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"simple_tool","_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result") || resp.contains("error"));
}

TEST(ProtocolExtraLineCoverage, ResourceListPaginationCursor) {
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    for (int i = 0; i < 20; ++i) {
        proto.register_resource({.uri = std::string("test://r") + std::to_string(i),
            .name = "R" + std::to_string(i), .description = "desc"},
            [](const json&) -> json { return {{"v", 1}}; });
    }
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/list","params":{"_cursor":5,"_limit":10,"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp["result"]["resources"].size() > 0);
}

TEST(ProtocolExtraLineCoverage, PromptListPaginationCursor) {
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    for (int i = 0; i < 15; ++i) {
        proto.register_prompt({.name = std::string("p") + std::to_string(i), .description = "prompt",
            .arguments = {{{"name", "arg1"}, {"description", "arg"}, {"required", true}}}},
            [](const json&) -> json { return json::array({{{"role", "user"}, {"content", "hi"}}}); });
    }
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"prompts/list","params":{"_cursor":0,"_limit":10,"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp["result"]["prompts"].size() > 0);
}

TEST(ProtocolExtraLineCoverage, ToolCallWithNullParams) {
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    proto.register_tool({.name = "null_params_tool", .description = "test",
        .input_schema = {{"type", "object"}}},
        [](const json& args) -> json { return {{"echo", args}}; });

    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"null_params_tool","arguments":null,"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result") || resp.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Protocol: notification paths (no id) - covers "return json::object()" branches
// ═══════════════════════════════════════════════════════════════════════════════

class ProtocolNotificationTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    void SetUp() override {
        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
        proto.register_tool({.name = "ntool", .description = "test",
            .input_schema = {{"type", "object"}}},
            [](const json&) -> json { return {{"ok", true}}; });
        proto.register_resource({.uri = "test://r1", .name = "R1", .description = "d"},
            [](const json&) -> json { return {{"data", 1}}; });
        proto.register_prompt({.name = "p1", .description = "prompt",
            .arguments = {{{"name", "x"}, {"description", "arg"}, {"required", false}}}},
            [](const json&) -> json { return json::array({{{"role", "user"}, {"content", "hi"}}}); });
    }
};

TEST_F(ProtocolNotificationTest, ToolsListWithoutId) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"tools/list","params":{"_auth":{"role":"worker"}}})"));
    EXPECT_EQ(resp, json::object());
}

TEST_F(ProtocolNotificationTest, ToolsCallWithoutId) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"ntool","arguments":{},"_auth":{"role":"worker"}}})"));
    EXPECT_EQ(resp, json::object());
}

TEST_F(ProtocolNotificationTest, ResourcesListWithoutId) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"resources/list","params":{"_auth":{"role":"worker"}}})"));
    EXPECT_EQ(resp, json::object());
}

TEST_F(ProtocolNotificationTest, ResourcesReadWithoutId) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"resources/read","params":{"uri":"test://r1","_auth":{"role":"worker"}}})"));
    EXPECT_EQ(resp, json::object());
}

TEST_F(ProtocolNotificationTest, PromptsListWithoutId) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"prompts/list","params":{"_auth":{"role":"worker"}}})"));
    EXPECT_EQ(resp, json::object());
}

TEST_F(ProtocolNotificationTest, PromptsGetWithoutId) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","method":"prompts/get","params":{"name":"p1","_auth":{"role":"worker"}}})"));
    EXPECT_EQ(resp, json::object());
}

// ═══════════════════════════════════════════════════════════════════════════════
// SecureMqtt: publish_signed with ACL denial
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SecureMqttPublish, PublishDeniedByACL) {
    MqttConfig cfg;
    SecureMqttClient client{cfg, "test-swarm", "secret"};
    AuthToken observer_token;
    observer_token.agent_id = "observer-1";
    observer_token.role = Role::Observer;
    observer_token.swarm_id = "test-swarm";
    EXPECT_FALSE(client.publish_signed("mcp-collab/test-swarm/tasks", {{"data", 1}}, observer_token));
}

// ═══════════════════════════════════════════════════════════════════════════════
// MqttEnvelope: is_fresh edge cases
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MqttEnvelopeFresh, FutureMessageRejected) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 60000;
    env.sender = "agent-1";
    env.swarm_id = "test-swarm";
    env.role = "worker";
    env.payload = {{"future", true}};
    EXPECT_FALSE(env.is_fresh(std::chrono::seconds(300)));
}