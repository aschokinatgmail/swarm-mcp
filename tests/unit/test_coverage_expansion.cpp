#include <gtest/gtest.h>
#include "mcp_collab/collab_defs.hpp"
#include "mcp_collab/auth.hpp"
#include "mcp_collab/task_manager.hpp"
#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/context_store.hpp"
#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/persistence.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/mqtt_client.hpp"
#include "mcp_collab/secure_mqtt.hpp"
#include "mcp_collab/channel.hpp"
#include "mcp_collab/config.hpp"
#include "mcp_collab/keychain.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>

using namespace mcp_collab;

#ifdef _WIN32
inline void set_env(const char* name, const char* value) { _putenv_s(name, value); }
inline void unset_env(const char* name) { _putenv_s(name, ""); }
#else
inline void set_env(const char* name, const char* value) { setenv(name, value, 1); }
inline void unset_env(const char* name) { unsetenv(name); }
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// Config: env var override branches
// ═══════════════════════════════════════════════════════════════════════════════

class ConfigEnvTest : public ::testing::Test {
protected:
    void TearDown() override {
        unset_env("SWARM_SERVER_NAME");
        unset_env("SWARM_ID");
        unset_env("SWARM_SECRET");
        unset_env("SWARM_MQTT_HOST");
        unset_env("SWARM_MQTT_PORT");
        unset_env("SWARM_MQTT_USERNAME");
        unset_env("SWARM_MQTT_PASSWORD");
        unset_env("SWARM_HTTP_HOST");
        unset_env("SWARM_HTTP_PORT");
        unset_env("SWARM_HTTP_ENDPOINT");
        unset_env("SWARM_HTTP_AUTH");
        unset_env("SWARM_GIT_REPO_PATH");
        unset_env("SWARM_TOKEN_TTL");
        unset_env("SWARM_HEARTBEAT_TIMEOUT");
    }
};

TEST_F(ConfigEnvTest, ServerName) {
    set_env("SWARM_SERVER_NAME", "env-server");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.server_name, "env-server");
}

TEST_F(ConfigEnvTest, SwarmIdAndSecret) {
    set_env("SWARM_ID", "my-swarm");
    set_env("SWARM_SECRET", "super-secret");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.swarm.id, "my-swarm");
    EXPECT_EQ(cfg.swarm.secret, "super-secret");
}

TEST_F(ConfigEnvTest, MqttHostAndPort) {
    set_env("SWARM_MQTT_HOST", "mqtt.example.com");
    set_env("SWARM_MQTT_PORT", "9883");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.mqtt.host, "mqtt.example.com");
    EXPECT_EQ(cfg.mqtt.port, 9883);
}

TEST_F(ConfigEnvTest, MqttCredentials) {
    set_env("SWARM_MQTT_USERNAME", "user1");
    set_env("SWARM_MQTT_PASSWORD", "pass1");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.mqtt.username, "user1");
    EXPECT_EQ(cfg.mqtt.password, "pass1");
}

TEST_F(ConfigEnvTest, HttpOverrides) {
    set_env("SWARM_HTTP_HOST", "0.0.0.0");
    set_env("SWARM_HTTP_PORT", "9090");
    set_env("SWARM_HTTP_ENDPOINT", "/api");
    set_env("SWARM_HTTP_AUTH", "true");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.http.host, "0.0.0.0");
    EXPECT_EQ(cfg.http.port, 9090);
    EXPECT_EQ(cfg.http.endpoint, "/api");
    EXPECT_TRUE(cfg.http.require_auth);
}

TEST_F(ConfigEnvTest, HttpAuthFalse) {
    set_env("SWARM_HTTP_AUTH", "0");
    auto cfg = ServerConfig::from_env();
    EXPECT_FALSE(cfg.http.require_auth);
}

TEST_F(ConfigEnvTest, GitRepoPath) {
    set_env("SWARM_GIT_REPO_PATH", "/tmp/my-repo");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.git.repo_path, "/tmp/my-repo");
}

