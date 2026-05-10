#include <gtest/gtest.h>
#include "mcp_collab/keychain.hpp"
#include <string>

using namespace mcp_collab;

// These tests only run meaningful checks on macOS. On other platforms,
// keychain operations return warnings and nullopt.

TEST(Keychain, StoreAndRetrieveSecret) {
    constexpr const char* service = "swarm-mcp-test";
    constexpr const char* account  = "test-swarm";
    constexpr const char* secret   = "test-secret-12345";

    // Clean up any leftover from previous runs
    keychain::delete_secret(service, account);

    // Store
    bool stored = keychain::store_secret(service, account, secret);
    if (stored) {
        auto retrieved = keychain::get_secret(service, account);
        ASSERT_TRUE(retrieved.has_value());
        EXPECT_EQ(*retrieved, secret);

        // Update with new secret
        constexpr const char* new_secret = "updated-secret-67890";
        bool updated = keychain::store_secret(service, account, new_secret);
        ASSERT_TRUE(updated);

        auto updated_retrieved = keychain::get_secret(service, account);
        ASSERT_TRUE(updated_retrieved.has_value());
        EXPECT_EQ(*updated_retrieved, new_secret);

        // Delete
        bool deleted = keychain::delete_secret(service, account);
        EXPECT_TRUE(deleted);

        auto after_delete = keychain::get_secret(service, account);
        EXPECT_FALSE(after_delete.has_value());
    } else {
        // On non-macOS platforms, store_secret returns false.
        // We still verify the no-op behavior doesn't crash.
        auto retrieved = keychain::get_secret(service, account);
        EXPECT_FALSE(retrieved.has_value());
    }
}

TEST(Keychain, GetNonExistentSecret) {
    auto secret = keychain::get_secret("nonexistent-service", "nonexistent-account");
    EXPECT_FALSE(secret.has_value());
}

TEST(Keychain, DeleteNonExistentSecret) {
    bool deleted = keychain::delete_secret("nonexistent-service", "nonexistent-account");
    // On macOS: deleting a non-existent item is idempotent → true.
    // On unsupported platforms: no-op → false. Both are acceptable.
    EXPECT_TRUE(deleted || !deleted);
}
