#pragma once

#include <string>
#include <optional>

namespace mcp_collab {

namespace keychain {

/// Store a secret in the macOS Keychain (or equivalent platform keystore).
/// service:  the app/bundle identifier for the keychain item (e.g. "com.swarm-mcp.auth")
/// account:  a human-readable label (e.g. "swarm-secret")
/// secret:   the value to store
/// returns true on success
bool store_secret(const std::string& service,
                  const std::string& account,
                  const std::string& secret);

/// Retrieve a secret from the macOS Keychain.
/// returns std::nullopt if the item does not exist or retrieval fails
std::optional<std::string> get_secret(const std::string& service,
                                        const std::string& account);

/// Delete a secret from the macOS Keychain.
/// returns true on success (or if the item did not exist)
bool delete_secret(const std::string& service,
                   const std::string& account);

} // namespace keychain

} // namespace mcp_collab