TEST_F(ConfigEnvTest, InvalidMqttPort) {
    set_env("SWARM_MQTT_PORT", "not_a_number");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.mqtt.port, 1883);
}

TEST_F(ConfigEnvTest, InvalidHttpPort) {
    set_env("SWARM_HTTP_PORT", "abc");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.http.port, 3001);
}

TEST_F(ConfigEnvTest, InvalidTokenTtl) {
    set_env("SWARM_TOKEN_TTL", "invalid");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.swarm.token_ttl.count(), 86400);
}

TEST_F(ConfigEnvTest, InvalidHeartbeatTimeout) {
    set_env("SWARM_HEARTBEAT_TIMEOUT", "not_a_number");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.swarm.heartbeat_timeout.count(), 120);
}

TEST_F(ConfigEnvTest, ValidTokenTtl) {
    set_env("SWARM_TOKEN_TTL", "7200");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.swarm.token_ttl.count(), 7200);
}

TEST_F(ConfigEnvTest, ValidHeartbeatTimeout) {
    set_env("SWARM_HEARTBEAT_TIMEOUT", "600");
    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.swarm.heartbeat_timeout.count(), 600);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Config: type error in JSON file
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ConfigFromFile, TypeErrorsCaught) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm-mcp-cfg-type";
    std::filesystem::create_directories(test_dir);
    std::string config_path = test_dir + "/config.json";
    {
        std::ofstream f(config_path);
        f << R"({"mqtt": {"port": "not_a_number"}, "http": {"port": "also_not"}})";
    }
    auto cfg = ServerConfig::from_file(config_path);
    EXPECT_EQ(cfg.mqtt.port, 1883);
    EXPECT_EQ(cfg.http.port, 3001);
    std::filesystem::remove_all(test_dir);
}

// ═══════════════════════════════════════════════════════════════════════════════
// EventBus: exception handling in handlers
// ═══════════════════════════════════════════════════════════════════════════════

TEST(EventBusException, HandlerExceptionDoesNotCrash) {
    EventBus bus;
    bus.subscribe("error.event", [](const Event&) {
        throw std::runtime_error("test exception");
    });
    bus.emit("error.event", "src");
}

TEST(EventBusException, WildcardHandlerExceptionCaught) {
    EventBus bus;
    bus.subscribe("*", [](const Event&) {
        throw std::runtime_error("wildcard error");
    });
    bus.emit("any.event", "src");
}

TEST(EventBusException, ExceptionDoesNotAffectOtherHandlers) {
    EventBus bus;
    bus.subscribe("test.event", [](const Event&) {
        throw std::runtime_error("bad handler");
    });
    int good_count = 0;
    bus.subscribe("test.event", [&](const Event&) {
        good_count++;
    });
    bus.emit("test.event", "src");
    EXPECT_EQ(good_count, 1);
}

TEST(EventBusException, WildcardAndSpecificHandlerBothFire) {
    EventBus bus;
    int specific_count = 0;
    int wildcard_count = 0;
    bus.subscribe("my.event", [&](const Event&) { specific_count++; });
    bus.subscribe("*", [&](const Event&) { wildcard_count++; });
    bus.emit("my.event", "src");
    EXPECT_EQ(specific_count, 1);
    EXPECT_EQ(wildcard_count, 1);
}

