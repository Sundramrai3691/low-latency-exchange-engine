#include "concurrency/feed_thread.hpp"
#include "exchange_pipeline.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

TEST(ExchangePipeline, FullQueueReportsBackpressureAndResumes) {
    ExchangePipeline<4> pipeline; // three usable entries
    EXPECT_TRUE(pipeline.submit(Order{1, Side::Sell, OrderType::Limit, 100, 2, 1}));
    EXPECT_TRUE(pipeline.submit(Order{2, Side::Buy, OrderType::Limit, 100, 1, 2}));
    EXPECT_TRUE(pipeline.submit(Order{3, Side::Sell, OrderType::Limit, 101, 1, 3}));
    EXPECT_FALSE(pipeline.submit(Order{4, Side::Buy, OrderType::Limit, 101, 1, 4}));

    ProcessedOrder result;
    ASSERT_TRUE(pipeline.processOne(result));
    EXPECT_EQ(result.order_id, 1u);
    EXPECT_TRUE(result.trades.empty());
    EXPECT_TRUE(pipeline.submit(Order{4, Side::Buy, OrderType::Limit, 101, 1, 4}));

    ASSERT_TRUE(pipeline.processOne(result));
    ASSERT_EQ(result.trades.size(), 1u);
    EXPECT_EQ(result.trades[0].buy_order_id, 2u);
    EXPECT_EQ(result.trades[0].sell_order_id, 1u);
}

TEST(ExchangePipeline, PreservesSingleWriterOrderAndTrades) {
    static constexpr size_t count = 2000;
    ExchangePipeline<256> pipeline;
    int generated = 0;
    auto generator = [&](Order& order) {
        if (generated == static_cast<int>(count)) return false;
        const uint64_t id = static_cast<uint64_t>(generated + 1);
        if (generated % 2 == 0) {
            order = Order{id, Side::Sell, OrderType::Limit, 100, 1, id};
        } else {
            order = Order{id, Side::Buy, OrderType::Limit, 100, 1, id};
        }
        ++generated;
        return true;
    };
    FeedThread<256> feed(pipeline.ingressQueue(), generator);
    feed.start();

    ProcessedOrder result;
    size_t processed = 0;
    size_t trades = 0;
    while (processed < count) {
        if (!pipeline.processOne(result)) {
            std::this_thread::yield();
            continue;
        }
        EXPECT_EQ(result.order_id, processed + 1);
        ++processed;
        trades += result.trades.size();
    }
    feed.stop();
    EXPECT_EQ(trades, count / 2);
}

TEST(FeedThread, StopUnblocksProducerSpinningOnFullQueue) {
    SPSCQueue<Order, 2> queue;
    ASSERT_TRUE(queue.push(Order{1, Side::Buy, OrderType::Limit, 100, 1, 0}));
    std::atomic<bool> generated{false};
    auto generator = [&](Order& order) {
        generated.store(true, std::memory_order_release);
        order = Order{2, Side::Buy, OrderType::Limit, 100, 1, 0};
        return true;
    };
    FeedThread<2> feed(queue, generator);
    feed.start();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!generated.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(generated.load(std::memory_order_acquire));
    feed.stop();
    EXPECT_FALSE(feed.running());
}
