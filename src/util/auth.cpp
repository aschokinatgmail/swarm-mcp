#include "mcp_collab/auth.hpp"
#include "mcp_collab/uuid.hpp"
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace mcp_collab {

std::string compute_hmac(const std::string& data, const std::string& secret) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS status;

    status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status != 0) return {};

    DWORD hash_len = 0, result_len = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(DWORD), &result_len, 0);

    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (status != 0) { BCryptCloseAlgorithmProvider(hAlg, 0); return {}; }

    status = BCryptHashData(hHash,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(secret.data())),
        static_cast<ULONG>(secret.size()), 0);
    if (status != 0) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); return {}; }

    status = BCryptHashData(hHash,
        const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data.data())),
        static_cast<ULONG>(data.size()), 0);
    if (status != 0) { BCryptDestroyHash(hHash); BCryptCloseAlgorithmProvider(hAlg, 0); return {}; }

    std::vector<UCHAR> digest(hash_len);
    status = BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(hash_len), 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (status != 0) return {};

    std::ostringstream oss;
    for (auto b : digest) oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(b);
    return oss.str();
#else
    unsigned int md_len = 32;
    unsigned char digest[EVP_MAX_MD_SIZE];
    auto* ctx = HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
        reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest, &md_len);
    if (!ctx) return {};

    std::ostringstream oss;
    for (unsigned int i = 0; i < md_len; i++) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(digest[i]);
    }
    return oss.str();
#endif
}

AuthProvider::AuthProvider(const std::string& swarm_secret)
    : swarm_secret_(swarm_secret) {}

AuthToken AuthProvider::issue_token(const std::string& agent_id, Role role,
                                    const std::string& swarm_id,
                                    std::chrono::seconds ttl) {
    AuthToken token;
    token.token_id = generate_uuid();
    token.agent_id = agent_id;
    token.role = role;
    token.swarm_id = swarm_id;
    token.issued_at = std::chrono::system_clock::now();
    token.expires_at = token.issued_at + ttl;

    // token format: <payload_base64>.<hmac_signature>
    // payload = {token_id}:{agent_id}:{role}:{swarm_id}:{expires_epoch_ms}
    auto expires_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        token.expires_at.time_since_epoch()).count();
    std::string payload = std::format("{}:{}:{}:{}:{}",
        token.token_id, agent_id, role_to_str(role), swarm_id, expires_ms);

    std::string signature = sign(payload, swarm_secret_);
    std::string token_str = std::format("{}.{}", payload, signature);

    {
        std::unique_lock lock(tokens_mutex_);
        active_tokens_[token_str] = token;
    }

    spdlog::info("Token issued: agent={} role={} swarm={} ttl={}s", agent_id, role_to_str(role), swarm_id, ttl.count());
    return token;
}

std::optional<AuthToken> AuthProvider::validate_token(const std::string& token_str) const {
    auto dot = token_str.rfind('.');
    if (dot == std::string::npos) return std::nullopt;

    std::string payload = token_str.substr(0, dot);
    std::string signature = token_str.substr(dot + 1);

    if (!verify_sig(payload, signature, swarm_secret_)) {
        spdlog::warn("Token signature invalid");
        return std::nullopt;
    }

    std::shared_lock lock(tokens_mutex_);

    auto it = active_tokens_.find(token_str);
    if (it == active_tokens_.end()) return std::nullopt;

    if (revoked_token_ids_.contains(it->second.token_id)) {
        spdlog::warn("Token revoked: {}", it->second.token_id);
        return std::nullopt;
    }

    if (it->second.is_expired()) {
        spdlog::warn("Token expired: {}", it->second.token_id);
        return std::nullopt;
    }

    return it->second;
}

bool AuthProvider::revoke_token(const std::string& token_id) {
    std::unique_lock lock(tokens_mutex_);
    revoked_token_ids_.insert(token_id);

    for (auto it = active_tokens_.begin(); it != active_tokens_.end();) {
        if (it->second.token_id == token_id) {
            it = active_tokens_.erase(it);
        } else {
            ++it;
        }
    }

    spdlog::info("Token revoked: {}", token_id);
    return true;
}

std::optional<std::string> AuthProvider::extract_bearer(const std::string& auth_header) {
    if (auth_header.size() > 7 && auth_header.substr(0, 7) == "Bearer ") {
        auto token = auth_header.substr(7);
        // trim
        while (!token.empty() && (token.back() == ' ' || token.back() == '\r' || token.back() == '\n'))
            token.pop_back();
        return token;
    }
    return std::nullopt;
}

bool AuthProvider::authorize(const AuthToken& token, Permission perm) const {
    return has_permission(token.role, perm);
}

bool AuthProvider::authorize(const AuthToken& token, Role minimum_role) const {
    auto level = [](Role r) -> int {
        switch (r) {
            case Role::Observer:    return 0;
            case Role::Worker:      return 1;
            case Role::Coordinator: return 2;
        }
        return 0;
    };
    return level(token.role) >= level(minimum_role);
}

std::optional<AuthToken> AuthProvider::refresh_token(const std::string& token_str,
                                                      std::chrono::seconds ttl) {
    auto existing = validate_token(token_str);
    if (!existing) return std::nullopt;

    // Revoke old
    revoke_token(existing->token_id);

    // Issue new with same role/swarm
    return issue_token(existing->agent_id, existing->role, existing->swarm_id, ttl);
}

}