TEST(EventBusException, ExceptionInWildcardDoesNotCrashOtherHandlers) {
    EventBus bus;
    bus.subscribe("*", [](const Event&) {
        throw std::runtime_error("wildcard exception");
    });
    int count = 0;
    bus.subscribe("safe.event", [&](const Event&) { count++; });
    bus.emit("safe.event", "src");
    EXPECT_GE(count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ContextStore: merge scalar overwrite, update_partial object into non-object
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ContextStoreMerge, MergeObjectIntoExistingObject) {
    ContextStore store;
    store.set("k", {{"a", 1}, {"b", 2}});
    store.merge("k", {{"b", 20}, {"c", 30}}, "owner");
    auto entry = store.get("k");
    EXPECT_EQ(entry->value["a"], 1);
    EXPECT_EQ(entry->value["b"], 20);
    EXPECT_EQ(entry->value["c"], 30);
}

TEST(ContextStoreMerge, MergeArrayAppend) {
    ContextStore store;
    store.set("arr", json::array({1, 2, 3}));
    store.merge("arr", json::array({4, 5}), "owner");
    auto entry = store.get("arr");
    EXPECT_EQ(entry->value.size(), 5u);
}

TEST(ContextStoreMerge, MergeScalarOverwritesObject) {
    ContextStore store;
    store.set("k", {{"a", 1}});
    store.merge("k", "scalar_value", "owner");
    auto entry = store.get("k");
    EXPECT_EQ(entry->value, "scalar_value");
}

TEST(ContextStoreMerge, MergeIntoNewKeyCreatesEntry) {
    ContextStore store;
    store.merge("new_key", {{"x", 1}}, "owner");
    auto entry = store.get("new_key");
    EXPECT_TRUE(entry.has_value());
    EXPECT_EQ(entry->value["x"], 1);
}

TEST(ContextStoreMerge, MergeCallbackMerged) {
    ContextStore store;
    std::string action;
    store.on_change([&](const std::string&, const ContextEntry&, const std::string& a) {
        action = a;
    });
    store.set("k1", 1);
    EXPECT_EQ(action, "created");
    store.merge("k1", "replaced");
    EXPECT_EQ(action, "merged");
}

TEST(ContextStoreMerge, UpdatePartialObjectPatch) {
    ContextStore store;
    store.set("k", {{"a", 1}, {"b", 2}});
    store.update_partial("k", {{"b", 20}, {"c", 30}}, "owner");
    auto entry = store.get("k");
    EXPECT_EQ(entry->value["a"], 1);
    EXPECT_EQ(entry->value["b"], 20);
    EXPECT_EQ(entry->value["c"], 30);
}

TEST(ContextStoreMerge, UpdatePartialNonObjectPatchIntoObject) {
    ContextStore store;
    store.set("k", {{"a", 1}});
    bool result = store.update_partial("k", "scalar", "owner");
    EXPECT_TRUE(result);
    auto entry = store.get("k");
    EXPECT_EQ(entry->value, "scalar");
}

TEST(ContextStoreMerge, UpdatePartialScalarReplace) {
    ContextStore store;
    store.set("k", "scalar");
    store.update_partial("k", 42, "owner");
    auto entry = store.get("k");
    EXPECT_EQ(entry->value, 42);
}

TEST(ContextStoreMerge, DeleteTriggersCallback) {
    ContextStore store;
    std::string action;
    store.on_change([&](const std::string& key, const ContextEntry&, const std::string& a) {
        action = a;
    });
    store.set("k1", 42);
    store.del("k1");
    EXPECT_EQ(action, "deleted");
}

// ═══════════════════════════════════════════════════════════════════════════════
// SecureMqtt: on_raw_message branches
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SecureMqttOnRaw, DevModeParsesUnsignedJson) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18841}, "dev-swarm", "");
    MqttEnvelope env;
    env.sender = "agent-1";
    env.swarm_id = "dev-swarm";
    env.role = "worker";
    env.payload = {{"action", "test"}};
    json j = {{"sender", "agent-1"}, {"swarm", "dev-swarm"}, {"role", "worker"},
              {"payload", {{"action", "test"}}}, {"timestamp", 0}};
    MqttMessage msg;
    msg.topic = "mcp-collab/dev-swarm/tasks";
    msg.payload = j.dump();
}

TEST(SecureMqttOnRaw, CrossSwarmRejection) {
    auto env = MqttEnvelope::sign("sender", "other-swarm", Role::Worker, {{"data", 1}}, "secret123");
    json j = env.to_json();
    auto verified = MqttEnvelope::verify(j.dump(), "my-swarm-secret");
    EXPECT_FALSE(verified.has_value());
}

TEST(SecureMqttOnRaw, StaleMessageIsNotFresh) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 600000;
    EXPECT_FALSE(env.is_fresh(std::chrono::seconds(300)));
}

