#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <chrono>
#include <functional>

#include <nlohmann/json.hpp>
#include "mcp_collab/auth.hpp"

namespace mcp_collab {

using json = nlohmann::json;

enum class AgentStatus {
    Online,
    Busy,
    Idle,
    Offline
};

struct AgentInfo {
    std::string id;
    std::string name;
    std::string platform;
    std::string hostname;
    std::string swarm_id;                       // which swarm this agent belongs to
    Role role{Role::Worker};                     // agent's authorization role
    std::vector<std::string> capabilities;
    AgentStatus status{AgentStatus::Offline};
    std::chrono::system_clock::time_point registered_at{std::chrono::system_clock::now()};
    std::chrono::system_clock::time_point last_heartbeat{std::chrono::system_clock::now()};
    json metadata;

    json to_json() const;
    static AgentInfo from_json(const json& j);
};

class AgentRegistry {
public:
    explicit AgentRegistry(const std::string& swarm_id = "") : swarm_id_(swarm_id) {}

    std::string register_agent(const AgentInfo& info, const AuthToken& token);
    bool update_agent(const std::string& id, const AgentInfo& info, const AuthToken& token);
    bool deregister_agent(const std::string& id, const AuthToken& token);

    // Read operations — no auth required (checked at protocol layer)
    std::optional<AgentInfo> get_agent(const std::string& id) const;
    std::vector<AgentInfo> list_agents(AgentStatus filter = {}) const;
    std::vector<AgentInfo> list_swarm_agents(const std::string& swarm_id, AgentStatus filter = {}) const;
    std::vector<AgentInfo> find_by_capability(const std::string& capability) const;
    std::vector<AgentInfo> find_idle() const;
    std::vector<AgentInfo> find_idle_in_swarm(const std::string& swarm_id) const;
    std::vector<AgentInfo> find_by_role(Role role) const;

    bool heartbeat(const std::string& id);
    size_t prune_stale(std::chrono::seconds timeout);
    size_t count() const;
    size_t swarm_count(const std::string& swarm_id) const;

    // Role management — requires Coordinator
    bool set_agent_role(const std::string& agent_id, Role new_role, const AuthToken& requester);

    using RegistryCallback = std::function<void(const std::string& event, const AgentInfo& agent)>;
    void on_change(RegistryCallback cb);

private:
    bool can_modify(const AuthToken& token, const std::string& target_agent_id) const;

    std::string swarm_id_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, AgentInfo> agents_;  // agent_id -> AgentInfo
    RegistryCallback callback_;
};

struct AgentInfo {
    std::string id;
    std::string name;
    std::string platform;
    std::string hostname;
    std::vector<std::string> capabilities;
    AgentStatus status{AgentStatus::Offline};
    std::chrono::system_clock::time_point registered_at{std::chrono::system_clock::now()};
    std::chrono::system_clock::time_point last_heartbeat{std::chrono::system_clock::now()};
    json metadata;

    json to_json() const;
    static AgentInfo from_json(const json& j);
};

class AgentRegistry {
public:
    AgentRegistry() = default;

    std::string register_agent(const AgentInfo& info);
    bool update_agent(const std::string& id, const AgentInfo& info);
    bool deregister_agent(const std::string& id);
    bool heartbeat(const std::string& id);

    std::optional<AgentInfo> get_agent(const std::string& id) const;
    std::vector<AgentInfo> list_agents(AgentStatus filter = {}) const;
    std::vector<AgentInfo> find_by_capability(const std::string& capability) const;
    std::vector<AgentInfo> find_idle() const;

    size_t prune_stale(std::chrono::seconds timeout);
    size_t count() const;

    using RegistryCallback = std::function<void(const std::string& event, const AgentInfo& agent)>;
    void on_change(RegistryCallback cb);

private:
    std::string agent_status_str(AgentStatus s) const;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, AgentInfo> agents_;
    RegistryCallback callback_;
};

}