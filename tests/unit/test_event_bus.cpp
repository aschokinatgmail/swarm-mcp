#include <gtest/gtest.h>
#include "mcp_collab/event_bus.hpp"

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