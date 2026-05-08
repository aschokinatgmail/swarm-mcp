#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <functional>
#include <mutex>
#include <shared_mutex>

#include <nlohmann/json.hpp>

namespace mcp_collab {

using json = nlohmann::json;

// ── Roles ──────────────────────────────────────────────────────────────
// Coordinator: full control — can register/deregister agents, approve
//              merges, manage tasks, change roles
// Worker:      can create tasks, commit to branches, request merges,
//              read all resources
// Observer:    read-only — can list tasks/agents/context/events
//              but cannot create, modify, or execute actions

enum class Role {
    Coordinator,
    Worker,
    Observer
};

inline std::string role_to_str(Role r) {
    switch (r) {
        case Role::Coordinator: return "coordinator";
        case Role::Worker:      return "worker";
        case Role::Observer:    return "observer";
    }
    return "observer";
}

inline Role role_from_str(const std::string& s) {
    if (s == "coordinator") return Role::Coordinator;
    if (s == "worker")      return Role::Worker;
    return Role::Observer;
}

// ── Permissions ────────────────────────────────────────────────────────

enum class Permission : uint32_t {
    TaskCreate       = 1 << 0,  // Create tasks
    TaskRead         = 1 << 1,  // List/get tasks
    TaskAssign       = 1 << 2,  // Assign tasks to agents
    TaskUpdateStatus = 1 << 3,  // Change task status
    TaskDelete       = 1 << 4,  // Delete tasks
    AgentRegister    = 1 << 5,  // Register new agents
    AgentRead        = 1 << 6,  // List/get agents
    AgentManage      = 1 << 7,  // Change agent roles, deregister
    ContextWrite     = 1 << 8,  // Set/merge context
    ContextRead      = 1 << 9,  // Read context
    BranchCreate      = 1 << 10, // Create git branches
    BranchCommit      = 1 << 11, // Commit to branches
    MergeRequest     = 1 << 12, // Request merges
    MergeApprove     = 1 << 13, // Approve/reject merges
    MergeExecute     = 1 << 14, // Execute merges
    EventPublish     = 1 << 15, // Publish events
    EventRead        = 1 << 16, // Read events
    PromptUse        = 1 << 17, // Use prompt templates
    Admin            = 1 << 18, // Manage server config, roles, secrets
};

constexpr uint32_t operator|(Permission a, Permission b) {
    return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
}

constexpr uint32_t operator|(uint32_t a, Permission b) {
    return a | static_cast<uint32_t>(b);
}

// Role → permission bitmask
inline uint32_t role_permissions(Role r) {
    switch (r) {
        case Role::Coordinator:
            return 0xFFFFFFFF; // all permissions
        case Role::Worker:
            return Permission::TaskCreate | Permission::TaskRead |
                   Permission::TaskAssign | Permission::TaskUpdateStatus |
                   Permission::AgentRead |
                   Permission::ContextWrite | Permission::ContextRead |
                   Permission::BranchCreate | Permission::BranchCommit |
                   Permission::MergeRequest |
                   Permission::EventPublish | Permission::EventRead |
                   Permission::PromptUse;
        case Role::Observer:
            return Permission::TaskRead | Permission::AgentRead |
                   Permission::ContextRead | Permission::EventRead |
                   Permission::PromptUse;
    }
    return 0;
}

inline bool has_permission(Role role, Permission perm) {
    return (role_permissions(role) & static_cast<uint32_t>(perm)) != 0;
}

// ── Authentication ─────────────────────────────────────────────────────

struct AuthToken {
    std::string token_id;
    std::string agent_id;
    Role role{Role::Observer};
    std::string swarm_id;
    std::vector<std::string> scopes;   // additional fine-grained scopes
    std::chrono::system_clock::time_point issued_at{std::chrono::system_clock::now()};
    std::chrono::system_clock::time_point expires_at;
    std::string token_string;  // Full signed token: payload.signature (populated by issue_token)

    bool is_expired() const {
        return std::chrono::system_clock::now() > expires_at;
    }

    json to_json() const {
        return {
            {"token_id", token_id},
            {"agent_id", agent_id},
            {"role", role_to_str(role)},
            {"swarm_id", swarm_id},
            {"scopes", scopes},
            {"issued_at", std::chrono::duration_cast<std::chrono::milliseconds>(
                issued_at.time_since_epoch()).count()},
            {"expires_at", std::chrono::duration_cast<std::chrono::milliseconds>(
                expires_at.time_since_epoch()).count()},
        };
    }
};

// ── HMAC utility (shared between AuthProvider and SecureMqtt) ────────

std::string compute_hmac(const std::string& data, const std::string& secret);

class AuthProvider {
public:
    explicit AuthProvider(const std::string& swarm_secret = "");

    // Issue a new token for an agent joining a swarm
    AuthToken issue_token(const std::string& agent_id, Role role,
                          const std::string& swarm_id,
                          std::chrono::seconds ttl = std::chrono::hours(24));

    // Validate and return the token (or nullopt if invalid/expired)
    std::optional<AuthToken> validate_token(const std::string& token_str) const;

    // Revoke a token
    bool revoke_token(const std::string& token_id);

    // Extract Bearer token from HTTP Authorization header
    static std::optional<std::string> extract_bearer(const std::string& auth_header);

    // Check if an agent has a specific role requirement
    bool authorize(const AuthToken& token, Permission perm) const;
    bool authorize(const AuthToken& token, Role minimum_role) const;

    // Token rotation — re-issue before expiry
    std::optional<AuthToken> refresh_token(const std::string& token_str,
                                           std::chrono::seconds ttl = std::chrono::hours(24));

    // Static signing utilities (used by issue_token and secure_mqtt)
    static std::string sign(const std::string& payload, const std::string& secret) {
        return compute_hmac(payload, secret);
    }

    static bool verify_sig(const std::string& payload, const std::string& signature, const std::string& secret) {
        auto expected = compute_hmac(payload, secret);
        if (expected.size() != signature.size()) return false;
        return std::equal(expected.begin(), expected.end(), signature.begin());
    }

    static std::string generate_secret(size_t length = 32);

private:
    std::string swarm_secret_;
    mutable std::shared_mutex tokens_mutex_;
    std::unordered_map<std::string, AuthToken> active_tokens_;  // token_str -> AuthToken
    std::unordered_set<std::string> revoked_token_ids_;
};

}