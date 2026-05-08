#include <gtest/gtest.h>
#include "mcp_collab/transport_http.hpp"
#include "mcp_collab/protocol.hpp"
#include "mcp_collab/auth.hpp"
#include <string>
#include <thread>
#include <chrono>

using namespace mcp_collab;

TEST(RateLimiterTest, UnlimitedWhenZero) {
    RateLimiter rl(0);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(rl.allow("key1"));
    }
}

TEST(RateLimiterTest, UnlimitedWhenNegative) {
    RateLimiter rl(-5);
    EXPECT_TRUE(rl.allow("key1"));
    EXPECT_TRUE(rl.allow("key1"));
}

TEST(RateLimiterTest, AllowsWithinLimit) {
    RateLimiter rl(10);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(rl.allow("client1"));
    }
}

TEST(RateLimiterTest, BlocksOverLimit) {
    RateLimiter rl(5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(rl.allow("client1"));
    }
    EXPECT_FALSE(rl.allow("client1"));
}

TEST(RateLimiterTest, IndependentKeys) {
    RateLimiter rl(2);
    EXPECT_TRUE(rl.allow("key-a"));
    EXPECT_TRUE(rl.allow("key-a"));
    EXPECT_FALSE(rl.allow("key-a"));

    EXPECT_TRUE(rl.allow("key-b"));
    EXPECT_TRUE(rl.allow("key-b"));
    EXPECT_FALSE(rl.allow("key-b"));
}

TEST(RateLimiterTest, ResetsAfterWindow) {
    RateLimiter rl(3);
    EXPECT_TRUE(rl.allow("key"));
    EXPECT_TRUE(rl.allow("key"));
    EXPECT_TRUE(rl.allow("key"));
    EXPECT_FALSE(rl.allow("key"));
}

TEST(SseStreamTest, AddAndRemoveClient) {
    SseStream sse;
    std::string data_received;
    auto id = sse.add_client([&data_received](const std::string& data) -> bool {
        data_received = data;
        return true;
    });
    EXPECT_EQ(sse.client_count(), 1u);

    sse.remove_client(id);
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST(SseStreamTest, BroadcastToClients) {
    SseStream sse;
    std::string received1, received2;

    sse.add_client([&received1](const std::string& data) -> bool {
        received1 = data;
        return true;
    });
    sse.add_client([&received2](const std::string& data) -> bool {
        received2 = data;
        return true;
    });

    sse.broadcast("test", {{"key", "value"}});

    EXPECT_NE(received1.find("test"), std::string::npos);
    EXPECT_NE(received2.find("test"), std::string::npos);
    EXPECT_NE(received1.find("\"key\""), std::string::npos);
}

TEST(SseStreamTest, RemoveStaleClientOnBroadcast) {
    SseStream sse;
    int call_count = 0;

    sse.add_client([&call_count](const std::string&) -> bool {
        call_count++;
        return false;
    });
    EXPECT_EQ(sse.client_count(), 1u);

    sse.broadcast("test", {});
    EXPECT_EQ(sse.client_count(), 0u);
    EXPECT_EQ(call_count, 1);
}

TEST(SseStreamTest, BroadcastNoClients) {
    SseStream sse;
    EXPECT_NO_THROW(sse.broadcast("test", {}));
}

TEST(SseStreamTest, ClientCountZero) {
    SseStream sse;
    EXPECT_EQ(sse.client_count(), 0u);
}

TEST(SseStreamTest, MultipleClientsCount) {
    SseStream sse;
    sse.add_client([](const std::string&) -> bool { return true; });
    sse.add_client([](const std::string&) -> bool { return true; });
    sse.add_client([](const std::string&) -> bool { return true; });
    EXPECT_EQ(sse.client_count(), 3u);
}
