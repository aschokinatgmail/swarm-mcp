#include <gtest/gtest.h>
#include "mcp_collab/persistence.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace mcp_collab;

class PersistenceTest : public ::testing::Test {
protected:
    std::string test_dir = std::filesystem::temp_directory_path().string() + "/swarm_mcp_test_persistence";
    std::string test_path = test_dir + "/snapshot.json";

    void SetUp() override {
        std::error_code ec;
        std::filesystem::create_directories(test_dir, ec);
        cleanup();
    }

    void TearDown() override {
        cleanup();
        std::error_code ec;
        std::filesystem::remove_all(test_dir, ec);
    }

    void cleanup() {
        std::error_code ec;
        std::filesystem::remove(test_path, ec);
        std::filesystem::remove(test_path + ".tmp", ec);
    }
};

TEST_F(PersistenceTest, SaveAndLoad) {
    PersistenceLayer pl(test_path);
    json data = {{"key", "value"}, {"number", 42}, {"array", json::array({1, 2, 3})}};

    EXPECT_TRUE(pl.save(data));
    EXPECT_TRUE(pl.exists());

    auto loaded = pl.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["key"], "value");
    EXPECT_EQ((*loaded)["number"], 42);
    EXPECT_EQ((*loaded)["array"].size(), 3u);
}

TEST_F(PersistenceTest, LoadNonexistent) {
    PersistenceLayer pl(test_path);
    auto loaded = pl.load();
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(PersistenceTest, ExistsFalse) {
    PersistenceLayer pl(test_path);
    EXPECT_FALSE(pl.exists());
}

TEST_F(PersistenceTest, ExistsTrueAfterSave) {
    PersistenceLayer pl(test_path);
    pl.save({{"x", 1}});
    EXPECT_TRUE(pl.exists());
}

TEST_F(PersistenceTest, Clear) {
    PersistenceLayer pl(test_path);
    pl.save({{"x", 1}});
    EXPECT_TRUE(pl.exists());
    EXPECT_TRUE(pl.clear());
    EXPECT_FALSE(pl.exists());
}

TEST_F(PersistenceTest, ClearNonexistent) {
    PersistenceLayer pl(test_path);
    EXPECT_FALSE(pl.clear());
}

TEST_F(PersistenceTest, SaveIfChangedNew) {
    PersistenceLayer pl(test_path);
    json data = {{"changed", true}};
    EXPECT_TRUE(pl.save_if_changed(data));
    auto loaded = pl.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["changed"], true);
}

TEST_F(PersistenceTest, SaveIfChangedSkipsIdentical) {
    PersistenceLayer pl(test_path);
    json data = {{"same", true}};
    EXPECT_TRUE(pl.save(data));
    EXPECT_TRUE(pl.save_if_changed(data));
}

TEST_F(PersistenceTest, SaveIfChangedWritesNewData) {
    PersistenceLayer pl(test_path);
    pl.save({{"version", 1}});
    json data2 = {{"version", 2}};
    EXPECT_TRUE(pl.save_if_changed(data2));
    auto loaded = pl.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["version"], 2);
}

TEST_F(PersistenceTest, AtomicWrite) {
    PersistenceLayer pl(test_path);
    pl.save({{"atomic", true}});
    EXPECT_FALSE(std::filesystem::exists(test_path + ".tmp"));
    EXPECT_TRUE(std::filesystem::exists(test_path));
}

TEST_F(PersistenceTest, SaveCreatesDirectories) {
    std::string nested = test_dir + "/sub/dir/data.json";
    PersistenceLayer pl(nested);
    EXPECT_TRUE(pl.save({{"nested", true}}));
    EXPECT_TRUE(std::filesystem::exists(nested));
    {
        std::error_code ec;
        std::filesystem::remove_all(test_dir + "/sub", ec);
    }
}

TEST_F(PersistenceTest, CreateSnapshot) {
    json tasks_arr = json::array();
    tasks_arr.push_back({{"id", "t1"}});
    auto snap = PersistenceLayer::create_snapshot(
        tasks_arr,
        json::array({{"id", "a1"}}),
        json::object({{"k", "v"}}),
        json::array(),
        json::array()
    );
    EXPECT_EQ(snap["version"], 1);
    EXPECT_TRUE(snap.contains("saved_at"));
    EXPECT_TRUE(snap.contains("tasks"));
    EXPECT_TRUE(snap.contains("agents"));
    EXPECT_TRUE(snap.contains("context"));
    EXPECT_TRUE(snap.contains("branches"));
    EXPECT_TRUE(snap.contains("merge_requests"));
    EXPECT_EQ(snap["tasks"].size(), 1u);
}

TEST_F(PersistenceTest, LoadCorruptFile) {
    {
        std::ofstream f(test_path, std::ios::binary);
        f << "{{{{invalid json";
    }
    PersistenceLayer pl(test_path);
    auto loaded = pl.load();
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(PersistenceTest, AutoSaveAndDisable) {
    PersistenceLayer pl(test_path);
    int call_count = 0;
    pl.set_auto_save_interval(std::chrono::seconds(1));
    pl.enable_auto_save([&call_count]() -> json {
        call_count++;
        return {{"auto", call_count}};
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    pl.disable_auto_save();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_GE(call_count, 1);
    auto loaded = pl.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_TRUE((*loaded).contains("auto"));
}

TEST_F(PersistenceTest, PathAccessor) {
    PersistenceLayer pl(test_path);
    EXPECT_EQ(pl.path(), test_path);
}

TEST_F(PersistenceTest, OverwriteExisting) {
    PersistenceLayer pl(test_path);
    pl.save({{"version", 1}});
    pl.save({{"version", 2}});
    auto loaded = pl.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["version"], 2);
}

TEST_F(PersistenceTest, SaveComplexNestedData) {
    PersistenceLayer pl(test_path);
    json data = {
        {"tasks", json::array({
            {{"id", "t1"}, {"title", "Task 1"}, {"nested", json::object({{"deep", json::array({1, 2, 3})}})}}
        })},
        {"config", json::object({{"flags", json::array({"a", "b"})}})}
    };
    EXPECT_TRUE(pl.save(data));
    auto loaded = pl.load();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ((*loaded)["tasks"][0]["nested"]["deep"].size(), 3u);
}
