#ifdef __APPLE__

#include "mcp_collab/keychain.hpp"
#include <Security/Security.h>
#include <spdlog/spdlog.h>

namespace mcp_collab {

namespace keychain {

bool store_secret(const std::string& service,
                  const std::string& account,
                  const std::string& secret) {
    // First delete any existing item with the same service+account to avoid duplicates
    delete_secret(service, account);

    OSStatus status = SecKeychainAddGenericPassword(
        nullptr,                       // default keychain
        static_cast<UInt32>(service.size()),
        service.c_str(),
        static_cast<UInt32>(account.size()),
        account.c_str(),
        static_cast<UInt32>(secret.size()),
        secret.c_str(),
        nullptr                        // itemRef (optional)
    );

    if (status == errSecSuccess) {
        spdlog::info("Keychain: stored secret for service={} account={}", service, account);
        return true;
    }

    spdlog::error("Keychain: failed to store secret (OSStatus={})", status);
    return false;
}

std::optional<std::string> get_secret(const std::string& service,
                                        const std::string& account) {
    UInt32 secret_len = 0;
    void* secret_data = nullptr;

    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,                       // default keychain
        static_cast<UInt32>(service.size()),
        service.c_str(),
        static_cast<UInt32>(account.size()),
        account.c_str(),
        &secret_len,
        &secret_data,
        nullptr                        // itemRef (optional)
    );

    if (status != errSecSuccess) {
        if (status == errSecItemNotFound) {
            spdlog::debug("Keychain: no secret found for service={} account={}", service, account);
        } else {
            spdlog::warn("Keychain: failed to retrieve secret (OSStatus={})", status);
        }
        return std::nullopt;
    }

    std::string secret(static_cast<const char*>(secret_data), secret_len);
    SecKeychainItemFreeContent(nullptr, secret_data);
    return secret;
}

bool delete_secret(const std::string& service,
                   const std::string& account) {
    SecKeychainItemRef item = nullptr;
    OSStatus status = SecKeychainFindGenericPassword(
        nullptr,
        static_cast<UInt32>(service.size()),
        service.c_str(),
        static_cast<UInt32>(account.size()),
        account.c_str(),
        nullptr,
        nullptr,
        &item
    );

    if (status == errSecItemNotFound) {
        return true;  // nothing to delete
    }

    if (status != errSecSuccess) {
        spdlog::warn("Keychain: failed to locate secret for deletion (OSStatus={})", status);
        return false;
    }

    status = SecKeychainItemDelete(item);
    CFRelease(item);

    if (status == errSecSuccess) {
        spdlog::info("Keychain: deleted secret for service={} account={}", service, account);
        return true;
    }

    spdlog::error("Keychain: failed to delete secret (OSStatus={})", status);
    return false;
}

} // namespace keychain

} // namespace mcp_collab

#else

// Stub implementation for non-macOS platforms
#include "mcp_collab/keychain.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

namespace keychain {

bool store_secret(const std::string&, const std::string&, const std::string&) {
    spdlog::warn("Keychain not supported on this platform");
    return false;
}

std::optional<std::string> get_secret(const std::string&, const std::string&) {
    spdlog::warn("Keychain not supported on this platform");
    return std::nullopt;
}

bool delete_secret(const std::string&, const std::string&) {
    spdlog::warn("Keychain not supported on this platform");
    return false;
}

} // namespace keychain

} // namespace mcp_collab

#endif
