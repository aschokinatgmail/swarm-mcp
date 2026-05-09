#pragma once

#include <string>
#include <functional>
#include <chrono>
#include <optional>
#include <atomic>
#include <thread>

#include <nlohmann/json.hpp>

namespace mcp_collab {

using json = nlohmann::json;

class PersistenceLayer {
public:
    using Snapshot = json;

    explicit PersistenceLayer(const std::string& path);
    ~PersistenceLayer();

    bool save(const Snapshot& data);
    std::optional<Snapshot> load();
    bool save_if_changed(const Snapshot& data);

    void set_auto_save_interval(std::chrono::seconds interval);
    void enable_auto_save(std::function<Snapshot()> snapshot_fn);
    void disable_auto_save();

    std::string path() const { return path_; }
    bool exists() const;
    bool clear();

    static Snapshot create_snapshot(
        const json& tasks,
        const json& agents,
        const json& context,
        const json& branches,
        const json& merge_requests);

private:
    std::string path_;
    std::chrono::seconds auto_save_interval_{300};
    std::atomic<bool> auto_save_active_{false};
    std::thread auto_save_thread_;
    json last_saved_;
};

}
