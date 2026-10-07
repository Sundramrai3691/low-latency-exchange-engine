#pragma once

#include "concurrency/spsc_queue.hpp"
#include "matching_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

struct ProcessedOrder {
    uint64_t order_id{0};
    uint64_t submitted_at_ns{0};
    std::vector<Trade> trades;
};

// One producer submits orders; one engine thread calls processOne(). The
// engine and its mutable book never leave the consumer thread.
template <size_t QueueSize>
class ExchangePipeline {
public:
    using Queue = SPSCQueue<Order, QueueSize>;

    ExchangePipeline() = default;
    ExchangePipeline(const ExchangePipeline&) = delete;
    ExchangePipeline& operator=(const ExchangePipeline&) = delete;

    // Producer thread only. A false result means backpressure: retry or reject
    // at the caller. The pipeline does not silently discard a full-queue order.
    bool submit(const Order& order) noexcept { return ingress_.push(order); }

    // Matching-engine thread only. Returns false when there is no queued order.
    bool processOne(ProcessedOrder& result) {
        Order order{};
        if (!ingress_.pop(order)) return false;
        result.order_id = order.id;
        result.submitted_at_ns = order.timestamp;
        result.trades = engine_.process(order);
        return true;
    }

    Queue& ingressQueue() noexcept { return ingress_; }
    const MatchingEngine& engine() const noexcept { return engine_; }

private:
    Queue ingress_;
    MatchingEngine engine_;
};
