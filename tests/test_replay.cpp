#include "matching_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace {

class ReferenceMatcher {
public:
    std::vector<Trade> process(Order incoming) {
        std::vector<Trade> trades;
        while (incoming.quantity != 0) {
            size_t best = orders_.size();
            for (size_t i = 0; i < orders_.size(); ++i) {
                const Order& candidate = orders_[i];
                if (candidate.side == incoming.side) continue;
                if (best == orders_.size() || better(candidate, orders_[best], incoming.side))
                    best = i;
            }
            if (best == orders_.size()) break;

            Order& resting = orders_[best];
            if (incoming.type == OrderType::Limit &&
                ((incoming.side == Side::Buy && incoming.price < resting.price) ||
                 (incoming.side == Side::Sell && incoming.price > resting.price))) {
                break;
            }

            const uint32_t quantity = std::min(incoming.quantity, resting.quantity);
            Trade trade{};
            trade.price = resting.price;
            trade.quantity = quantity;
            if (incoming.side == Side::Buy) {
                trade.buy_order_id = incoming.id;
                trade.sell_order_id = resting.id;
            } else {
                trade.buy_order_id = resting.id;
                trade.sell_order_id = incoming.id;
            }
            trades.push_back(trade);
            incoming.quantity -= quantity;
            resting.quantity -= quantity;
            if (resting.quantity == 0) {
                orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(best));
            }
        }
        if (incoming.type == OrderType::Limit && incoming.quantity != 0)
            orders_.push_back(incoming);
        return trades;
    }

    std::optional<uint32_t> bestPrice(Side side) const {
        std::optional<uint32_t> price;
        for (const auto& order : orders_) {
            if (order.side != side) continue;
            if (!price || (side == Side::Buy ? order.price > *price : order.price < *price))
                price = order.price;
        }
        return price;
    }

    std::optional<Order> front(Side side) const {
        const auto price = bestPrice(side);
        if (!price) return std::nullopt;
        for (const auto& order : orders_) {
            if (order.side == side && order.price == *price) return order;
        }
        return std::nullopt;
    }

    std::optional<Order> find(uint64_t id) const {
        auto it = std::find_if(orders_.begin(), orders_.end(),
            [&](const Order& order) { return order.id == id; });
        if (it == orders_.end()) return std::nullopt;
        return *it;
    }

private:
    static bool better(const Order& candidate, const Order& best, Side incoming_side) {
        if (incoming_side == Side::Buy) return candidate.price < best.price;
        return candidate.price > best.price;
    }

    std::vector<Order> orders_;
};

static bool sameTrade(const Trade& a, const Trade& b) {
    return a.buy_order_id == b.buy_order_id &&
           a.sell_order_id == b.sell_order_id &&
           a.price == b.price && a.quantity == b.quantity;
}

} // namespace

TEST(Replay, DeterministicStreamMatchesSimpleReference) {
    MatchingEngine engine;
    ReferenceMatcher reference;
    std::vector<Order> stream;
    stream.reserve(2000);
    uint64_t state = 0x5eed1234ULL;
    auto random = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    for (uint64_t i = 1; i <= 2000; ++i) {
        const Side side = (random() & 1u) ? Side::Buy : Side::Sell;
        const bool market = i % 17 == 0;
        const uint32_t price = market ? 0u
            : static_cast<uint32_t>(95u + random() % 11u);
        const uint32_t quantity = static_cast<uint32_t>(1u + random() % 8u);
        stream.push_back(Order{i, side,
            market ? OrderType::Market : OrderType::Limit,
            price, quantity, i});
    }

    std::vector<uint64_t> seen_ids;
    seen_ids.reserve(stream.size());
    for (const auto& order : stream) {
        const auto actual_trades = engine.process(order);
        const auto expected_trades = reference.process(order);
        ASSERT_EQ(actual_trades.size(), expected_trades.size()) << "order " << order.id;
        for (size_t i = 0; i < actual_trades.size(); ++i)
            EXPECT_TRUE(sameTrade(actual_trades[i], expected_trades[i]))
                << "order " << order.id << " trade " << i;

        if (order.type == OrderType::Limit) seen_ids.push_back(order.id);
        EXPECT_EQ(engine.book().bestBid(), reference.bestPrice(Side::Buy));
        EXPECT_EQ(engine.book().bestAsk(), reference.bestPrice(Side::Sell));
        const auto bid_front = engine.book().bestBidFront();
        const auto expected_bid = reference.front(Side::Buy);
        ASSERT_EQ(bid_front != nullptr, expected_bid.has_value());
        if (bid_front) {
            EXPECT_EQ(bid_front->id, expected_bid->id);
            EXPECT_EQ(bid_front->quantity, expected_bid->quantity);
        }
        const auto ask_front = engine.book().bestAskFront();
        const auto expected_ask = reference.front(Side::Sell);
        ASSERT_EQ(ask_front != nullptr, expected_ask.has_value());
        if (ask_front) {
            EXPECT_EQ(ask_front->id, expected_ask->id);
            EXPECT_EQ(ask_front->quantity, expected_ask->quantity);
        }
        for (const uint64_t id : seen_ids) {
            EXPECT_EQ(engine.book().findOrder(id).has_value(), reference.find(id).has_value())
                << "order ID " << id;
            const auto actual = engine.book().findOrder(id);
            const auto expected = reference.find(id);
            if (actual && expected) {
                EXPECT_EQ(actual->price, expected->price);
                EXPECT_EQ(actual->quantity, expected->quantity);
                EXPECT_EQ(actual->side, expected->side);
            }
        }
    }
}
