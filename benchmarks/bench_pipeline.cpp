#include "exchange_pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static constexpr size_t ORDER_COUNT = 500'000;
static constexpr size_t QUEUE_SIZE = 4096;

static uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count());
}

static std::vector<Order> makeOrders() {
    std::vector<Order> orders;
    orders.reserve(ORDER_COUNT);
    for (size_t i = 0; i < ORDER_COUNT; i += 2) {
        orders.push_back(Order{static_cast<uint64_t>(i + 1), Side::Sell,
                               OrderType::Limit,
                               static_cast<uint32_t>(100 + (i / 2) % 10),
                               10, 0});
        orders.push_back(Order{static_cast<uint64_t>(i + 2), Side::Buy,
                               OrderType::Limit, 115, 10, 0});
    }
    return orders;
}

struct Result {
    double throughput_mps;
    int64_t p50;
    int64_t p95;
    int64_t p99;
    int64_t p999;
    size_t trades;
};

static Result summarize(Clock::time_point start, Clock::time_point end,
                        std::vector<int64_t>& latencies, size_t trades) {
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](size_t numerator, size_t denominator) {
        return latencies[(latencies.size() - 1) * numerator / denominator];
    };
    const double elapsed = std::chrono::duration<double>(end - start).count();
    return {static_cast<double>(ORDER_COUNT) / elapsed / 1e6,
            percentile(50, 100), percentile(95, 100),
            percentile(99, 100), percentile(999, 1000), trades};
}

static Result runDirect(const std::vector<Order>& source) {
    MatchingEngine engine;
    std::vector<int64_t> latencies;
    latencies.reserve(source.size());
    size_t trades = 0;
    const auto start = Clock::now();
    for (auto order : source) {
        const uint64_t submitted = nowNs();
        order.timestamp = submitted;
        trades += engine.process(order).size();
        latencies.push_back(static_cast<int64_t>(nowNs() - submitted));
    }
    return summarize(start, Clock::now(), latencies, trades);
}

static Result runQueued(const std::vector<Order>& source) {
    ExchangePipeline<QUEUE_SIZE> pipeline;
    std::vector<int64_t> latencies;
    latencies.reserve(source.size());
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::thread producer([&]() {
        ready.store(true, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (auto order : source) {
            order.timestamp = nowNs();
            while (!pipeline.submit(order)) std::this_thread::yield();
        }
    });

    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    const auto start = Clock::now();
    go.store(true, std::memory_order_release);

    ProcessedOrder result;
    size_t processed = 0;
    size_t trades = 0;
    while (processed < source.size()) {
        if (!pipeline.processOne(result)) {
            std::this_thread::yield();
            continue;
        }
        ++processed;
        trades += result.trades.size();
        latencies.push_back(static_cast<int64_t>(nowNs() - result.submitted_at_ns));
    }
    producer.join();
    return summarize(start, Clock::now(), latencies, trades);
}

static void print(const char* name, const Result& r) {
    std::cout << name << ": " << std::fixed << std::setprecision(2)
              << r.throughput_mps << " M orders/s, trades=" << r.trades
              << ", latency p50/p95/p99/p99.9="
              << r.p50 << '/' << r.p95 << '/' << r.p99 << '/' << r.p999
              << " ns\n";
}

int main() {
    const auto orders = makeOrders();
    std::cout << "Single-writer pipeline benchmark; " << ORDER_COUNT
              << " orders; queue usable capacity=" << QUEUE_SIZE - 1
              << "; one timed pass per mode.\n";
    print("Direct baseline", runDirect(orders));
    print("Queued pipeline", runQueued(orders));
    return 0;
}
