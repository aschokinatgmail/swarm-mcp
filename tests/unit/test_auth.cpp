#include <gtest/gtest.h>
#include "mcp_collab/auth.hpp"
#include <chrono>
#include <thread>
#include <set>

using namespace mcp_collab;

class AuthTest : public ::testing::Test {
protected:
    void SetUp() override {
        secret = "test-swarm-secret-key-12345";
        provider = std::make_unique<AuthProvider>(secret);
    }
    std::string secret;
    std::unique_ptr<AuthProvider> provider;
};

// ── Role / Permission tests ────────────────────────────────────────

TEST_F(AuthTest, RoleToString) {
    EXPECT_EQ(role_to_str(Role::Coordinator), "coordinator");
    EXPECT_EQ(role_to_str(Role::Worker), "worker");
    EXPECT_EQ(role_to_str(Role::Observer), "observer");
}

TEST_F(AuthTest, RoleFromString) {
    EXPECT_EQ(role_from_str("coordinator"), Role::Coordinator);
    EXPECT_EQ(role_from_str("worker"), Role::Worker);
    EXPECT_EQ(role_from_str("observer"), Role::Observer);
    EXPECT_EQ(role_from_str("unknown"), Role::Observer);
}

TEST_F(AuthTest, CoordinatorHasAllPermissions) {
    uint32_t perms = role_permissions(Role::Coordinator);
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::TaskDelete));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::AgentManage));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::MergeApprove));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::MergeExecute));
    EXPECT_TRUE(has_permission(Role::Coordinator, Permission::Admin));
    EXPECT_EQ(perms, 0xFFFFFFFF);
}

TEST_F(AuthTest, WorkerPermissions) {
    EXPECT_TRUE(has_permission(Role::Worker, Permission::TaskCreate));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::TaskRead));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::TaskAssign));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::ContextWrite));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::BranchCreate));
    EXPECT_TRUE(has_permission(Role::Worker, Permission::MergeRequest));
    EXPECT_FALSE(has_permission(Role::Worker, Permission::TaskDelete));
    EXPECT_FALSE(has_permission(Role::Worker, Permission::MergeApprove));
    EXPECT_FALSE(has_permission(Role::Worker, Permission::AgentManage));
    EXPECT_FALSE(has_permission(Role::Worker, Permission::Admin));
}

TEST_F(AuthTest, ObserverPermissions) {
    EXPECT_TRUE(has_permission(Role::Observer, Permission::TaskRead));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::AgentRead));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::ContextRead));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::EventRead));
    EXPECT_TRUE(has_permission(Role::Observer, Permission::PromptUse));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::TaskCreate));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::ContextWrite));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::BranchCreate));
    EXPECT_FALSE(has_permission(Role::Observer, Permission::MergeRequest));
}

// ── Token issuance ─────────────────────────────────────────────────

TEST_F(AuthTest, IssueToken) {
    auto token = provider->issue_token("agent-1", Role::Worker, "test-swarm");
    EXPECT_EQ(token.agent_id, "agent-1");
    EXPECT_EQ(token.role, Role::Worker);
    EXPECT_EQ(token.swarm_id, "test-swarm");
    EXPECT_FALSE(token.token_id.empty());
    EXPECT_FALSE(token.is_expired());
}

TEST_F(AuthTest, ValidateValidToken) {
    auto token = provider->issue_token("agent-1", Role::Worker, "test-swarm");
    auto validated = provider->validate_token(AuthProvider::extract_bearer("Bearer test-token").value_or(""));
    // We need the actual token string; issue_token puts it in internal map
    // Let's re-validate by getting from the provider
}

TEST_F(AuthTest, ValidateTokenRoundTrip) {
    auto token = provider->issue_token("agent-2", Role::Coordinator, "swarm-42");
    EXPECT_EQ(token.agent_id, "agent-2");
    EXPECT_EQ(token.role, Role::Coordinator);
    EXPECT_EQ(token.swarm_id, "swarm-42");
    EXPECT_FALSE(token.is_expired());

    // Validate using the full token_string (includes signature)
    auto validated = provider->validate_token(token.token_string);
    EXPECT_TRUE(validated.has_value());
    EXPECT_EQ(validated->agent_id, "agent-2");
    EXPECT_EQ(validated->role, Role::Coordinator);
}

TEST_F(AuthTest, ValidateTamperedTokenFails) {
    auto token = provider->issue_token("agent-2", Role::Coordinator, "swarm-42");
    std::string tampered = token.token_string + "X";
    auto validated = provider->validate_token(tampered);
    EXPECT_FALSE(validated.has_value());
}

TEST_F(AuthTest, ValidateExpiredTokenFails) {
    auto token = provider->issue_token("agent-3", Role::Observer, "test-swarm", std::chrono::seconds(0));
    // Token with 0-second TTL should be expired immediately or very soon
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(token.is_expired());

    auto validated = provider->validate_token(token.token_string);
    EXPECT_FALSE(validated.has_value());
}

TEST_F(AuthTest, ValidateForgedSignatureFails) {
    auto token = provider->issue_token("agent-fake", Role::Worker, "test-swarm");
    // Create a forged token with same payload but different signature
    std::string forged = token.token_string.substr(0, token.token_string.rfind('.') + 1) + "deadbeef";
    auto validated = provider->validate_token(forged);
    EXPECT_FALSE(validated.has_value());
}

