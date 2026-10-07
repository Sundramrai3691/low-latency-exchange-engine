# Low-Latency Exchange Engine

A C++17 limit-order-book and matching-engine project. The repository contains three order-book implementations, a price-time-priority matching engine, a fixed-capacity object pool, an SPSC ingress pipeline, and a parser for selected Nasdaq ITCH 5.0 messages.

The single-writer pipeline keeps the mutable book on the queue consumer thread. See [Baseline](docs/BASELINE.md), [Architecture](docs/ARCHITECTURE.md), [Design Notes](docs/DESIGN_NOTES.md), and the benchmark sources for details.

## Build and run

The project uses CMake 3.20+, C++17, and Google Test. If Google Test is not installed, CMake fetches pinned version 1.16.0. A GCC Release preset is provided:

```sh
cmake --preset gcc-release
cmake --build --preset gcc-release
ctest --preset gcc-release
./build/gcc-release/bench_order_book
./build/gcc-release/bench_matching
./build/gcc-release/bench_spsc
./build/gcc-release/bench_pipeline
./build/gcc-release/bench_gateway 1000
./build/gcc-release/matching_engine
```

The executable runs a synthetic workload with no arguments, or streams a supplied ITCH file. The repository does not include market-data files.

Start the TCP gateway with `./build/gcc-release/matching_engine --tcp 9000`. See [docs/PROTOCOL.md](docs/PROTOCOL.md) for the line format and responses.

## Project structure

- `src/order_book_v1.*`: `std::map` price levels containing `std::deque<Order>`.
- `src/order_book_v2.*`: `std::map` price levels containing FIFO linked lists and pooled nodes.
- `src/order_book.*`: direct price-indexed arrays, active-price bitsets, order index, and pools.
- `src/matching_engine.*`: incoming-order matching and trade generation.
- `src/pool_allocator.hpp`: fixed-capacity, non-thread-safe free-list pool.
- `src/concurrency/`: bounded SPSC ring buffer and producer-thread wrapper.
- `src/exchange_pipeline.hpp`: queue ingress and single-writer engine processing facade.
- `src/gateway/`: line protocol, command worker, and multi-client TCP gateway.
- `src/itch/`: selected ITCH message parsing and Add Order conversion.
- `tests/`: Google Test suites for order books, matching, SPSC/feed thread, ITCH parsing, pipeline, protocol, and TCP gateway behavior.
- `benchmarks/`: standalone order-book, matching, SPSC, direct-versus-queued pipeline, and TCP gateway benchmarks.

## Measured status and limits

The Release build and 65 tests pass on the recorded Windows/MinGW environment. The pipeline and TCP gateway benchmarks report measured throughput and latency percentiles. See [docs/BASELINE.md](docs/BASELINE.md) and [docs/BENCHMARKS.md](docs/BENCHMARKS.md) for actual run data and its limitations.

One measured run of `bench_pipeline` with 500,000 alternating limit orders on an Intel Core i3-1115G4 (GCC 16.1.0, Release) produced:

| Mode | Throughput | p50 | p95 | p99 | p99.9 |
|---|---:|---:|---:|---:|---:|
| Direct processing | 3.19 M orders/s | 300 ns | 500 ns | 600 ns | 1,000 ns |
| SPSC queued | 2.61 M orders/s | 1.42 ms | 2.47 ms | 3.82 ms | 4.00 ms |

The queue run showed substantial queueing delay under this producer/consumer scheduling pattern. These are single-run measurements, not a claim that the queue improves performance. The queue provides thread separation and bounded backpressure; whether that tradeoff is useful depends on the workload and scheduling.

This is a single-instrument book design with a finite price range in V3. The matching path returns a `std::vector<Trade>`, and the ITCH replay path currently sends only Add Order messages into the matching engine; parsed delete/cancel/execute/replace records are counted but do not update its book. The SPSC queue is strictly one-producer/one-consumer. Detailed limitations and observed behavior are recorded in the baseline document.

## Upstream attribution

This project is based on Skywalklau/matching-engine. The original MIT license and attribution are preserved. This repository extends the baseline with additional systems, networking, benchmarking, testing, and performance-engineering work.

**License file status:** no `LICENSE` file is present in the checked-out workspace. The attribution above records the requested upstream statement, but the original MIT license text cannot be verified as preserved from the files currently available. Restore/verify the upstream license text before distributing this repository.
