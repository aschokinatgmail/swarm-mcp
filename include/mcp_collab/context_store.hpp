#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <chrono>

#include <nlohmann/json.hpp>

namespace mcp_collab {

using json = nlohmann::json;

struct ContextEntry {
    std::string key;
    json value;
    std::string owner;
    std::chrono::system_clock::time_point updated_at{std::chrono::system_clock::now()};
    int version{1};

    json to_json() const;
    static ContextEntry from_json(const json& j);
};

class ContextStore {
public:
    ContextStore() = default;

    bool set(const std::string& key, const json& value, const std::string& owner = "");
    std::optional<ContextEntry> get(const std::string& key) const;
    bool del(const std::string& key);
    bool exists(const std::string& key) const;

    bool update_partial(const std::string& key, const json& patch, const std::string& owner = "");
    bool merge(const std::string& key, const json& data, const std::string& owner = "");

    std::vector<ContextEntry> list(const std::string& prefix = "") const;
    std::unordered_map<std::string, json> snapshot() const;

    void clear();
    size_t size() const;

    using ChangeCallback = std::function<void(const std::string& key, const ContextEntry& entry, const std::string& action)>;
    void on_change(ChangeCallback cb);

private:
    void notify(const std::string& key, const ContextEntry& entry, const std::string& action);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ContextEntry> store_;
    ChangeCallback callback_;
};

}