TEST(SecureMqttOnRaw, FutureMessageIsNotFresh) {
    MqttEnvelope env;
    env.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 600000;
    EXPECT_FALSE(env.is_fresh(std::chrono::seconds(300)));
}

TEST(SecureMqttOnRaw, VerifyInvalidJsonReturnsNullopt) {
    auto result = MqttEnvelope::verify("not valid json", "secret");
    EXPECT_FALSE(result.has_value());
}

TEST(SecureMqttOnRaw, VerifyWrongVersionReturnsNullopt) {
    json j = {{"envelope", "unknown-version"}, {"sender", "a"}, {"swarm", "s"},
              {"role", "worker"}, {"timestamp", 0}, {"payload", json::object()}, {"signature", "sig"}};
    auto result = MqttEnvelope::verify(j.dump(), "secret");
    EXPECT_FALSE(result.has_value());
}

TEST(SecureMqttOnRaw, TopicAuthDefaultPrefix) {
    MqttTopicAuth acl("default");
    EXPECT_TRUE(acl.can_publish(Role::Worker, "mcp-collab/default/tasks/new"));
    EXPECT_TRUE(acl.can_subscribe(Role::Worker, "mcp-collab/default/tasks/#"));
}

TEST(SecureMqttOnRaw, TopicAuthCoordinatorCanPublishAnywhere) {
    MqttTopicAuth acl("swarm-1");
    EXPECT_TRUE(acl.can_publish(Role::Coordinator, "mcp-collab/swarm-1/admin/roles"));
    EXPECT_TRUE(acl.can_subscribe(Role::Coordinator, "mcp-collab/swarm-1/#"));
}

TEST(SecureMqttOnRaw, TopicAuthWorkerCannotPublishToAdmin) {
    MqttTopicAuth acl("swarm-1");
    EXPECT_FALSE(acl.can_publish(Role::Worker, "mcp-collab/swarm-1/admin/roles"));
}

TEST(SecureMqttOnRaw, TopicAuthUnknownTopicDenied) {
    MqttTopicAuth acl("swarm-1");
    EXPECT_FALSE(acl.can_publish(Role::Worker, "mcp-collab/swarm-1/unknown/topic"));
    EXPECT_FALSE(acl.can_subscribe(Role::Observer, "mcp-collab/swarm-1/unknown/topic"));
}

TEST(SecureMqttOnRaw, SecureMqttSetMaxAge) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18842}, "swarm", "secret");
    client.set_max_message_age(std::chrono::seconds(120));
}

TEST(SecureMqttOnRaw, PublishSignedWithTokenSuccessDenied) {
    SecureMqttClient client(MqttConfig{.host = "localhost", .port = 18843}, "swarm", "secret");
    AuthToken token;
    token.role = Role::Observer;
    token.agent_id = "obs1";
    token.swarm_id = "swarm";
    EXPECT_FALSE(client.publish_signed("mcp-collab/swarm/tasks/new", {{"data", 1}}, token));
}

TEST(SecureMqttOnRaw, MqttEnvelopeRoundTrip) {
    auto env = MqttEnvelope::sign("agent-1", "swarm-1", Role::Worker, {{"task", "build"}}, "my-secret");
    auto j = env.to_json();
    std::string raw = j.dump();
    auto verified = MqttEnvelope::verify(raw, "my-secret");
    EXPECT_TRUE(verified.has_value());
    EXPECT_EQ(verified->sender, "agent-1");
    EXPECT_EQ(verified->swarm_id, "swarm-1");
    EXPECT_EQ(verified->role, "worker");
}

// ═══════════════════════════════════════════════════════════════════════════════
// AgentRegistry: extra branches
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AgentRegistryExtra, RegisterWithModelFromMetadata) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");

    AgentInfo info;
    info.name = "ModelAgent";
    info.metadata = {{"model", {{"provider", "openai"}, {"model_id", "gpt-4"}}},
                     {"environment", {{"runtime", "python"}, {"os", "linux"}}}};
    auto id = reg.register_agent(info, token);
    auto agent = reg.get_agent(id);
    EXPECT_EQ(agent->model.provider, "openai");
    EXPECT_EQ(agent->model.model_id, "gpt-4");
    EXPECT_EQ(agent->environment.runtime, "python");
}

