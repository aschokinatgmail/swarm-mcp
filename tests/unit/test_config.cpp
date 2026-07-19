#include <gtest/gtest.h>
#include "mcp_collab/config.hpp"
#include <fstream>
#include <filesystem>

using namespace mcp_collab;

// Portable environment variable wrapper
#ifdef _WIN32
    #include <stdlib.h>
    inline void set_env(const char* name, const char* value) {
        _putenv_s(name, value);
    }
    inline void unset_env(const char* name) {
        _putenv_s(name, "");
    }
#else
    #include <stdlib.h>
    inline void set_env(const char* name, const char* value) {
        setenv(name, value, 1);
    }
    inline void unset_env(const char* name) {
        unsetenv(name);
    }
#endif

class ConfigTest : public ::testing::Test {
protected:
    std::string test_config_path;

    void SetUp() override {
        test_config_path = (std::filesystem::temp_directory_path() / ("swarm-mcp-test-config-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".json")).string();
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
    EXPECT_EQ(cfg.http.host, "127.0.0.1");
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
    // Non-existent file under temp directory (allowed root) → returns defaults
    auto cfg = ServerConfig::from_file((std::filesystem::temp_directory_path() / "nonexistent-config.json").string());
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
    set_env("SWARM_ID", "env-swarm");
    set_env("SWARM_SECRET", "env-secret");
    set_env("SWARM_HTTP_PORT", "9999");
    set_env("SWARM_HTTP_AUTH", "true");
    set_env("SWARM_TOKEN_TTL", "7200");

    auto cfg = ServerConfig::from_env();
    EXPECT_EQ(cfg.swarm.id, "env-swarm");
    EXPECT_EQ(cfg.swarm.secret, "env-secret");
    EXPECT_EQ(cfg.http.port, 9999);
    EXPECT_EQ(cfg.http.require_auth, true);
    EXPECT_EQ(cfg.swarm.token_ttl.count(), 7200);

    unset_env("SWARM_ID");
    unset_env("SWARM_SECRET");
    unset_env("SWARM_HTTP_PORT");
    unset_env("SWARM_HTTP_AUTH");
    unset_env("SWARM_TOKEN_TTL");
}

// ═══════════════════════════════════════════════════════════════════════════════
// Path traversal regression tests (CWE-23)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(ConfigTest, PathTraversalDotDotRejected) {
    // Raw ".." segments are rejected before canonicalization
    EXPECT_THROW(ServerConfig::from_file("../../../etc/passwd"), std::invalid_argument);
    EXPECT_THROW(ServerConfig::from_file("../../etc/passwd"), std::invalid_argument);
    EXPECT_THROW(ServerConfig::from_file("../config/default.json"), std::invalid_argument);
}

TEST_F(ConfigTest, AbsolutePathOutsideAllowlistRejected) {
    // Absolute paths to system files are rejected
    EXPECT_THROW(ServerConfig::from_file("/etc/passwd"), std::invalid_argument);
    EXPECT_THROW(ServerConfig::from_file("/proc/self/environ"), std::invalid_argument);
    EXPECT_THROW(ServerConfig::from_file("/tmp/../etc/passwd"), std::invalid_argument);
}

TEST_F(ConfigTest, PathWithDotDotResolvedInsideAllowedRootRejected) {
    // Path containing ".." segments is rejected even if it resolves inside an allowed root
    // (raw ".." check is defense in depth before canonicalization)
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / ("swarm-mcp-path-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(test_dir);
    std::string config_path = (test_dir / "config.json").string();
    std::ofstream(config_path) << R"({"server_name": "test"})";

    // Path with ".." segment is rejected even though it resolves to the same file
    std::string path_with_dotdot = (test_dir / "subdir" / ".." / "config.json").string();
    EXPECT_THROW(ServerConfig::from_file(path_with_dotdot), std::invalid_argument);

    std::filesystem::remove_all(test_dir);
}

TEST_F(ConfigTest, SymlinkOutsideAllowlistRejected) {
    // Symlink pointing outside the allowlist is rejected (canonicalization resolves symlinks)
    std::filesystem::path test_dir = std::filesystem::temp_directory_path() / ("swarm-mcp-symlink-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(test_dir);

    // Create a symlink to /etc/passwd. On Windows, symlink creation requires
    // admin/developer mode and may fail with an error — skip the test in that
    // case rather than reporting a false failure.
    std::filesystem::path symlink_path = test_dir / "config.json";
    std::error_code symlink_ec;
    std::filesystem::create_symlink("/etc/passwd", symlink_path, symlink_ec);
    if (symlink_ec) {
        std::filesystem::remove_all(test_dir);
        GTEST_SKIP() << "Symlinks not supported on this platform";
    }

    // Symlink resolves to /etc/passwd which is outside the allowlist
    EXPECT_THROW(ServerConfig::from_file(symlink_path.string()), std::invalid_argument);

    std::filesystem::remove_all(test_dir);
}

TEST_F(ConfigTest, ValidPathsUnderAllowlistAccepted) {
    // Temp directory is in the allowlist (used by existing tests)
    write_config(R"({"server_name": "temp-test"})");
    auto cfg = ServerConfig::from_file(test_config_path);
    EXPECT_EQ(cfg.server_name, "temp-test");
}

// ═══════════════════════════════════════════════════════════════════════════════
// require_auth precedence tests (C10)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(ConfigTest, RequireAuthPrecedence_EnvOverridesFile) {
    // env true overrides file false
    EXPECT_TRUE(ServerConfig::resolve_require_auth(std::optional<bool>(true), false));
    // env false overrides file true
    EXPECT_FALSE(ServerConfig::resolve_require_auth(std::optional<bool>(false), true));
}

TEST_F(ConfigTest, RequireAuthPrecedence_NoEnvUsesFile) {
    // no env → file value used
    EXPECT_TRUE(ServerConfig::resolve_require_auth(std::nullopt, true));
    EXPECT_FALSE(ServerConfig::resolve_require_auth(std::nullopt, false));
}

TEST_F(ConfigTest, RequireAuthPrecedence_EnvTrueFileTrue) {
    EXPECT_TRUE(ServerConfig::resolve_require_auth(std::optional<bool>(true), true));
}

TEST_F(ConfigTest, RequireAuthPrecedence_EnvFalseFileFalse) {
    EXPECT_FALSE(ServerConfig::resolve_require_auth(std::optional<bool>(false), false));
}

TEST_F(ConfigTest, RequireAuthPrecedence_DefaultBothUnset) {
    // no env, file default (true) → true
    EXPECT_TRUE(ServerConfig::resolve_require_auth(std::nullopt, true));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Default config tests (C11)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(ConfigTest, DefaultConfigHostIsLoopback) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.http.host, "127.0.0.1");
}

TEST_F(ConfigTest, DefaultConfigRateLimitRpm) {
    ServerConfig cfg;
    EXPECT_EQ(cfg.http.rate_limit_rpm, 60);
}

TEST_F(ConfigTest, DefaultConfigRequireAuthEnvEmpty) {
    ServerConfig cfg;
    EXPECT_FALSE(cfg.http.require_auth_env.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// TLS config tests (#100/#60)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_F(ConfigTest, DefaultConfigTlsDisabled) {
    ServerConfig cfg;
    EXPECT_FALSE(cfg.http.tls_enabled);
    EXPECT_EQ(cfg.http.tls_cert_path, "");
    EXPECT_EQ(cfg.http.tls_key_path, "");
}

TEST_F(ConfigTest, FromFileParsesTlsFields) {
    write_config(R"({
        "http": {
            "host": "0.0.0.0",
            "port": 8443,
            "tls_enabled": true,
            "tls_cert_path": "/certs/server.pem",
            "tls_key_path": "/certs/server.key"
        }
    })");

    auto cfg = ServerConfig::from_file(test_config_path);
    EXPECT_TRUE(cfg.http.tls_enabled);
    EXPECT_EQ(cfg.http.tls_cert_path, "/certs/server.pem");
    EXPECT_EQ(cfg.http.tls_key_path, "/certs/server.key");
    EXPECT_EQ(cfg.http.port, 8443);
}

TEST_F(ConfigTest, FromFileTlsOmittedDefaultsToDisabled) {
    write_config(R"({"http": {"port": 3001}})");
    auto cfg = ServerConfig::from_file(test_config_path);
    EXPECT_FALSE(cfg.http.tls_enabled);
    EXPECT_EQ(cfg.http.tls_cert_path, "");
    EXPECT_EQ(cfg.http.tls_key_path, "");
}

TEST_F(ConfigTest, FromEnvParsesTlsFields) {
    set_env("SWARM_HTTP_TLS", "true");
    set_env("SWARM_HTTP_TLS_CERT", "/env/cert.pem");
    set_env("SWARM_HTTP_TLS_KEY", "/env/key.pem");

    auto cfg = ServerConfig::from_env();
    EXPECT_TRUE(cfg.http.tls_enabled);
    EXPECT_EQ(cfg.http.tls_cert_path, "/env/cert.pem");
    EXPECT_EQ(cfg.http.tls_key_path, "/env/key.pem");

    unset_env("SWARM_HTTP_TLS");
    unset_env("SWARM_HTTP_TLS_CERT");
    unset_env("SWARM_HTTP_TLS_KEY");
}

TEST_F(ConfigTest, FromEnvTlsFalseWhenNotSet) {
    unset_env("SWARM_HTTP_TLS");
    auto cfg = ServerConfig::from_env();
    EXPECT_FALSE(cfg.http.tls_enabled);
}

TEST_F(ConfigTest, FromEnvTlsAcceptsOneAsTrue) {
    set_env("SWARM_HTTP_TLS", "1");
    auto cfg = ServerConfig::from_env();
    EXPECT_TRUE(cfg.http.tls_enabled);
    unset_env("SWARM_HTTP_TLS");
}

TEST_F(ConfigTest, FromEnvTlsRejectsGarbageAsFalse) {
    set_env("SWARM_HTTP_TLS", "garbage");
    auto cfg = ServerConfig::from_env();
    EXPECT_FALSE(cfg.http.tls_enabled);
    unset_env("SWARM_HTTP_TLS");
}