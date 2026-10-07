#include "gateway/engine_worker.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

TEST(GatewayEngineWorker, RunsQueuedNewModifyAndProducesTrades) {
    using Worker = GatewayEngineWorker<16>;
    Worker::CommandQueue commands;
    Worker::EventQueue events;
    Worker worker(commands, events);
    worker.start();

    GatewayCommand command{};
    command.kind = GatewayCommandKind::New;
    command.session = 4;
    command.request_id = 1;
    command.order = Order{10, Side::Sell, OrderType::Limit, 100, 5, 1};
    ASSERT_TRUE(commands.push(command));

    command.kind = GatewayCommandKind::Modify;
    command.request_id = 2;
    command.order_id = 10;
    command.order = Order{10, Side::Sell, OrderType::Limit, 101, 3, 2};
    ASSERT_TRUE(commands.push(command));

    command.kind = GatewayCommandKind::New;
    command.request_id = 3;
    command.order = Order{11, Side::Buy, OrderType::Limit, 101, 3, 3};
    ASSERT_TRUE(commands.push(command));

    std::vector<GatewayEvent> received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.size() < 4 && std::chrono::steady_clock::now() < deadline) {
        GatewayEvent event{};
        if (events.pop(event)) received.push_back(event);
        else std::this_thread::yield();
    }
    ASSERT_EQ(received.size(), 4u);
    EXPECT_EQ(received[0].kind, GatewayEventKind::Acknowledgement);
    EXPECT_TRUE(received[0].accepted);
    EXPECT_EQ(received[0].request_id, 1u);
    EXPECT_TRUE(received[1].accepted);
    EXPECT_EQ(received[1].request_id, 2u);
    EXPECT_TRUE(received[2].accepted);
    EXPECT_EQ(received[2].request_id, 3u);
    ASSERT_EQ(received[3].kind, GatewayEventKind::Trade);
    EXPECT_EQ(received[3].trade.sell_order_id, 10u);
    EXPECT_EQ(received[3].trade.buy_order_id, 11u);
    EXPECT_EQ(received[3].trade.price, 101u);

    worker.requestStop();
    worker.join();
    EXPECT_TRUE(worker.finished());
}

TEST(GatewayEngineWorker, RejectsDuplicateAndMissingCancel) {
    using Worker = GatewayEngineWorker<8>;
    Worker::CommandQueue commands;
    Worker::EventQueue events;
    Worker worker(commands, events);
    worker.start();

    GatewayCommand command{};
    command.kind = GatewayCommandKind::New;
    command.session = 1;
    command.request_id = 1;
    command.order = Order{5, Side::Buy, OrderType::Limit, 100, 1, 1};
    ASSERT_TRUE(commands.push(command));
    command.request_id = 2;
    ASSERT_TRUE(commands.push(command)); // same live order ID
    command.kind = GatewayCommandKind::Cancel;
    command.request_id = 3;
    command.order_id = 999;
    ASSERT_TRUE(commands.push(command));

    bool accepted[3] = {};
    size_t seen = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (seen < 3 && std::chrono::steady_clock::now() < deadline) {
        GatewayEvent event{};
        if (events.pop(event) && event.kind == GatewayEventKind::Acknowledgement) {
            accepted[seen++] = event.accepted;
        } else {
            std::this_thread::yield();
        }
    }
    ASSERT_EQ(seen, 3u);
    EXPECT_TRUE(accepted[0]);
    EXPECT_FALSE(accepted[1]);
    EXPECT_FALSE(accepted[2]);
    worker.requestStop();
    worker.join();
}
