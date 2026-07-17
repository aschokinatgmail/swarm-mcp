#include "mcp_collab/config.hpp"

#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <format>
#include <spdlog/spdlog.h>

namespace mcp_collab {

namespace {
// Check if a path contains ".." segments (defense in depth)
bool contains_dotdot(const std::string& path) {
    for (const auto& segment : std::filesystem::path(path)) {
        if (segment == "..") return true;
    }
    return false;
}

// Check if resolved path is lexically under root (no ".." in relative path)
bool is_under_root(const std::filesystem::path& resolved, const std::filesystem::path& root) {
    auto rel = resolved.lexically_relative(root);
    if (rel.empty()) return false;
    for (const auto& seg : rel) {
        if (seg == "..") return false;
    }
    return true;
}
} // namespace

ServerConfig ServerConfig::from_file(const std::string& path) {
    ServerConfig cfg;

    // Path traversal protection: reject ".." segments in raw input.
    // Validated BEFORE the existence check so rejected paths don't leak via
    // the empty-config early return.
    if (contains_dotdot(path)) {
        throw std::invalid_argument(
            std::format("Config path '{}' contains '..' segments, which are not allowed.", path));
    }

    // Canonicalize and validate against allowlist
    auto resolved = std::filesystem::weakly_canonical(path);

    // Canonicalize the allowlist roots too, so symlinked roots (e.g. macOS
    // /var -> /private/var, or /tmp -> /private/tmp) compare lexically equal
    // against the canonicalized user path.
    std::filesystem::path cwd = std::filesystem::weakly_canonical(std::filesystem::current_path());
    std::filesystem::path cwd_config = cwd / "config";
    std::filesystem::path temp_dir = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());

    const char* home_env = std::getenv("HOME");
    std::filesystem::path home = home_env
        ? std::filesystem::weakly_canonical(std::filesystem::path(home_env))
        : std::filesystem::path();

    bool allowed = is_under_root(resolved, cwd) ||
                   is_under_root(resolved, cwd_config) ||
                   is_under_root(resolved, temp_dir) ||
                   (!home.empty() && is_under_root(resolved, home));

    if (!allowed) {
        throw std::invalid_argument(
            std::format("Config path '{}' (resolved to '{}') is outside allowed directories. "
                       "Config files must be under: current working directory, 'config/' subdirectory, "
                       "system temp directory, or $HOME.",
                       path, resolved.string()));
    }

    std::ifstream file(path);
    if (!file.is_open()) return cfg;

    try {
        json j;
        file >> j;

        if (j.contains("server_name")) cfg.server_name = j["server_name"].get<std::string>();
        if (j.contains("server_version")) cfg.server_version = j["server_version"].get<std::string>();

        if (j.contains("swarm")) {
            auto& s = j["swarm"];
            if (s.contains("id")) cfg.swarm.id = s["id"].get<std::string>();
            if (s.contains("display_name")) cfg.swarm.display_name = s["display_name"].get<std::string>();
            if (s.contains("secret")) cfg.swarm.secret = s["secret"].get<std::string>();
            if (s.contains("open_enrollment")) cfg.swarm.open_enrollment = s["open_enrollment"].get<bool>();
            if (s.contains("allowed_agents")) {
                for (const auto& a : s["allowed_agents"]) cfg.swarm.allowed_agents.push_back(a.get<std::string>());
            }
            if (s.contains("token_ttl_seconds")) cfg.swarm.token_ttl = std::chrono::seconds(s["token_ttl_seconds"].get<int>());
            if (s.contains("heartbeat_timeout_seconds")) cfg.swarm.heartbeat_timeout = std::chrono::seconds(s["heartbeat_timeout_seconds"].get<int>());
        }

        if (j.contains("mqtt")) {
            auto& m = j["mqtt"];
            if (m.contains("host")) cfg.mqtt.host = m["host"].get<std::string>();
            if (m.contains("port")) cfg.mqtt.port = m["port"].get<uint16_t>();
            if (m.contains("username")) cfg.mqtt.username = m["username"].get<std::string>();
            if (m.contains("password")) cfg.mqtt.password = m["password"].get<std::string>();
            if (m.contains("client_id")) cfg.mqtt.client_id = m["client_id"].get<std::string>();
            if (m.contains("keep_alive_sec")) cfg.mqtt.keep_alive_sec = m["keep_alive_sec"].get<int>();
            if (m.contains("use_tls")) cfg.mqtt.use_tls = m["use_tls"].get<bool>();
            if (m.contains("ca_cert_path")) cfg.mqtt.ca_cert_path = m["ca_cert_path"].get<std::string>();
        }

        if (j.contains("http")) {
            auto& h = j["http"];
            if (h.contains("host")) cfg.http.host = h["host"].get<std::string>();
            if (h.contains("port")) cfg.http.port = h["port"].get<uint16_t>();
            if (h.contains("endpoint")) cfg.http.endpoint = h["endpoint"].get<std::string>();
            if (h.contains("cors_origin")) cfg.http.cors_origin = h["cors_origin"].get<std::string>();
            if (h.contains("thread_pool_size")) cfg.http.thread_pool_size = h["thread_pool_size"].get<int>();
            if (h.contains("require_auth")) cfg.http.require_auth = h["require_auth"].get<bool>();
            if (h.contains("rate_limit_rpm")) cfg.http.rate_limit_rpm = h["rate_limit_rpm"].get<int>();
        }

        if (j.contains("git")) {
            auto& g = j["git"];
            if (g.contains("repo_path")) cfg.git.repo_path = g["repo_path"].get<std::string>();
            if (g.contains("default_branch")) cfg.git.default_branch = g["default_branch"].get<std::string>();
            if (g.contains("branch_prefix")) cfg.git.branch_prefix = g["branch_prefix"].get<std::string>();
            if (g.contains("auto_commit")) cfg.git.auto_commit = g["auto_commit"].get<bool>();
        }

    } catch (const json::parse_error& e) {
        spdlog::error("Failed to parse config file '{}': {}", path, e.what());
    } catch (const json::type_error& e) {
        spdlog::error("Type error in config file '{}': {}", path, e.what());
    }