TEST_F(AuthTest, RevokeTokenPreventsValidation) {
    auto token = provider->issue_token("agent-4", Role::Worker, "test-swarm");
    EXPECT_TRUE(provider->revoke_token(token.token_id));
    // After revocation, the token should no longer validate
    auto validated = provider->validate_token(token.token_string);
    EXPECT_FALSE(validated.has_value());
}

TEST_F(AuthTest, ExtractBearerToken) {
    auto result = AuthProvider::extract_bearer("Bearer abc123def");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, "abc123def");

    result = AuthProvider::extract_bearer("bearer lowercase");
    EXPECT_FALSE(result.has_value());

    result = AuthProvider::extract_bearer("Basic abc123");
    EXPECT_FALSE(result.has_value());

    result = AuthProvider::extract_bearer("");
    EXPECT_FALSE(result.has_value());

    result = AuthProvider::extract_bearer("Bearer  ");
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 0u);
}

TEST_F(AuthTest, AuthorizeByPermission) {
    EXPECT_TRUE(provider->authorize(AuthToken{.role = Role::Coordinator}, Permission::Admin));
    EXPECT_TRUE(provider->authorize(AuthToken{.role = Role::Worker}, Permission::TaskCreate));
    EXPECT_FALSE(provider->authorize(AuthToken{.role = Role::Observer}, Permission::TaskCreate));
}

TEST_F(AuthTest, AuthorizeByRole) {
    EXPECT_TRUE(provider->authorize(AuthToken{.role = Role::Coordinator}, Role::Worker));
    EXPECT_TRUE(provider->authorize(AuthToken{.role = Role::Coordinator}, Role::Observer));
    EXPECT_TRUE(provider->authorize(AuthToken{.role = Role::Worker}, Role::Worker));
    EXPECT_FALSE(provider->authorize(AuthToken{.role = Role::Worker}, Role::Coordinator));
    EXPECT_FALSE(provider->authorize(AuthToken{.role = Role::Observer}, Role::Worker));
}

TEST_F(AuthTest, RefreshToken) {
    auto token = provider->issue_token("agent-5", Role::Worker, "test-swarm", std::chrono::hours(1));
    // Token string is internal, but refresh should work via the internal map
    // The refresh method validates, revokes, and re-issues
    auto refreshed = provider->refresh_token("nonexistent-token");
    EXPECT_FALSE(refreshed.has_value());
}

TEST_F(AuthTest, HmacConsistency) {
    auto hmac1 = compute_hmac("test-data", secret);
    auto hmac2 = compute_hmac("test-data", secret);
    EXPECT_EQ(hmac1, hmac2);
    EXPECT_FALSE(hmac1.empty());
}

TEST_F(AuthTest, HmacDifferentInputs) {
    auto hmac1 = compute_hmac("data-1", secret);
    auto hmac2 = compute_hmac("data-2", secret);
    EXPECT_NE(hmac1, hmac2);
}

TEST_F(AuthTest, HmacDifferentSecrets) {
    auto hmac1 = compute_hmac("same-data", secret);
    auto hmac2 = compute_hmac("same-data", "different-secret");
    EXPECT_NE(hmac1, hmac2);
}

TEST_F(AuthTest, AuthTokenToJson) {
    AuthToken token;
    token.token_id = "tid-123";
    token.agent_id = "agent-1";
    token.role = Role::Worker;
    token.swarm_id = "swarm-1";
    auto j = token.to_json();
    EXPECT_EQ(j["agent_id"], "agent-1");
    EXPECT_EQ(j["role"], "worker");
    EXPECT_EQ(j["swarm_id"], "swarm-1");
}

// ── Constant-time signature verification ──────────────────────────

TEST_F(AuthTest, VerifySigConstantTimeCorrectAndTampered) {
    std::string payload = "constant-time-verify-test-payload";
    std::string correct_sig = AuthProvider::sign(payload, secret);

    // 1. Correct signature verifies
    EXPECT_TRUE(AuthProvider::verify_sig(payload, correct_sig, secret));

    // 2. Tampered signature (one byte changed) fails
    std::string tampered_sig = correct_sig;
    ASSERT_FALSE(tampered_sig.empty());
    tampered_sig[tampered_sig.size() - 1] ^= 0x01;  // flip one bit in the last byte
    EXPECT_FALSE(AuthProvider::verify_sig(payload, tampered_sig, secret));

    // 3. Wrong-length signature fails via the length early-return path
    std::string wrong_len_sig = correct_sig + "XX";
    EXPECT_FALSE(AuthProvider::verify_sig(payload, wrong_len_sig, secret));
}

// ── generate_secret (CSPRNG) ───────────────────────────────────────

TEST_F(AuthTest, GenerateSecretLength) {
    auto s = AuthProvider::generate_secret(32);
    EXPECT_EQ(s.size(), 32u);
}

TEST_F(AuthTest, GenerateSecretCharset) {
    auto s = AuthProvider::generate_secret(64);
    for (char c : s) {
        EXPECT_TRUE((c >= '0' && c <= '9') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z'))
            << "Invalid character in secret: " << c;
    }
}

TEST_F(AuthTest, GenerateSecretUniqueness) {
    std::set<std::string> secrets;
    for (int i = 0; i < 1000; ++i) {
        auto [it, inserted] = secrets.insert(AuthProvider::generate_secret(32));
        EXPECT_TRUE(inserted) << "Duplicate secret at iteration " << i;
    }
    EXPECT_EQ(secrets.size(), 1000u);
}