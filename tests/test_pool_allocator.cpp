#include "pool_allocator.hpp"

#include <gtest/gtest.h>

struct LifetimeTracked {
    explicit LifetimeTracked(int value) : value(value) { ++constructed; }
    ~LifetimeTracked() { ++destroyed; }
    int value;
    static int constructed;
    static int destroyed;
};

int LifetimeTracked::constructed = 0;
int LifetimeTracked::destroyed = 0;

TEST(PoolAllocator, EnforcesCapacityAndReusesReleasedSlot) {
    PoolAllocator<int, 2> pool;
    int* first = pool.construct(11);
    int* second = pool.construct(22);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(pool.construct(33), nullptr);
    EXPECT_TRUE(pool.full());

    pool.destroy(first);
    EXPECT_EQ(pool.available(), 1u);
    int* reused = pool.construct(44);
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(*reused, 44);
    EXPECT_EQ(pool.used(), 2u);
    pool.destroy(second);
    pool.destroy(reused);
    EXPECT_TRUE(pool.empty());
}

TEST(PoolAllocator, CallsConstructorAndDestructorExactlyOnce) {
    LifetimeTracked::constructed = 0;
    LifetimeTracked::destroyed = 0;
    PoolAllocator<LifetimeTracked, 1> pool;
    auto* item = pool.construct(7);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->value, 7);
    EXPECT_EQ(LifetimeTracked::constructed, 1);
    EXPECT_EQ(LifetimeTracked::destroyed, 0);
    pool.destroy(item);
    EXPECT_EQ(LifetimeTracked::destroyed, 1);
}