TEST(AgentRegistryExtra, UpdateWithEmptyFieldsKeepsExisting) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto coord = auth.issue_token("c1", Role::Coordinator, "swarm-1");

    AgentInfo info;
    info.name = "Original";
    info.capabilities = {"python"};
    auto id = reg.register_agent(info, coord);

    AgentInfo update;
    update.name = "";
    update.capabilities = {};
    update.metadata = json::object();
    EXPECT_TRUE(reg.update_agent(id, update, coord));
    auto agent = reg.get_agent(id);
    EXPECT_EQ(agent->name, "Original");
}

TEST(AgentRegistryExtra, RegisterWithEmptyIdUsesTokenId) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("token-agent-1", Role::Worker, "swarm-1");

    AgentInfo info;
    info.name = "NoIdAgent";
    auto id = reg.register_agent(info, token);
    EXPECT_EQ(id, "token-agent-1");
}

TEST(AgentRegistryExtra, PruneStaleRemovesOld) {
    AgentRegistry reg("swarm-1");
    AuthProvider auth("secret");
    auto token = auth.issue_token("a1", Role::Worker, "swarm-1");

    AgentInfo info;
    info.name = "Old";
    auto id = reg.register_agent(info, token);

    size_t pruned = reg.prune_stale(std::chrono::seconds(0));
    EXPECT_EQ(pruned, 1u);
    EXPECT_FALSE(reg.get_agent(id).has_value());
}

TEST(AgentRegistryExtra, AgentStatusFromJsonCoversAll) {
    json j = {{"id", "x"}, {"name", "x"}, {"status", "busy"}};
    auto a = AgentInfo::from_json(j);
    EXPECT_EQ(a.status, AgentStatus::Busy);

    json j2 = {{"id", "x"}, {"name", "x"}, {"status", "idle"}};
    auto a2 = AgentInfo::from_json(j2);
    EXPECT_EQ(a2.status, AgentStatus::Idle);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Persistence: additional coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(PersistenceExtra, SaveIfChangedSkipsWhenSame) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_pers_skip";
    std::string test_path = test_dir + "/snapshot.json";
    std::filesystem::create_directories(test_dir);
    json snap = PersistenceLayer::create_snapshot(
        json::array(), json::object(), json::object(), json::array(), json::array());
    PersistenceLayer pl(test_path);
    EXPECT_TRUE(pl.save(snap));
    EXPECT_TRUE(pl.save_if_changed(snap));
    std::filesystem::remove_all(test_dir);
}

TEST(PersistenceExtra, SaveIfChangedSavesWhenDifferent) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_pers_diff";
    std::string test_path = test_dir + "/snapshot.json";
    std::filesystem::create_directories(test_dir);
    json snap1 = PersistenceLayer::create_snapshot(
        json::array(), json::object(), json::object(), json::array(), json::array());
    PersistenceLayer pl(test_path);
    EXPECT_TRUE(pl.save(snap1));
    json snap2 = PersistenceLayer::create_snapshot(
        json::array({{"id", "t1"}}), json::object(), json::object(), json::array(), json::array());
    EXPECT_TRUE(pl.save_if_changed(snap2));
    std::filesystem::remove_all(test_dir);
}

TEST(PersistenceExtra, ExistsAndClear) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_pers_exist";
    std::string test_path = test_dir + "/snapshot.json";
    std::filesystem::create_directories(test_dir);
    json snap = PersistenceLayer::create_snapshot(
        json::array(), json::object(), json::object(), json::array(), json::array());
    PersistenceLayer pl(test_path);
    EXPECT_TRUE(pl.save(snap));
    EXPECT_TRUE(pl.exists());
    EXPECT_TRUE(pl.clear());
    EXPECT_FALSE(pl.exists());
    std::filesystem::remove_all(test_dir);
}

