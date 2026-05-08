#include "mcp_collab/agent_registry.hpp"
#include "mcp_collab/uuid.hpp"
#include "mcp_collab/platform.hpp"
#include <algorithm>
#include <spdlog/spdlog.h>

namespace mcp_collab {

json AgentInfo::ModelInfo::to_json() const {
    json j = {
        {"provider", provider},
        {"model_id", model_id},
        {"model_family", model_family},
    };
    if (context_window > 0) j["context_window"] = context_window;
    if (max_output_tokens > 0) j["max_output_tokens"] = max_output_tokens;
    return j;
}

AgentInfo::ModelInfo AgentInfo::ModelInfo::from_json(const json& j) {
    ModelInfo m;
    m.provider = j.value("provider", "");
    m.model_id = j.value("model_id", "");
    m.model_family = j.value("model_family", "");
    m.context_window = j.value("context_window", 0);
    m.max_output_tokens = j.value("max_output_tokens", 0);
    return m;
}

json AgentInfo::EnvironmentInfo::to_json() const {
    json j = {
        {"runtime", runtime},
        {"os", os},
    };
    if (cpu_cores > 0) j["cpu_cores"] = cpu_cores;
    if (memory_mb > 0) j["memory_mb"] = memory_mb;
    if (!gpu.empty()) j["gpu"] = gpu;
    if (!supported_languages.empty()) j["supported_languages"] = supported_languages;
    return j;
}

AgentInfo::EnvironmentInfo AgentInfo::EnvironmentInfo::from_json(const json& j) {
    EnvironmentInfo e;
    e.runtime = j.value("runtime", "");
    e.os = j.value("os", "");
    e.cpu_cores = j.value("cpu_cores", 0);
    e.memory_mb = j.value("memory_mb", 0);
    e.gpu = j.value("gpu", "");
    e.supported_languages = j.value("supported_languages", std::vector<std::string>{});
    return e;
}

json AgentInfo::to_json() const {
    json j = {
        {"id", id},
        {"name", name},
        {"platform", platform},
        {"hostname", hostname},
        {"swarm_id", swarm_id},
        {"role", role_to_str(role)},
        {"capabilities", capabilities},
        {"status", status == AgentStatus::Online ? "online" :
                  status == AgentStatus::Busy ? "busy" :
                  status == AgentStatus::Idle ? "idle" : "offline"},
        {"registered_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            registered_at.time_since_epoch()).count()},
        {"last_heartbeat", std::chrono::duration_cast<std::chrono::milliseconds>(
            last_heartbeat.time_since_epoch()).count()},
        {"metadata", metadata},
        {"model", model.to_json()},
        {"environment", environment.to_json()},
    };
    return j;
}

AgentInfo AgentInfo::from_json(const json& j) {
    AgentInfo a;
    a.id = j.value("id", "");
    a.name = j.value("name", "");
    a.platform = j.value("platform", "");
    a.hostname = j.value("hostname", "");
    a.swarm_id = j.value("swarm_id", "");
    a.role = role_from_str(j.value("role", "worker"));
    a.capabilities = j.value("capabilities", std::vector<std::string>{});
    auto s = j.value("status", "offline");
    if (s == "online") a.status = AgentStatus::Online;
    else if (s == "busy") a.status = AgentStatus::Busy;
    else if (s == "idle") a.status = AgentStatus::Idle;
    else a.status = AgentStatus::Offline;
    auto ts1 = j.value("registered_at", 0LL);
    a.registered_at = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(ts1);
    auto ts2 = j.value("last_heartbeat", 0LL);
    a.last_heartbeat = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(ts2);
    a.metadata = j.value("metadata", json::object());
    if (j.contains("model") && j["model"].is_object()) a.model = ModelInfo::from_json(j["model"]);
    if (j.contains("environment") && j["environment"].is_object()) a.environment = EnvironmentInfo::from_json(j["environment"]);
    return a;
}

bool AgentRegistry::can_modify(const AuthToken& token, const std::string& target_agent_id) const {
    // Coordinators can modify anyone in their swarm
    if (token.role == Role::Coordinator && token.swarm_id == swarm_id_) return true;
    // Agents can modify themselves
    if (token.agent_id == target_agent_id) return true;
    return false;
}

std::string AgentRegistry::register_agent(const AgentInfo& info, const AuthToken& token) {
    if (token.swarm_id != swarm_id_ && !swarm_id_.empty()) {
        spdlog::warn("Agent registration rejected: swarm mismatch (token={}, server={})",
            token.swarm_id, swarm_id_);
        return "";
    }

    std::unique_lock lock(mutex_);
    AgentInfo agent = info;
    if (agent.id.empty()) agent.id = token.agent_id.empty() ? generate_uuid() : token.agent_id;
    if (agent.hostname.empty()) agent.hostname = platform::hostname();
    if (agent.platform.empty()) agent.platform =
#ifdef MCP_PLATFORM_WINDOWS
        "windows";
#elif defined(MCP_PLATFORM_MACOS)
        "macos";
#elif defined(MCP_PLATFORM_LINUX)
        "linux";
#else
        "unknown";
#endif
    agent.swarm_id = swarm_id_;
    agent.role = token.role;
    agent.registered_at = std::chrono::system_clock::now();
    agent.last_heartbeat = std::chrono::system_clock::now();
    agent.status = AgentStatus::Online;

    if (agent.metadata.contains("model") && agent.metadata["model"].is_object()) {
        agent.model = AgentInfo::ModelInfo::from_json(agent.metadata["model"]);
    }
    if (agent.metadata.contains("environment") && agent.metadata["environment"].is_object()) {
        agent.environment = AgentInfo::EnvironmentInfo::from_json(agent.metadata["environment"]);
    }

    agents_[agent.id] = agent;
    lock.unlock();

    spdlog::info("Agent registered: id={} name=\"{}\" swarm={} role={}",
        agent.id, agent.name, agent.swarm_id, role_to_str(agent.role));
    if (callback_) callback_("agent.registered", agent);
    return agent.id;
}

bool AgentRegistry::update_agent(const std::string& id, const AgentInfo& info, const AuthToken& token) {
    if (!can_modify(token, id) && token.role != Role::Coordinator) {
        spdlog::warn("Agent update denied: agent={} requester={} role={}", id, token.agent_id, role_to_str(token.role));
        return false;
    }

    std::unique_lock lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;

    it->second.name = info.name.empty() ? it->second.name : info.name;
    it->second.capabilities = info.capabilities.empty() ? it->second.capabilities : info.capabilities;
    it->second.metadata = info.metadata.is_null() ? it->second.metadata : info.metadata;
    it->second.status = info.status;
    it->second.last_heartbeat = std::chrono::system_clock::now();

    AgentInfo agent = it->second;
    lock.unlock();

    if (callback_) callback_("agent.updated", agent);
    return true;
}

bool AgentRegistry::deregister_agent(const std::string& id, const AuthToken& token) {
    if (token.role != Role::Coordinator && token.agent_id != id) {
        spdlog::warn("Agent deregister denied: agent={} requester={} role={}", id, token.agent_id, role_to_str(token.role));
        return false;
    }

    std::unique_lock lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;

    AgentInfo agent = it->second;
    agents_.erase(it);
    lock.unlock();

    spdlog::info("Agent deregistered: id={} name=\"{}\"", agent.id, agent.name);
    if (callback_) callback_("agent.deregistered", agent);
    return true;
}

bool AgentRegistry::heartbeat(const std::string& id) {
    std::unique_lock lock(mutex_);
    auto it = agents_.find(id);
    if (it == agents_.end()) return false;

    it->second.last_heartbeat = std::chrono::system_clock::now();
    it->second.status = AgentStatus::Online;
    return true;
}

std::optional<AgentInfo> AgentRegistry::get_agent(const std::string& id) const {
    std::shared_lock lock(mutex_);
    auto it = agents_.find(id);
    if (it != agents_.end()) return it->second;
    return std::nullopt;
}

std::vector<AgentInfo> AgentRegistry::list_agents(std::optional<AgentStatus> filter) const {
    std::shared_lock lock(mutex_);
    std::vector<AgentInfo> result;
    for (const auto& [_, agent] : agents_) {
        if (!filter.has_value() || agent.status == *filter) {
            result.push_back(agent);
        }
    }
    return result;
}

std::vector<AgentInfo> AgentRegistry::list_swarm_agents(const std::string& swarm_id, std::optional<AgentStatus> filter) const {
    std::shared_lock lock(mutex_);
    std::vector<AgentInfo> result;
    for (const auto& [_, agent] : agents_) {
        if (agent.swarm_id != swarm_id) continue;
        if (filter.has_value() && agent.status != *filter) continue;
        result.push_back(agent);
    }
    return result;
}

std::vector<AgentInfo> AgentRegistry::find_by_capability(const std::string& capability) const {
    std::shared_lock lock(mutex_);
    std::vector<AgentInfo> result;
    for (const auto& [_, agent] : agents_) {
        if (std::ranges::find(agent.capabilities, capability) != agent.capabilities.end()) {
            result.push_back(agent);
        }
    }
    return result;
}

std::vector<AgentInfo> AgentRegistry::find_idle() const {
    std::shared_lock lock(mutex_);
    std::vector<AgentInfo> result;
    for (const auto& [_, agent] : agents_) {
        if (agent.status == AgentStatus::Idle || agent.status == AgentStatus::Online) {
            result.push_back(agent);
        }
    }
    return result;
}

std::vector<AgentInfo> AgentRegistry::find_idle_in_swarm(const std::string& swarm_id) const {
    std::shared_lock lock(mutex_);
    std::vector<AgentInfo> result;
    for (const auto& [_, agent] : agents_) {
        if (agent.swarm_id != swarm_id) continue;
        if (agent.status == AgentStatus::Idle || agent.status == AgentStatus::Online) {
            result.push_back(agent);
        }
    }
    return result;
}

std::vector<AgentInfo> AgentRegistry::find_by_role(Role role) const {
    std::shared_lock lock(mutex_);
    std::vector<AgentInfo> result;
    for (const auto& [_, agent] : agents_) {
        if (agent.role == role) result.push_back(agent);
    }
    return result;
}

size_t AgentRegistry::prune_stale(std::chrono::seconds timeout) {
    std::vector<AgentInfo> pruned_agents;
    size_t pruned = 0;

    {
        std::unique_lock lock(mutex_);
        auto now = std::chrono::system_clock::now();

        for (auto it = agents_.begin(); it != agents_.end();) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_heartbeat);
            if (elapsed >= timeout) {
                pruned_agents.push_back(it->second);
                it = agents_.erase(it);
                pruned++;
            } else {
                ++it;
            }
        }
    }

