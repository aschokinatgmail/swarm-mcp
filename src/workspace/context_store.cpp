#include "mcp_collab/context_store.hpp"
#include <spdlog/spdlog.h>

namespace mcp_collab {

json ContextEntry::to_json() const {
    return {
        {"key", key},
        {"value", value},
        {"owner", owner},
        {"updated_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            updated_at.time_since_epoch()).count()},
        {"version", version},
    };
}

ContextEntry ContextEntry::from_json(const json& j) {
    ContextEntry e;
    e.key = j.value("key", "");
    e.value = j.value("value", json());
    e.owner = j.value("owner", "");
    auto ts = j.value("updated_at", 0LL);
    e.updated_at = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(ts);
    e.version = j.value("version", 1);
    return e;
}

bool ContextStore::set(const std::string& key, const json& value, const std::string& owner) {
    ContextEntry entry;
    bool is_new;
    {
        std::unique_lock lock(mutex_);
        auto it = store_.find(key);
        if (it != store_.end()) {
            it->second.value = value;
            it->second.owner = owner;
            it->second.updated_at = std::chrono::system_clock::now();
            it->second.version++;
            entry = it->second;
            is_new = false;
        } else {
            entry.key = key;
            entry.value = value;
            entry.owner = owner;
            entry.updated_at = std::chrono::system_clock::now();
            store_[key] = entry;
            is_new = true;
        }
    }
    notify(key, entry, is_new ? "created" : "updated");
    return true;
}

std::optional<ContextEntry> ContextStore::get(const std::string& key) const {
    std::shared_lock lock(mutex_);
    auto it = store_.find(key);
    if (it != store_.end()) return it->second;
    return std::nullopt;
}

bool ContextStore::del(const std::string& key) {
    ContextEntry entry;
    {
        std::unique_lock lock(mutex_);
        auto it = store_.find(key);
        if (it == store_.end()) return false;
        entry = it->second;
        store_.erase(it);
    }
    notify(key, entry, "deleted");
    return true;
}

bool ContextStore::exists(const std::string& key) const {
    std::shared_lock lock(mutex_);
    return store_.contains(key);
}

bool ContextStore::update_partial(const std::string& key, const json& patch, const std::string& owner) {
    ContextEntry entry;
    {
        std::unique_lock lock(mutex_);
        auto it = store_.find(key);
        if (it == store_.end()) return false;

        if (patch.is_object() && !it->second.value.is_object()) {
            return false;
        }

        if (patch.is_object() && it->second.value.is_object()) {
            it->second.value.merge_patch(patch);
        } else {
            it->second.value = patch;
        }

        it->second.owner = owner;
        it->second.updated_at = std::chrono::system_clock::now();
        it->second.version++;
        entry = it->second;
    }
    notify(key, entry, "updated");
    return true;
}

bool ContextStore::merge(const std::string& key, const json& data, const std::string& owner) {
    ContextEntry entry;
    bool created;
    {
        std::unique_lock lock(mutex_);
        auto it = store_.find(key);
        if (it == store_.end()) {
            ContextEntry new_entry;
            new_entry.key = key;
            new_entry.value = data;
            new_entry.owner = owner;
            new_entry.updated_at = std::chrono::system_clock::now();
            store_[key] = new_entry;
            entry = new_entry;
            created = true;
        } else {
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
            entry = it->second;
            created = false;
        }
    }
    notify(key, entry, created ? "created" : "merged");
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