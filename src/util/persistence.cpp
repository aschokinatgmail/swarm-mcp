#include "mcp_collab/persistence.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>

namespace mcp_collab {

PersistenceLayer::PersistenceLayer(const std::string& path)
    : path_(path) {}

PersistenceLayer::~PersistenceLayer() {
    disable_auto_save();
}

bool PersistenceLayer::save(const Snapshot& data) {
    std::error_code ec;
    auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
    }

    std::string tmp_path = path_ + ".tmp";
    try {
        std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            spdlog::error("Failed to open persistence file for writing: {}", tmp_path);
            return false;
        }
        f << data.dump(2);
        f.close();

        if (!f.good()) {
            spdlog::error("Failed to write persistence file: {}", tmp_path);
            std::filesystem::remove(tmp_path, ec);
            return false;
        }

        std::filesystem::rename(tmp_path, path_);
        last_saved_ = data;
        spdlog::debug("Persistence snapshot saved to {}", path_);
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Persistence save error: {}", e.what());
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
}

std::optional<PersistenceLayer::Snapshot> PersistenceLayer::load() {
    try {
        if (!std::filesystem::exists(path_)) {
            spdlog::info("No persistence file found at {}", path_);
            return std::nullopt;
        }

        std::ifstream f(path_, std::ios::binary);
        if (!f.is_open()) {
            spdlog::error("Failed to open persistence file: {}", path_);
            return std::nullopt;
        }

        json data = json::parse(f, nullptr, false);
        if (data.is_discarded()) {
            spdlog::error("Failed to parse persistence file: {}", path_);
            return std::nullopt;
        }

        last_saved_ = data;
        spdlog::info("Persistence snapshot loaded from {} ({} bytes)", path_,
            std::filesystem::file_size(path_));
        return data;
    } catch (const std::exception& e) {
        spdlog::error("Persistence load error: {}", e.what());
        return std::nullopt;
    }
}

bool PersistenceLayer::save_if_changed(const Snapshot& data) {
    if (data == last_saved_) return true;
    return save(data);
}

bool PersistenceLayer::exists() const {
    return std::filesystem::exists(path_);
}

bool PersistenceLayer::clear() {
    std::error_code ec;
    return std::filesystem::remove(path_, ec);
}

void PersistenceLayer::set_auto_save_interval(std::chrono::seconds interval) {
    auto_save_interval_ = interval;
}

void PersistenceLayer::enable_auto_save(std::function<Snapshot()> snapshot_fn) {
    if (auto_save_active_) disable_auto_save();
    auto_save_active_ = true;

    std::thread([this, fn = std::move(snapshot_fn)]() {
        while (auto_save_active_) {
            std::this_thread::sleep_for(auto_save_interval_);
            if (!auto_save_active_) break;
            try {
                auto snapshot = fn();
                save_if_changed(snapshot);
            } catch (const std::exception& e) {
                spdlog::error("Auto-save error: {}", e.what());
            }
        }
    }).detach();
}

void PersistenceLayer::disable_auto_save() {
    auto_save_active_ = false;
}

PersistenceLayer::Snapshot PersistenceLayer::create_snapshot(
    const json& tasks, const json& agents, const json& context,
    const json& branches, const json& merge_requests) {
    return {
        {"version", 1},
        {"saved_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
        {"tasks", tasks},
        {"agents", agents},
        {"context", context},
        {"branches", branches},
        {"merge_requests", merge_requests},
    };
}

}