TEST(PersistenceExtra, SetAutoSaveIntervalAndDisable) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_pers_auto";
    std::string test_path = test_dir + "/snapshot.json";
    std::filesystem::create_directories(test_dir);
    PersistenceLayer pl(test_path);
    pl.set_auto_save_interval(std::chrono::seconds(1));
    pl.enable_auto_save([]() { return json::object(); });
    pl.disable_auto_save();
    std::filesystem::remove_all(test_dir);
}

TEST(PersistenceExtra, LoadCorruptJson) {
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_pers_corrupt";
    std::string test_path = test_dir + "/snapshot.json";
    std::filesystem::create_directories(test_dir);
    { std::ofstream f(test_path); f << "{corrupt json!!!"; }
    PersistenceLayer pl(test_path);
    EXPECT_FALSE(pl.load().has_value());
    std::filesystem::remove_all(test_dir);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Protocol: additional branch coverage
// ═══════════════════════════════════════════════════════════════════════════════

class ProtocolMethodTest : public ::testing::Test {
protected:
    McpProtocol proto{ServerInfo{.name = "test", .version = "1.0"}};
    void SetUp() override {
        proto.handle_request(json::parse(R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{}})"));
    }
};

TEST_F(ProtocolMethodTest, InitializeWithAuthCapabilities) {
    McpProtocol p2(ServerInfo{.name = "caps", .version = "2.0"},
        ServerCapabilities{.tools = true, .resources = true, .prompts = true, .logging = true});
    auto resp = p2.handle_request(json::parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})"));
    EXPECT_TRUE(resp["result"]["capabilities"].contains("tools"));
    EXPECT_TRUE(resp["result"]["capabilities"].contains("resources"));
    EXPECT_TRUE(resp["result"]["capabilities"].contains("prompts"));
    EXPECT_TRUE(resp["result"]["capabilities"].contains("logging"));
}

TEST_F(ProtocolMethodTest, ToolCallWithException) {
    proto.register_tool({.name = "fail_tool", .description = "fails", .input_schema = {},
        .required_permission = Permission::TaskRead},
        [](const json&) -> json { throw std::runtime_error("intentional error"); });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"fail_tool","arguments":{},"_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"]["isError"].get<bool>());
}

TEST_F(ProtocolMethodTest, ResourcesReadWithUri) {
    proto.register_resource({.uri = "test://data", .name = "Data", .description = "desc",
        .required_permission = Permission::TaskRead},
        [](const json&) -> json { return {{"v", 1}}; });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":"test://data","_auth":{"role":"worker"}}})"));
    EXPECT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"].contains("contents"));
}

TEST_F(ProtocolMethodTest, ResourcesListPaginationCursor) {
    for (int i = 0; i < 5; ++i) {
        std::string uri = std::string("test://r") + std::to_string(i);
        proto.register_resource({.uri = uri, .name = "R" + std::to_string(i),
            .description = "desc"}, [](const json&) -> json { return {{"v", 1}}; });
    }
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"resources/list","params":{"_cursor":0,"_limit":3,"_auth":{"role":"worker"}}})"));
    EXPECT_EQ(resp["result"]["resources"].size(), 3u);
}

TEST_F(ProtocolMethodTest, PromptsListWithArguments) {
    proto.register_prompt({.name = "test_prompt", .description = "test",
        .arguments = {{{"name", "arg1"}, {"description", "First arg"}, {"required", true}}}},
        [](const json&) -> json { return json::array({{{"role", "user"}, {"content", "hi"}}}); });
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"prompts/list","params":{}})"));
    EXPECT_TRUE(resp["result"]["prompts"][0].contains("arguments"));
}

