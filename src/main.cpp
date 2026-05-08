#include <iostream>
#include <string>
#include <csignal>
#include <atomic>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "mcp_collab/server.hpp"
#include "mcp_collab/config.hpp"
#include "mcp_collab/auth.hpp"

static std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running.store(false);
    spdlog::info("Shutdown signal received");
}

void print_usage(const char* prog) {
    std::cout << std::format(
        "Swarm MCP - Multi-Agent Collaboration Server\n\n"
        "Usage: {} [OPTIONS]\n\n"
        "Options:\n"
        "  -c, --config <path>        Config file path (default: config/default.json)\n"
        "  -H, --host <addr>          HTTP listen address (default: 0.0.0.0)\n"
        "  -p, --port <port>          HTTP listen port (default: 3001)\n"
        "  -m, --mqtt <host>          MQTT broker address (default: localhost)\n"
        "  -M, --mqtt-port <port>     MQTT broker port (default: 1883)\n"
        "  -s, --swarm <id>           Swarm identifier\n"
        "  -k, --secret <key>         Swarm authentication secret\n"
        "  -g, --git-path <path>      Git repository path\n"
        "  --no-auth                  Disable authentication (dev mode)\n"
        "  --enroll <agent_id>        Enroll a new agent and print its token\n"
        "  --role <role>              Role for enrollment (coordinator, worker, observer)\n"
        "  -v, --verbose              Enable verbose logging\n"
        "  -q, --quiet                Suppress all but error logs\n"
        "  -h, --help                 Show this help\n",
        prog);
}