    return cfg;
}

ServerConfig ServerConfig::from_env() {
    ServerConfig cfg;

    const char* env_val = nullptr;

    env_val = std::getenv("SWARM_SERVER_NAME");
    if (env_val) cfg.server_name = env_val;

    env_val = std::getenv("SWARM_ID");
    if (env_val) cfg.swarm.id = env_val;

    env_val = std::getenv("SWARM_SECRET");
    if (env_val) cfg.swarm.secret = env_val;

    env_val = std::getenv("SWARM_MQTT_HOST");
    if (env_val) cfg.mqtt.host = env_val;

    env_val = std::getenv("SWARM_MQTT_PORT");
    if (env_val) {
        try { cfg.mqtt.port = static_cast<uint16_t>(std::stoi(env_val)); }
        catch (const std::exception& e) { spdlog::warn("Invalid SWARM_MQTT_PORT='{}': {}", env_val, e.what()); }
    }

    env_val = std::getenv("SWARM_MQTT_USERNAME");
    if (env_val) cfg.mqtt.username = env_val;

    env_val = std::getenv("SWARM_MQTT_PASSWORD");
    if (env_val) cfg.mqtt.password = env_val;

    env_val = std::getenv("SWARM_HTTP_HOST");
    if (env_val) cfg.http.host = env_val;

    env_val = std::getenv("SWARM_HTTP_PORT");
    if (env_val) {
        try { cfg.http.port = static_cast<uint16_t>(std::stoi(env_val)); }
        catch (const std::exception& e) { spdlog::warn("Invalid SWARM_HTTP_PORT='{}': {}", env_val, e.what()); }
    }

    env_val = std::getenv("SWARM_HTTP_ENDPOINT");
    if (env_val) cfg.http.endpoint = env_val;

    env_val = std::getenv("SWARM_HTTP_AUTH");
    if (env_val) cfg.http.require_auth_env = (std::string(env_val) == "true" || std::string(env_val) == "1");

    env_val = std::getenv("SWARM_RATE_LIMIT_RPM");
    if (env_val) {
        try { cfg.http.rate_limit_rpm = std::stoi(env_val); }
        catch (const std::exception& e) { spdlog::warn("Invalid SWARM_RATE_LIMIT_RPM='{}': {}", env_val, e.what()); }
    }

    env_val = std::getenv("SWARM_GIT_REPO_PATH");
    if (env_val) cfg.git.repo_path = env_val;

    env_val = std::getenv("SWARM_TOKEN_TTL");
    if (env_val) {
        try { cfg.swarm.token_ttl = std::chrono::seconds(std::stoi(env_val)); }
        catch (const std::exception& e) { spdlog::warn("Invalid SWARM_TOKEN_TTL='{}': {}", env_val, e.what()); }
    }

    env_val = std::getenv("SWARM_HEARTBEAT_TIMEOUT");
    if (env_val) {
        try { cfg.swarm.heartbeat_timeout = std::chrono::seconds(std::stoi(env_val)); }
        catch (const std::exception& e) { spdlog::warn("Invalid SWARM_HEARTBEAT_TIMEOUT='{}': {}", env_val, e.what()); }
    }

    return cfg;
}

bool ServerConfig::resolve_require_auth(const std::optional<bool>& env_val, bool file_val) {
    if (env_val.has_value()) return *env_val;
    return file_val;
}

}