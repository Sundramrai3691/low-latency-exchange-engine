#pragma once

#include "order.hpp"
#include "order_book.hpp"
#include "trade.hpp"

#include <vector>
#include <cstdint>

class MatchingEngine {
public:
    std::vector<Trade> process(Order order);
    bool tryProcess(Order order, std::vector<Trade>& trades);
    bool cancelOrder(uint64_t id);
    bool modifyOrder(uint64_t id, Side side, uint32_t price,
                     uint32_t quantity, std::vector<Trade>& trades);
    const OrderBook& book() const { return book_; }

private:
    OrderBook book_;
    void fillAgainst(Order& aggressor, std::vector<Trade>& trades);
};
