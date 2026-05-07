#include "mcp_collab/config.hpp"

#include <fstream>
#include <cstdlib>
#include <filesystem>

namespace mcp_collab {

ServerConfig ServerConfig::from_file(const std::string& path) {
    ServerConfig cfg;

    if (!std::filesystem::exists(path)) return cfg;

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
        }

        if (j.contains("git")) {
            auto& g = j["git"];
            if (g.contains("repo_path")) cfg.git.repo_path = g["repo_path"].get<std::string>();
            if (g.contains("default_branch")) cfg.git.default_branch = g["default_branch"].get<std::string>();
            if (g.contains("branch_prefix")) cfg.git.branch_prefix = g["branch_prefix"].get<std::string>();
            if (g.contains("auto_commit")) cfg.git.auto_commit = g["auto_commit"].get<bool>();
        }

    } catch (const json::parse_error& e) {
        return cfg;
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
    if (env_val) cfg.mqtt.port = static_cast<uint16_t>(std::stoi(env_val));

    env_val = std::getenv("SWARM_MQTT_USERNAME");
    if (env_val) cfg.mqtt.username = env_val;

    env_val = std::getenv("SWARM_MQTT_PASSWORD");
    if (env_val) cfg.mqtt.password = env_val;

    env_val = std::getenv("SWARM_HTTP_HOST");
    if (env_val) cfg.http.host = env_val;

    env_val = std::getenv("SWARM_HTTP_PORT");
    if (env_val) cfg.http.port = static_cast<uint16_t>(std::stoi(env_val));

    env_val = std::getenv("SWARM_HTTP_ENDPOINT");
    if (env_val) cfg.http.endpoint = env_val;

    env_val = std::getenv("SWARM_HTTP_AUTH");
    if (env_val) cfg.http.require_auth = (std::string(env_val) == "true" || std::string(env_val) == "1");

    env_val = std::getenv("SWARM_GIT_REPO_PATH");
    if (env_val) cfg.git.repo_path = env_val;

    env_val = std::getenv("SWARM_TOKEN_TTL");
    if (env_val) cfg.swarm.token_ttl = std::chrono::seconds(std::stoi(env_val));

    env_val = std::getenv("SWARM_HEARTBEAT_TIMEOUT");
    if (env_val) cfg.swarm.heartbeat_timeout = std::chrono::seconds(std::stoi(env_val));

    return cfg;
}

}