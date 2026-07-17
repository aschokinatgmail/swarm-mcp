#include <gtest/gtest.h>
#include "mcp_collab/event_bus.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace mcp_collab;

class EventBusTest : public ::testing::Test {
protected:
    EventBus bus;
};

TEST_F(EventBusTest, EmitAndSubscribe) {
    int count = 0;
    bus.subscribe("test.event", [&](const Event& e) { count++; });
    bus.emit("test.event", "source", {{"key", "value"}});
    EXPECT_EQ(count, 1);
}

TEST_F(EventBusTest, MultipleSubscribers) {
    int count1 = 0, count2 = 0;
    bus.subscribe("evt", [&](const Event& e) { count1++; });
    bus.subscribe("evt", [&](const Event& e) { count2++; });
    bus.emit("evt", "src");
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
}

TEST_F(EventBusTest, WildcardSubscribe) {
    int count = 0;
    bus.subscribe("*", [&](const Event& e) { count++; });
    bus.emit("evt1", "src");
    bus.emit("evt2", "src");
    EXPECT_EQ(count, 2);
}

TEST_F(EventBusTest, NoSubscriber) {
    bus.emit("no.subscriber", "src");
}

TEST_F(EventBusTest, Unsubscribe) {
    int count = 0;
    auto id = bus.subscribe("evt", [&](const Event& e) { count++; });
    bus.emit("evt", "src");
    EXPECT_EQ(count, 1);
    bus.unsubscribe(id);
    bus.emit("evt", "src");
    EXPECT_EQ(count, 1);
}

TEST_F(EventBusTest, EventDataPreserved) {
    json received;
    bus.subscribe("data.test", [&](const Event& e) { received = e.data; });
    json data = {{"nested", {{"key", 42}}}};
    bus.emit("data.test", "src", data);
    EXPECT_EQ(received["nested"]["key"], 42);
}

TEST_F(EventBusTest, RecentEvents) {
    bus.emit("e1", "src");
    bus.emit("e2", "src");
    bus.emit("e3", "src");
    auto recent = bus.recent_events(3);
    EXPECT_EQ(recent.size(), 3u);
}

TEST_F(EventBusTest, RecentEventsLimit) {
    for (int i = 0; i < 5; ++i) bus.emit("evt", "src");
    auto recent = bus.recent_events(2);
    EXPECT_EQ(recent.size(), 2u);
}

TEST_F(EventBusTest, QueryByType) {
    bus.emit("type-a", "src");
    bus.emit("type-b", "src");
    bus.emit("type-a", "src");
    auto results = bus.query_events("type-a", 10);
    EXPECT_EQ(results.size(), 2u);
}

TEST_F(EventBusTest, EventToJsonRoundTrip) {
    Event e;
    e.id = "evt-1";
    e.type = "test";
    e.source = "src";
    e.data = {{"x", 1}};
    auto j = e.to_json();
    auto restored = Event::from_json(j);
    EXPECT_EQ(restored.id, e.id);
    EXPECT_EQ(restored.type, e.type);
    EXPECT_EQ(restored.source, e.source);
}

TEST_F(EventBusTest, EventLogMaxSize) {
    for (int i = 0; i < 11000; ++i) {
        bus.emit("overflow", "src", {{"i", i}});
    }
    auto recent = bus.recent_events(20000);
    EXPECT_LT(recent.size(), 10100u);
    EXPECT_GT(recent.size(), 9000u);
}

// #112: log trimming must be O(1) (deque pop_front), not O(n) (vector erase).
// Push well beyond max_log_size_ and verify the log is trimmed to exactly
// max_log_size_ and that the MOST RECENT events are retained in order.
TEST_F(EventBusTest, LogTrimIsO1AndRetainsMostRecent) {
    // max_log_size_ is 10000 (private). Push max+1000 distinct events.
    constexpr int kMaxLog = 10000;
    constexpr int kExtra = 1000;
    for (int i = 0; i < kMaxLog + kExtra; ++i) {
        bus.emit("trim", "src", {{"i", i}});
    }
    auto recent = bus.recent_events(kMaxLog + kExtra);
    // Trimmed to exactly max_log_size_.
    ASSERT_EQ(recent.size(), static_cast<size_t>(kMaxLog));
    // Most recent retained: first kept event should be index kExtra, last kMaxLog+kExtra-1.
    EXPECT_EQ(recent.front().data["i"].get<int>(), kExtra);
    EXPECT_EQ(recent.back().data["i"].get<int>(), kMaxLog + kExtra - 1);
    // Verify ordering is preserved (ascending i).
    bool ordered = true;
    for (size_t k = 1; k < recent.size(); ++k) {
        if (recent[k].data["i"].get<int>() < recent[k - 1].data["i"].get<int>()) {
            ordered = false;
            break;
        }
    }
    EXPECT_TRUE(ordered);
}

// #45: a handler that calls unsubscribe() on itself must not deadlock.
// Before the fix, handlers ran under a shared_lock; unsubscribe() needs a
// unique_lock, which cannot be acquired while the shared_lock is held ->
// deadlock. Now handlers are invoked outside the lock.
TEST_F(EventBusTest, HandlerUnsubscribesSelfNoDeadlock) {
    std::atomic<int> count{0};
    EventBus::SubscriptionId self_id = 0;
    self_id = bus.subscribe("evt", [&](const Event&) {
        count++;
        bus.unsubscribe(self_id);
    });
    // Run emit in a separate thread with a timeout watchdog so a deadlock
    // fails the test instead of hanging the suite.
    auto fut = std::async(std::launch::async, [&] { bus.emit("evt", "src"); });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "emit() deadlocked when handler called unsubscribe()";
    EXPECT_EQ(count.load(), 1);
    // A second emit should not invoke the now-unsubscribed handler.
    bus.emit("evt", "src");
    EXPECT_EQ(count.load(), 1);
}

// #45: a handler that calls subscribe() (adding a new handler) must not deadlock.
TEST_F(EventBusTest, HandlerSubscribesNewNoDeadlock) {
    std::atomic<int> first{0};
    std::atomic<int> second{0};
    bus.subscribe("evt", [&](const Event&) {
        first++;
        // Subscribe a new handler from within the handler invocation.
        bus.subscribe("evt", [&](const Event&) { second++; });
    });
    auto fut = std::async(std::launch::async, [&] { bus.emit("evt", "src"); });
    ASSERT_EQ(fut.wait_for(std::chrono::seconds(5)), std::future_status::ready)
        << "emit() deadlocked when handler called subscribe()";
    EXPECT_EQ(first.load(), 1);
    // The newly-subscribed handler is NOT invoked for this emit (snapshot was
    // taken before subscribe), but should fire on the next emit.
    bus.emit("evt", "src");
    EXPECT_EQ(first.load(), 2);
    EXPECT_EQ(second.load(), 1);
}