int main(int argc, char* argv[]) {
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    mcp_collab::ServerConfig config;
    std::string config_path = "config/default.json";
    bool verbose = false;
    bool quiet = false;
    bool no_auth = false;
    std::string enroll_agent;
    std::string enroll_role = "worker";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            config_path = argv[++i];
        } else if ((arg == "-H" || arg == "--host") && i + 1 < argc) {
            config.http.host = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.http.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-m" || arg == "--mqtt") && i + 1 < argc) {
            config.mqtt.host = argv[++i];
        } else if ((arg == "-M" || arg == "--mqtt-port") && i + 1 < argc) {
            config.mqtt.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-s" || arg == "--swarm") && i + 1 < argc) {
            config.swarm.id = argv[++i];
        } else if ((arg == "-k" || arg == "--secret") && i + 1 < argc) {
            config.swarm.secret = argv[++i];
        } else if ((arg == "-g" || arg == "--git-path") && i + 1 < argc) {
            config.git.repo_path = argv[++i];
        } else if (arg == "--no-auth") {
            no_auth = true;
            config.http.require_auth = false;
        } else if (arg == "--enroll" && i + 1 < argc) {
            enroll_agent = argv[++i];
        } else if (arg == "--role" && i + 1 < argc) {
            enroll_role = argv[++i];
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (verbose) spdlog::set_level(spdlog::level::debug);
    if (quiet) spdlog::set_level(spdlog::level::err);

    // Load config: file < env < CLI
    auto file_config = mcp_collab::ServerConfig::from_file(config_path);
    auto env_config = mcp_collab::ServerConfig::from_env();

    // Config precedence: CLI args > env vars > config file > defaults.
    // For each value, pick the first non-empty source.
    auto pick_str = [](const std::string& cli, const std::string& env, const std::string& file, const std::string& def) -> std::string {
        if (!cli.empty()) return cli;
        if (!env.empty()) return env;
        if (!file.empty()) return file;
        return def;
    };
    auto pick_uint = [](uint16_t cli_val, uint16_t cli_default, uint16_t env_val, uint16_t file_val, uint16_t builtin_default) {
        // If CLI was explicitly set (different from default), use it
        if (cli_val != cli_default) return cli_val;
        // Otherwise env > file
        return env_val ? env_val : (file_val ? file_val : builtin_default);
    };

    config.server_name = pick_str(config.server_name, env_config.server_name, file_config.server_name, "swarm-mcp");
    config.swarm.id = pick_str(config.swarm.id, env_config.swarm.id, file_config.swarm.id, "");
    config.swarm.secret = pick_str(config.swarm.secret, env_config.swarm.secret, file_config.swarm.secret, "");
    config.mqtt.host = pick_str(config.mqtt.host, env_config.mqtt.host, file_config.mqtt.host, "localhost");
    config.mqtt.port = pick_uint(config.http.port, 3001, env_config.mqtt.port, file_config.mqtt.port, 1883);
    config.http.host = pick_str(config.http.host, env_config.http.host, file_config.http.host, "0.0.0.0");
    config.http.port = pick_uint(config.http.port, 3001, env_config.http.port, file_config.http.port, 3001);
    config.http.endpoint = pick_str(config.http.endpoint, env_config.http.endpoint, file_config.http.endpoint, "/mcp");
    config.git.repo_path = pick_str(config.git.repo_path, env_config.git.repo_path, file_config.git.repo_path, "");
    config.git.branch_prefix = pick_str(config.git.branch_prefix, "", file_config.git.branch_prefix, "collab/");
    config.http.require_auth = env_config.http.require_auth ? env_config.http.require_auth : file_config.http.require_auth;
    config.swarm.token_ttl = env_config.swarm.token_ttl.count() > 0 ? env_config.swarm.token_ttl : file_config.swarm.token_ttl;
    config.swarm.heartbeat_timeout = env_config.swarm.heartbeat_timeout.count() > 0 ? env_config.swarm.heartbeat_timeout : file_config.swarm.heartbeat_timeout;

    // Enrollment mode — issue a token and exit
    if (!enroll_agent.empty()) {
        if (config.swarm.secret.empty()) {
            spdlog::error("Cannot enroll agent without a swarm secret. Set --secret or SWARM_SECRET.");
            return 1;
        }

        mcp_collab::AuthProvider auth(config.swarm.secret);
        auto role = mcp_collab::role_from_str(enroll_role);
        auto token = auth.issue_token(enroll_agent, role, config.swarm.id.empty() ? "default" : config.swarm.id);

        std::cout << std::format("Agent enrolled successfully.\n\n"
            "  Agent ID : {}\n"
            "  Role     : {}\n"
            "  Swarm    : {}\n"
            "  Token    : {}\n\n"
            "Use this token as: Authorization: Bearer <token>\n",
            enroll_agent, enroll_role, config.swarm.id.empty() ? "default" : config.swarm.id,
            token.token_id);
        return 0;
    }

    if (config.swarm.secret.empty() && config.http.require_auth) {
        config.swarm.secret = mcp_collab::AuthProvider::generate_secret(32);
        spdlog::warn("No swarm secret configured. Auto-generated a random secret for this session.");
        spdlog::warn("  Set --secret or SWARM_SECRET for persistent authentication.");
    }

    if (!config.http.require_auth) {
        spdlog::warn("Authentication is DISABLED — all agents can connect without tokens.");
    }

    spdlog::info("╔══════════════════════════════════════════╗");
    spdlog::info("║       Swarm MCP Collaboration Server     ║");
    spdlog::info("║       {} v{}           ║", config.server_name, config.server_version);
    spdlog::info("╠══════════════════════════════════════════╣");
    spdlog::info("║  Swarm : {}                    ║", config.swarm.id.empty() ? "default" : config.swarm.id);
    spdlog::info("║  Auth  : {}                      ║", config.http.require_auth ? "ON" : "OFF");
    spdlog::info("║  HTTP  : {}:{}{}               ║", config.http.host, config.http.port, config.http.endpoint);
    spdlog::info("║  MQTT  : {}:{}                    ║", config.mqtt.host, config.mqtt.port);
    spdlog::info("║  Git   : {}                      ║", config.git.repo_path.empty() ? "(none)" : config.git.repo_path);
    spdlog::info("╚══════════════════════════════════════════╝");

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    mcp_collab::SwarmServer server(config);

    if (!server.start()) {
        spdlog::error("Failed to start server");
        return 1;
    }

    spdlog::info("Server running. Press Ctrl+C to stop.");

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    server.stop();
    spdlog::info("Goodbye!");
    return 0;
}