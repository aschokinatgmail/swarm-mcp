#pragma once

#include <string>
#include <functional>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <chrono>

#include <nlohmann/json.hpp>

namespace mcp_collab {

using json = nlohmann::json;

struct Event {
    std::string id;
    std::string type;
    std::string source;
    json data;
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};

    json to_json() const;
    static Event from_json(const json& j);
};

class EventBus {
public:
    using SubscriptionId = uint64_t;
    using EventHandler = std::function<void(const Event&)>;

    EventBus() = default;

    SubscriptionId subscribe(const std::string& event_type, EventHandler handler);
    void unsubscribe(SubscriptionId id);
    void emit(const std::string& type, const std::string& source, const json& data = {});

    std::vector<Event> recent_events(size_t count = 100) const;
    std::vector<Event> query_events(const std::string& type, size_t limit = 100) const;

private:
    mutable std::shared_mutex events_mutex_;
    mutable std::mutex handlers_mutex_;
    std::vector<Event> event_log_;
    static constexpr size_t max_log_size_{10000};
    std::unordered_map<std::string, std::vector<std::pair<SubscriptionId, EventHandler>>> handlers_;
    SubscriptionId next_id_{1};
};

}