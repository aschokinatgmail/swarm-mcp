#include <gtest/gtest.h>
#include "mcp_collab/context_store.hpp"
#include <thread>

using namespace mcp_collab;

class ContextStoreTest : public ::testing::Test {
protected:
    ContextStore store;
};

TEST_F(ContextStoreTest, SetAndGet) {
    store.set("key1", {{"value", 42}});
    auto entry = store.get("key1");
    EXPECT_TRUE(entry.has_value());
    EXPECT_EQ(entry->key, "key1");
    EXPECT_EQ(entry->value["value"], 42);
}

TEST_F(ContextStoreTest, GetNonexistent) {
    EXPECT_FALSE(store.get("nonexistent").has_value());
}

TEST_F(ContextStoreTest, SetOverwrites) {
    store.set("key1", "old");
    store.set("key1", "new");
    EXPECT_EQ(store.get("key1")->value, "new");
    EXPECT_EQ(store.get("key1")->version, 2);
}

TEST_F(ContextStoreTest, SetWithOwner) {
    store.set("key1", {{"x", 1}}, "agent-1");
    EXPECT_EQ(store.get("key1")->owner, "agent-1");
}

TEST_F(ContextStoreTest, Delete) {
    store.set("key1", "val");
    EXPECT_TRUE(store.del("key1"));
    EXPECT_FALSE(store.get("key1").has_value());
}

TEST_F(ContextStoreTest, DeleteNonexistent) {
    EXPECT_FALSE(store.del("nonexistent"));
}

TEST_F(ContextStoreTest, Exists) {
    store.set("key1", "val");
    EXPECT_TRUE(store.exists("key1"));
    EXPECT_FALSE(store.exists("key2"));
}

TEST_F(ContextStoreTest, UpdatePartial) {
    store.set("obj", {{"a", 1}, {"b", 2}, {"c", 3}});
    store.update_partial("obj", {{"b", 20}, {"d", 4}});
    auto entry = store.get("obj");
    EXPECT_EQ(entry->value["a"], 1);
    EXPECT_EQ(entry->value["b"], 20);
    EXPECT_EQ(entry->value["c"], 3);
    EXPECT_EQ(entry->value["d"], 4);
    EXPECT_EQ(entry->version, 2);
}

TEST_F(ContextStoreTest, UpdatePartialNonObject) {
    store.set("key1", "scalar");
    EXPECT_FALSE(store.update_partial("key1", {{"x", 1}}));
}

TEST_F(ContextStoreTest, MergeObject) {
    store.set("obj", {{"a", 1}});
    store.merge("obj", {{"b", 2}}, "agent-2");
    auto val = store.get("obj")->value;
    EXPECT_EQ(val["a"], 1);
    EXPECT_EQ(val["b"], 2);
}

TEST_F(ContextStoreTest, MergeArray) {
    store.set("arr", json::array({1, 2, 3}));
    store.merge("arr", json::array({4, 5}));
    auto val = store.get("arr")->value;
    EXPECT_EQ(val.size(), 5u);
}

TEST_F(ContextStoreTest, MergeCreateIfMissing) {
    store.merge("new_key", {{"x", 1}});
    EXPECT_TRUE(store.exists("new_key"));
    EXPECT_EQ(store.get("new_key")->value["x"], 1);
}

TEST_F(ContextStoreTest, ListAll) {
    store.set("a", 1);
    store.set("b", 2);
    store.set("c", 3);
    auto list = store.list();
    EXPECT_EQ(list.size(), 3u);
}

TEST_F(ContextStoreTest, ListWithPrefix) {
    store.set("ns.key1", 1);
    store.set("ns.key2", 2);
    store.set("other.key3", 3);
    auto list = store.list("ns.");
    EXPECT_EQ(list.size(), 2u);
}

TEST_F(ContextStoreTest, Snapshot) {
    store.set("k1", "v1");
    store.set("k2", "v2");
    auto snap = store.snapshot();
    EXPECT_EQ(snap.size(), 2u);
    EXPECT_EQ(snap["k1"], "v1");
}

TEST_F(ContextStoreTest, Clear) {
    store.set("k1", "v1");
    store.set("k2", "v2");
    store.clear();
    EXPECT_EQ(store.size(), 0u);
}

TEST_F(ContextStoreTest, Size) {
    EXPECT_EQ(store.size(), 0u);
    store.set("k1", "v1");
    EXPECT_EQ(store.size(), 1u);
}

TEST_F(ContextStoreTest, ChangeCallback) {
    std::string last_action;
    std::string last_key;
    store.on_change([&](const std::string& key, const ContextEntry&, const std::string& action) {
        last_key = key;
        last_action = action;
    });
    store.set("k1", "v1");
    EXPECT_EQ(last_key, "k1");
    EXPECT_EQ(last_action, "created");
    store.set("k1", "v2");
    EXPECT_EQ(last_action, "updated");
}

TEST_F(ContextStoreTest, EntryToJsonRoundTrip) {
    store.set("test", {{"nested", true}}, "owner-1");
    auto entry = store.get("test");
    auto j = entry->to_json();
    auto restored = ContextEntry::from_json(j);
    EXPECT_EQ(restored.key, "test");
    EXPECT_EQ(restored.owner, "owner-1");
    EXPECT_EQ(restored.version, 1);
}

TEST_F(ContextStoreTest, ThreadSafety) {
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i]() {
            store.set("key-" + std::to_string(i), i);
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(store.size(), 10u);
}