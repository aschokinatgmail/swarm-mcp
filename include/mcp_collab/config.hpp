#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <optional>

#include "mcp_collab/auth.hpp"

namespace mcp_collab {

struct MqttConfig {
    std::string host{"localhost"};
    uint16_t port{1883};
    std::string username;
    std::string password;
    std::string client_id;
    int keep_alive_sec{60};
    bool use_tls{false};
    std::string ca_cert_path;
};

struct HttpConfig {
    std::string host{"127.0.0.1"};
    uint16_t port{3001};
    std::string endpoint{"/mcp"};
    int thread_pool_size{4};
    std::string cors_origin{"*"};
    bool require_auth{true};
    std::optional<bool> require_auth_env{};
    int rate_limit_rpm{60};
};

struct GitConfig {
    std::string repo_path;
    std::string default_branch{"main"};
    std::string branch_prefix{"collab/"};
    bool auto_commit{false};
};

struct SwarmConfig {
    std::string id;                             // Swarm identifier (e.g. "my-project")
    std::string display_name;                   // Human-readable name
    std::vector<std::string> allowed_agents;    // Empty = open, non-empty = allowlist
    std::string secret;                         // HMAC signing key for tokens (empty = insecure)
    bool open_enrollment{true};                 // true = any agent joins, false = invite-only
    std::chrono::seconds token_ttl{86400};      // Default token lifetime (24h)
    std::chrono::seconds heartbeat_timeout{120};// Stale agent prune timeout
};

struct ServerConfig {
    std::string server_name{"swarm-mcp"};
    std::string server_version{"1.0.0"};
    SwarmConfig swarm;
    MqttConfig mqtt;
    HttpConfig http;
    GitConfig git;

    static ServerConfig from_file(const std::string& path);
    static ServerConfig from_env();
    static bool resolve_require_auth(const std::optional<bool>& env_val, bool file_val);

    // Read a secret from a file, trimming trailing whitespace/newlines.
    // Returns std::nullopt if the file cannot be opened or is empty.
    static std::optional<std::string> read_secret_file(const std::string& path);
};

}