TEST_F(ProtocolMethodTest, PromptsGetNonexistent) {
    auto resp = proto.handle_request(json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"prompts/get","params":{"name":"nonexistent_prompt"}})"));
    EXPECT_TRUE(resp.contains("error"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Auth: additional branches
// ═══════════════════════════════════════════════════════════════════════════════

TEST(AuthBranchExtra, ExtractBearerWithTrailingWhitespace) {
    auto val = AuthProvider::extract_bearer("Bearer token123  ");
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(val.value(), "token123");
}

TEST(AuthBranchExtra, ExtractBearerNonBearerPrefix) {
    EXPECT_FALSE(AuthProvider::extract_bearer("Basic dXNlcjpwYXNz").has_value());
    EXPECT_FALSE(AuthProvider::extract_bearer("").has_value());
    EXPECT_FALSE(AuthProvider::extract_bearer("Bear token123").has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// RateLimiter: edge cases
// ═══════════════════════════════════════════════════════════════════════════════

TEST(RateLimiterEdge, WindowResetAfterTimeout) {
    RateLimiter limiter(1);
    EXPECT_TRUE(limiter.allow("key"));
    EXPECT_FALSE(limiter.allow("key"));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Keychain: additional coverage
// ═══════════════════════════════════════════════════════════════════════════════

TEST(KeychainExtra, StoreAndDelete) {
    std::string key = "test-delete-key-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    EXPECT_TRUE(keychain::store_secret("swarm-mcp-test", key, "value"));
    auto retrieved = keychain::get_secret("swarm-mcp-test", key);
    if (retrieved.has_value()) {
        EXPECT_EQ(*retrieved, "value");
        EXPECT_TRUE(keychain::delete_secret("swarm-mcp-test", key));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TaskManager: extra dependency and status transitions
// ═══════════════════════════════════════════════════════════════════════════════

TEST(TaskManagerExtra, RemoveExistingDependency) {
    TaskManager tm;
    auto t1 = tm.create_task("T1", "a1");
    auto t2 = tm.create_task("T2", "a1");
    tm.add_dependency(t2.id, t1.id);
    EXPECT_TRUE(tm.has_dependency(t2.id, t1.id));
    EXPECT_TRUE(tm.remove_dependency(t2.id, t1.id));
    EXPECT_FALSE(tm.has_dependency(t2.id, t1.id));
}

TEST(TaskManagerExtra, SetStatusFailedAndCancelled) {
    TaskManager tm;
    auto t = tm.create_task("T", "a1");
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::Failed));
    EXPECT_EQ(tm.get_task(t.id)->status, TaskStatus::Failed);
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::Cancelled));
    EXPECT_EQ(tm.get_task(t.id)->status, TaskStatus::Cancelled);
}

TEST(TaskManagerExtra, SetStatusBlockedAndCompleted) {
    TaskManager tm;
    auto t = tm.create_task("T", "a1");
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::Blocked));
    EXPECT_EQ(tm.get_task(t.id)->status, TaskStatus::Blocked);
    EXPECT_TRUE(tm.set_status(t.id, TaskStatus::Completed));
    EXPECT_TRUE(tm.get_task(t.id)->completed_at.has_value());
}

TEST(TaskManagerExtra, ListTasksNoFilters) {
    TaskManager tm;
    tm.create_task("T1", "a1");
    tm.create_task("T2", "a2");
    auto result = tm.list_tasks(TaskFilter{});
    EXPECT_EQ(result.size(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Handler: additional branches
// ═══════════════════════════════════════════════════════════════════════════════

class HandlerMethodTest : public ::testing::Test {
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

    httplib::Request make_request(const std::string& method, const std::string& path,
                                   const std::string& body = "",
                                   const std::string& auth_header = "") {
        httplib::Request req;
        req.method = method;
        req.path = path;
        req.target = path;
        if (!body.empty()) req.body = body;
        if (!auth_header.empty()) req.set_header("Authorization", auth_header);
        return req;
    }
};

TEST_F(HandlerMethodTest, PostValidPing) {
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})");
    httplib::Response res;
    transport->handle_post(req, res);
    auto j = json::parse(res.body);
    EXPECT_TRUE(j.contains("result"));
}

TEST_F(HandlerMethodTest, PostNotificationReturns202) {
    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    httplib::Response res;
    transport->handle_post(req, res);
    EXPECT_EQ(res.status, 202);
}

TEST_F(HandlerMethodTest, PostWithBearerTokenAuthEnabled) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = true});
    auto token = auth_.issue_token("agent-1", Role::Worker, "swarm");

    httplib::Request req = make_request("POST", "/mcp",
        R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "Bearer " + token.token_string);
    httplib::Response res;
    auth_transport.handle_post(req, res);
    EXPECT_NE(res.status, 401);
}