    for (const auto& agent : pruned_agents) {
        spdlog::info("Pruned stale agent: id={} name=\"{}\"", agent.id, agent.name);
        if (callback_) callback_("agent.pruned", agent);
    }
    return pruned;
}

size_t AgentRegistry::count() const {
    std::shared_lock lock(mutex_);
    return agents_.size();
}

size_t AgentRegistry::swarm_count(const std::string& swarm_id) const {
    std::shared_lock lock(mutex_);
    size_t n = 0;
    for (const auto& [_, agent] : agents_) {
        if (agent.swarm_id == swarm_id) n++;
    }
    return n;
}

bool AgentRegistry::set_agent_role(const std::string& agent_id, Role new_role, const AuthToken& requester) {
    if (requester.role != Role::Coordinator) {
        spdlog::warn("Role change denied: requester={} is not coordinator", requester.agent_id);
        return false;
    }

    std::unique_lock lock(mutex_);
    auto it = agents_.find(agent_id);
    if (it == agents_.end()) return false;

    auto old_role = it->second.role;
    it->second.role = new_role;
    auto agent = it->second;
    lock.unlock();

    spdlog::info("Agent role changed: agent={} {} -> {}", agent_id, role_to_str(old_role), role_to_str(new_role));
    if (callback_) callback_("agent.role_changed", agent);
    return true;
}

void AgentRegistry::on_change(RegistryCallback cb) {
    callback_ = std::move(cb);
}

}