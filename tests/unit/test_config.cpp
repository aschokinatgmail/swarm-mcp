#include <gtest/gtest.h>
#include "mcp_collab/config.hpp"
#include <fstream>
#include <filesystem>

using namespace mcp_collab;

class ConfigTest : public ::testing::Test {
protected:
    std::string test_config_path;

    void SetUp() override {
        test_config_path = std::filesystem::temp_directory_path().string() + "/swarm-mcp-test-config.json";
    }

    void TearDown() override {
        std::filesystem::remove(test_config_path);
    }

    void write_config(const std::string& content) {
        std::ofstream(test_config_path) << content;
    }
};

TEST_F(ConfigTest, DefaultConfig) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.server_name, "swarm-mcp");
    EXPECT_EQ(cfg.mqtt.host, "localhost");
    EXPECT_EQ(cfg.mqtt.port, 1883);
    EXPECT_EQ(cfg.http.host, "0.0.0.0");
    EXPECT_EQ(cfg.http.port, 3001);
    EXPECT_EQ(cfg.http.require_auth, true);
    EXPECT_EQ(cfg.http.endpoint, "/mcp");
    EXPECT_EQ(cfg.git.default_branch, "main");
    EXPECT_EQ(cfg.git.branch_prefix, "collab/");
}

TEST_F(ConfigTest, FromFile) {
    write_config(R"({
        "server_name": "my-server",
        "server_version": "2.0.0",
        "swarm": {
            "id": "team-alpha",
            "display_name": "Team Alpha",
            "secret": "my-secret-key",
            "open_enrollment": false,
            "token_ttl_seconds": 3600,
            "heartbeat_timeout_seconds": 60
        },
        "mqtt": {
            "host": "mqtt.example.com",
            "port": 8883,
            "username": "user1",
            "password": "pass1",
            "use_tls": true,
            "ca_cert_path": "/certs/ca.pem"
        },
        "http": {
            "host": "127.0.0.1",
            "port": 8080,
            "endpoint": "/api/mcp",
            "require_auth": false,
            "thread_pool_size": 8
        },
        "git": {
            "repo_path": "/repos/project",
            "default_branch": "develop",
            "branch_prefix": "feature/",
            "auto_commit": true
        }
    })");

    auto cfg = ServerConfig::from_file(test_config_path);
    EXPECT_EQ(cfg.server_name, "my-server");
    EXPECT_EQ(cfg.server_version, "2.0.0");
    EXPECT_EQ(cfg.swarm.id, "team-alpha");
    EXPECT_EQ(cfg.swarm.secret, "my-secret-key");
    EXPECT_EQ(cfg.swarm.open_enrollment, false);
    EXPECT_EQ(cfg.swarm.token_ttl.count(), 3600);
    EXPECT_EQ(cfg.mqtt.host, "mqtt.example.com");
    EXPECT_EQ(cfg.mqtt.port, 8883);
    EXPECT_EQ(cfg.mqtt.username, "user1");
    EXPECT_EQ(cfg.mqtt.use_tls, true);
    EXPECT_EQ(cfg.http.host, "127.0.0.1");
    EXPECT_EQ(cfg.http.port, 8080);
    EXPECT_EQ(cfg.http.require_auth, false);
    EXPECT_EQ(cfg.git.repo_path, "/repos/project");
    EXPECT_EQ(cfg.git.default_branch, "develop");
}

TEST_F(ConfigTest, FromNonexistentFile) {
    auto cfg = ServerConfig::from_file("/nonexistent/path/config.json");
    EXPECT_EQ(cfg.server_name, "swarm-mcp"); // defaults preserved
}

TEST_F(ConfigTest, FromFileInvalidJson) {
    write_config("{ not valid json }");
    auto cfg = ServerConfig::from_file(test_config_path);
    EXPECT_EQ(cfg.server_name, "swarm-mcp"); // defaults preserved
}

TEST_F(ConfigTest, FromFilePartialOverride) {
    write_config(R"({
        "server_name": "partial",
        "mqtt": {"host": "custom-mqtt"}
    })");
    auto cfg = ServerConfig::from_file(test_config_path);
    EXPECT_EQ(cfg.server_name, "partial");
    EXPECT_EQ(cfg.mqtt.host, "custom-mqtt");
    EXPECT_EQ(cfg.mqtt.port, 1883); // default preserved
}

TEST_F(ConfigTest, FromEnv) {
    // Set some env vars
    _putenv_s("SWARM_ID", "env-swarm");
    _putenv_s("SWARM_SECRET", "env-secret");
    _putenv_s("SWARM_HTTP_PORT", "9999");
    _putenv_s("SWARM_HTTP_AUTH", "true");
    _putenv_s("SWARM_TOKEN_TTL", "7200");

    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.swarm.id, "env-swarm");
    EXPECT_EQ(cfg.swarm.secret, "env-secret");
    EXPECT_EQ(cfg.http.port, 9999);
    EXPECT_EQ(cfg.http.require_auth, true);
    EXPECT_EQ(cfg.swarm.token_ttl.count(), 7200);

    _putenv_s("SWARM_ID", "");
    _putenv_s("SWARM_SECRET", "");
    _putenv_s("SWARM_HTTP_PORT", "");
    _putenv_s("SWARM_HTTP_AUTH", "");
    _putenv_s("SWARM_TOKEN_TTL", "");
}