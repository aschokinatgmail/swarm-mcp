#include "mcp_collab/context_store.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

json ContextEntry::to_json() const {
    return {
        {"key", key},
        {"value", value},
        {"owner", owner},
        {"version", version},
        {"updated_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            updated_at.time_since_epoch()).count()},
    };
}

ContextEntry ContextEntry::from_json(const json& j) {
    ContextEntry e;
    e.key = j.value("key", "");
    e.value = j.value("value", json::object());
    e.owner = j.value("owner", "");
    e.version = j.value("version", 1);
    auto ts = j.value("updated_at", 0LL);
    e.updated_at = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(ts);
    return e;
}

bool ContextStore::set(const std::string& key, const json& value, const std::string& owner) {
    std::unique_lock lock(mutex_);
    auto it = store_.find(key);
    if (it != store_.end()) {
        it->second.value = value;
        it->second.owner = owner;
        it->second.updated_at = std::chrono::system_clock::now();
        it->second.version++;
        notify(key, it->second, "updated");
    } else {
        ContextEntry entry{.key = key, .value = value, .owner = owner};
        store_[key] = entry;
        notify(key, store_[key], "created");
    }
    return true;
}

std::optional<ContextEntry> ContextStore::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    auto it = store_.find(key);
    if (it != store_.end()) return it->second;
    return std::nullopt;
}

bool ContextStore::del(const std::string& key) {
    std::unique_lock lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) return false;

    ContextEntry entry = it->second;
    store_.erase(it);
    notify(key, entry, "deleted");
    return true;
}

bool ContextStore::exists(const std::string& key) const {
    std::shared_lock lock(mutex_);
    return store_.contains(key);
}

bool ContextStore::update_partial(const std::string& key, const json& patch, const std::string& owner) {
    std::unique_lock lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) return false;

    if (patch.is_object() && it->second.value.is_object()) {
        it->second.value.merge_patch(patch);
    } else {
        it->second.value = patch;
    }

    it->second.owner = owner;
    it->second.updated_at = std::chrono::system_clock::now();
    it->second.version++;
    notify(key, it->second, "updated");
    return true;
}

bool ContextStore::merge(const std::string& key, const json& data, const std::string& owner) {
    std::unique_lock lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return set(key, data, owner);
    }

    if (data.is_object() && it->second.value.is_object()) {
        it->second.value.merge_patch(data);
    } else if (data.is_array() && it->second.value.is_array()) {
        auto arr = it->second.value.get<std::vector<json>>();
        auto new_arr = data.get<std::vector<json>>();
        arr.insert(arr.end(), new_arr.begin(), new_arr.end());
        it->second.value = arr;
    } else {
        it->second.value = data;
    }

    it->second.owner = owner;
    it->second.updated_at = std::chrono::system_clock::now();
    it->second.version++;
    notify(key, it->second, "merged");
    return true;
}

std::vector<ContextEntry> ContextStore::list(const std::string& prefix) const {
    std::shared_lock lock(mutex_);
    std::vector<ContextEntry> result;
    for (const auto& [key, entry] : store_) {
        if (prefix.empty() || key.starts_with(prefix)) {
            result.push_back(entry);
        }
    }
    return result;
}

std::unordered_map<std::string, json> ContextStore::snapshot() const {
    std::shared_lock lock(mutex_);
    std::unordered_map<std::string, json> snap;
    for (const auto& [key, entry] : store_) {
        snap[key] = entry.value;
    }
    return snap;
}

void ContextStore::clear() {
    std::unique_lock lock(mutex_);
    store_.clear();
}

size_t ContextStore::size() const {
    std::shared_lock lock(mutex_);
    return store_.size();
}

void ContextStore::on_change(ChangeCallback cb) {
    callback_ = std::move(cb);
}

void ContextStore::notify(const std::string& key, const ContextEntry& entry, const std::string& action) {
    if (callback_) callback_(key, entry, action);
}

}