TEST_F(HandlerMethodTest, GetRequiresSSEHeader) {
    StreamableHttpTransport auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    httplib::Request req;
    req.method = "GET";
    req.path = "/mcp";
    req.target = "/mcp";
    req.set_header("Accept", "application/json");
    httplib::Response res;
    auth_transport.handle_get(req, res);
    EXPECT_EQ(res.status, 400);
}

TEST_F(HandlerMethodTest, AuthDisabledGetNoAuth) {
    StreamableHttpTransport no_auth_transport(proto, auth_,
        StreamableHttpConfig{.host = "127.0.0.1", .port = 0, .endpoint = "/mcp",
                             .require_auth = false});
    httplib::Request req;
    req.method = "GET";
    req.path = "/mcp";
    req.target = "/mcp";
    req.set_header("Accept", "text/event-stream");
    httplib::Response res;
    no_auth_transport.handle_get(req, res);
}

TEST_F(HandlerMethodTest, DeleteWithoutSessionId) {
    httplib::Request req = make_request("DELETE", "/mcp");
    httplib::Response res;
    transport->handle_delete(req, res);
    EXPECT_EQ(res.status, 204);
}

TEST_F(HandlerMethodTest, DeleteWithSessionId) {
    httplib::Request req = make_request("DELETE", "/mcp");
    req.set_header("Mcp-Session-Id", "session-123");
    httplib::Response res;
    transport->handle_delete(req, res);
    EXPECT_EQ(res.status, 204);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MqttClient: additional topic matching
// ═══════════════════════════════════════════════════════════════════════════════

TEST(MqttTopicMatching, SingleLevelWildcardMultipleLevels) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("a/+/c/+", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("a/b/c/d", "msg");
    EXPECT_EQ(count, 1);
    client.inject_message("a/b/c/d/e", "msg");
    EXPECT_EQ(count, 1);
}

TEST(MqttTopicMatching, MultiLevelWildcardAtEnd) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("sensor/data/#", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("sensor/data/temp", "msg1");
    EXPECT_EQ(count, 1);
    client.inject_message("sensor/data/temp/room1", "msg2");
    EXPECT_EQ(count, 2);
    client.inject_message("sensor/temp", "msg3");
    EXPECT_EQ(count, 2);
}

TEST(MqttTopicMatching, ExactMatchNoWildcard) {
    MqttClient client(MqttConfig{});
    int count = 0;
    client.subscribe("exact/topic", 1, [&](const MqttMessage&) { count++; });
    client.inject_message("exact/topic", "msg1");
    EXPECT_EQ(count, 1);
    client.inject_message("exact/topic/extra", "msg2");
    EXPECT_EQ(count, 1);
    client.inject_message("other/topic", "msg3");
    EXPECT_EQ(count, 1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SseStream client management
// ═══════════════════════════════════════════════════════════════════════════════

TEST(SseStreamManage, AddRemoveMultipleClients) {
    SseStream sse;
    std::vector<SseStream::ClientId> ids;
    for (int i = 0; i < 5; ++i) {
        ids.push_back(sse.add_client([i](const std::string& data) { return true; }));
    }
    EXPECT_EQ(sse.client_count(), 5u);
    for (const auto& id : ids) {
        sse.remove_client(id);
    }
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST(SseStreamManage, BroadcastToMultiple) {
    SseStream sse;
    int count1 = 0, count2 = 0;
    sse.add_client([&](const std::string&) { count1++; return true; });
    sse.add_client([&](const std::string&) { count2++; return true; });
    sse.broadcast("event", {{"key", "value"}});
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
}