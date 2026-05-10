#include <gtest/gtest.h>
#include "mcp_collab/uuid.hpp"
#include <set>
#include <thread>
#include <vector>

using namespace mcp_collab;

TEST(UUIDTest, GeneratesNonNull) {
    auto id = generate_uuid();
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.size(), 36); // 8-4-4-4-12 format
}

TEST(UUIDTest, FormatCorrect) {
    auto id = generate_uuid();
    // Format: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx where y is 8/9/a/b
    EXPECT_EQ(id[8], '-');
    EXPECT_EQ(id[13], '-');
    EXPECT_EQ(id[18], '-');
    EXPECT_EQ(id[23], '-');
    EXPECT_EQ(id[14], '4'); // version 4 UUID
    EXPECT_TRUE(id[19] == '8' || id[19] == '9' || id[19] == 'a' || id[19] == 'b');
}

TEST(UUIDTest, Uniqueness) {
    std::set<std::string> ids;
    for (int i = 0; i < 10000; ++i) {
        auto result = ids.insert(generate_uuid());
        EXPECT_TRUE(result.second) << "Duplicate UUID generated at iteration " << i;
    }
}

TEST(UUIDTest, ThreadSafety) {
    std::vector<std::string> results(1000);
    std::vector<std::thread> threads;
    for (int i = 0; i < 1000; ++i) {
        threads.emplace_back([&results, i]() { results[i] = generate_uuid(); });
    }
    for (auto& t : threads) t.join();
    std::set<std::string> unique(results.begin(), results.end());
    EXPECT_EQ(unique.size(), 1000);
}