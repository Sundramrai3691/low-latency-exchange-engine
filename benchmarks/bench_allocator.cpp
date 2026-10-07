#include "pool_allocator.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

using Clock = std::chrono::steady_clock;
static constexpr size_t ITERATIONS = 500'000;

struct AllocationRecord {
    uint64_t id;
    uint64_t price;
    uint32_t quantity;
    uint32_t flags;
};

#if defined(_MSC_VER)
#  define BENCH_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#  define BENCH_NOINLINE __attribute__((noinline))
#else
#  define BENCH_NOINLINE
#endif

static BENCH_NOINLINE AllocationRecord* heapAllocate(uint64_t id) {
    return new AllocationRecord{id, id * 4, 10, 0};
}

static BENCH_NOINLINE void heapRelease(AllocationRecord* record) {
    delete record;
}

struct Result {
    double operations_per_second;
    int64_t p50;
    int64_t p95;
    int64_t p99;
    int64_t p999;
};

template <typename AllocateAndRelease>
static Result measure(AllocateAndRelease operation) {
    std::vector<int64_t> latencies;
    latencies.reserve(ITERATIONS);
    volatile uint64_t sink = 0;
    const auto start = Clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        const auto op_start = Clock::now();
        sink += operation(static_cast<uint64_t>(i));
        const auto op_end = Clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
            op_end - op_start).count());
    }
    const auto end = Clock::now();
    std::sort(latencies.begin(), latencies.end());
    const auto pct = [&](size_t n, size_t d) {
        return latencies[(latencies.size() - 1) * n / d];
    };
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (sink == UINT64_MAX) std::cout << "";
    return {static_cast<double>(ITERATIONS) / seconds,
            pct(50, 100), pct(95, 100), pct(99, 100), pct(999, 1000)};
}

static void print(const char* label, const Result& r) {
    std::cout << label << ": " << std::fixed << std::setprecision(2)
              << r.operations_per_second / 1e6 << " M alloc/free pairs/s"
              << ", p50/p95/p99/p99.9=" << r.p50 << '/'
              << r.p95 << '/' << r.p99 << '/' << r.p999 << " ns\n";
}

int main() {
    PoolAllocator<AllocationRecord, 1024> pool;
    auto heap = measure([](uint64_t i) {
        auto* record = heapAllocate(i);
        const uint64_t id = record->id;
        heapRelease(record);
        return id;
    });
    auto pooled = measure([&](uint64_t i) {
        AllocationRecord* record = pool.construct(AllocationRecord{i, i * 4, 10, 0});
        if (!record) return uint64_t{0};
        const uint64_t id = record->id;
        pool.destroy(record);
        return id;
    });

    std::cout << "Allocation benchmark: " << ITERATIONS
              << " single-object allocate/use/free operations; timing includes clock reads.\n";
    print("Heap new/delete", heap);
    print("Pool construct/destroy", pooled);
    return 0;
}
