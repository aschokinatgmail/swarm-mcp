#include "mcp_collab/event_bus.hpp"
#include "mcp_collab/uuid.hpp"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace mcp_collab {

json Event::to_json() const {
    json j = {
        {"id", id},
        {"type", type},
        {"source", source},
        {"data", data},
        {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch()).count()},
    };
    return j;
}

Event Event::from_json(const json& j) {
    Event e;
    e.id = j.value("id", "");
    e.type = j.value("type", "");
    e.source = j.value("source", "");
    e.data = j.value("data", json::object());
    auto ts = j.value("timestamp", 0LL);
    e.timestamp = std::chrono::system_clock::time_point{} + std::chrono::milliseconds(ts);
    return e;
}

EventBus::SubscriptionId EventBus::subscribe(const std::string& event_type, EventHandler handler) {
    std::unique_lock lock(handlers_mutex_);
    auto id = next_id_++;
    handlers_[event_type].emplace_back(id, std::move(handler));
    return id;
}

void EventBus::unsubscribe(SubscriptionId id) {
    std::unique_lock lock(handlers_mutex_);
    for (auto& [type, list] : handlers_) {
        std::erase_if(list, [id](const auto& pair) { return pair.first == id; });
    }
}

void EventBus::emit(const std::string& type, const std::string& source, const json& data) {
    Event event{
        .id = generate_uuid(),
        .type = type,
        .source = source,
        .data = data,
    };

    {
        std::shared_lock lock(handlers_mutex_);
        if (auto it = handlers_.find(type); it != handlers_.end()) {
            for (const auto& [_, handler] : it->second) {
                try { handler(event); }
                catch (const std::exception& e) {
                    spdlog::error("Event handler exception for '{}': {}", type, e.what());
                }
            }
        }
        if (auto it = handlers_.find("*"); it != handlers_.end()) {
            for (const auto& [_, handler] : it->second) {
                try { handler(event); }
                catch (const std::exception& e) {
                    spdlog::error("Wildcard event handler exception: {}", e.what());
                }
            }
        }
    }

    {
        std::unique_lock lock(events_mutex_);
        event_log_.push_back(event);
        if (event_log_.size() > max_log_size_) {
            event_log_.erase(event_log_.begin(), event_log_.begin() + (event_log_.size() - max_log_size_));
        }
    }
}

std::vector<Event> EventBus::recent_events(size_t count) const {
    std::shared_lock lock(events_mutex_);
    auto n = std::min(count, event_log_.size());
    return {event_log_.end() - static_cast<ptrdiff_t>(n), event_log_.end()};
}

std::vector<Event> EventBus::query_events(const std::string& type, size_t limit) const {
    std::shared_lock lock(events_mutex_);
    std::vector<Event> result;
    for (auto it = event_log_.rbegin(); it != event_log_.rend() && result.size() < limit; ++it) {
        if (it->type == type) result.push_back(*it);
    }
    return result;
}

}