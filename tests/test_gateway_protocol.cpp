#include "gateway/gateway_protocol.hpp"

#include <gtest/gtest.h>

TEST(GatewayProtocol, ParsesNewCancelAndModify) {
    GatewayCommand command{};
    std::string error;

    ASSERT_TRUE(parseGatewayLine("NEW 17 123 B L 1000 25", 9, command, error));
    EXPECT_EQ(command.kind, GatewayCommandKind::New);
    EXPECT_EQ(command.session, 9u);
    EXPECT_EQ(command.request_id, 17u);
    EXPECT_EQ(command.order.id, 123u);
    EXPECT_EQ(command.order.side, Side::Buy);
    EXPECT_EQ(command.order.type, OrderType::Limit);
    EXPECT_EQ(command.order.price, 1000u);
    EXPECT_EQ(command.order.quantity, 25u);

    ASSERT_TRUE(parseGatewayLine("CANCEL 18 123", 9, command, error));
    EXPECT_EQ(command.kind, GatewayCommandKind::Cancel);
    EXPECT_EQ(command.order_id, 123u);

    ASSERT_TRUE(parseGatewayLine("MODIFY 19 123 S 1001 12", 9, command, error));
    EXPECT_EQ(command.kind, GatewayCommandKind::Modify);
    EXPECT_EQ(command.order.side, Side::Sell);
    EXPECT_EQ(command.order.price, 1001u);
    EXPECT_EQ(command.order.quantity, 12u);
}

TEST(GatewayProtocol, AcceptsCarriageReturnAndMarketOrder) {
    GatewayCommand command{};
    std::string error;
    ASSERT_TRUE(parseGatewayLine("NEW 1 2 B M 0 3\r", 1, command, error));
    EXPECT_EQ(command.order.type, OrderType::Market);
}

TEST(GatewayProtocol, RejectsMalformedAndOutOfRangeInput) {
    GatewayCommand command{};
    std::string error;
    EXPECT_FALSE(parseGatewayLine("NEW 1 2 B L 0 3", 1, command, error));
    EXPECT_EQ(error, "out_of_range");
    EXPECT_FALSE(parseGatewayLine("CANCEL 1", 1, command, error));
    EXPECT_FALSE(parseGatewayLine("NEW 1 2 X L 100 3", 1, command, error));
    EXPECT_FALSE(parseGatewayLine("NOPE 1", 1, command, error));
    EXPECT_FALSE(parseGatewayLine("NEW 1 2 B M 100 3", 1, command, error));
    EXPECT_FALSE(parseGatewayLine("NEW 1 2 B L 100 3 trailing", 1, command, error));
}

TEST(GatewayProtocol, RejectsOverflowAndZeroSequence) {
    GatewayCommand command{};
    std::string error;
    EXPECT_FALSE(parseGatewayLine("NEW 0 2 B L 100 3", 1, command, error));
    EXPECT_FALSE(parseGatewayLine("NEW 1 2 B L 100 4294967296", 1, command, error));
    EXPECT_FALSE(parseGatewayLine("CANCEL 1x 2", 1, command, error));
}
