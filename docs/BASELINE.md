# Baseline inspection

Inspection date: 2026-10-08 (Asia/Calcutta). Scope: checked-in source, CMake configuration, tests, benchmark programs, and the available local build environment. Phase 0 exposed and fixed two build issues: C++17-incompatible lambda captures and missing CTest registration. No matching-engine behavior was changed.

## Repository and targets

The project is a C++17 CMake project. It defines the static `engine` library, the `matching_engine` application, a Google Test executable, and three standalone benchmarks. Google Test is found as a system package or fetched at pinned v1.16.0 when unavailable. CMake presets describe the MinGW GCC environment. The `data/` directory is absent; there is no market-data file in this workspace.

| Area | Files | Baseline implementation |
|---|---|---|
| Order/trade model | `src/order.hpp`, `src/trade.hpp` | ID, side, type, integer price, remaining quantity, timestamp; trade output record. |
| Shared book nodes | `src/order_book_types.hpp` | `OrderNode` and FIFO `Limit` linked list. |
| V1 order book | `src/order_book_v1.hpp/.cpp` | Ordered maps of price to deque; ID index stores side and price. |
| V2 order book | `src/order_book_v2.hpp/.cpp` | Ordered maps of price to linked-list level; ID index includes direct node pointer; pools for nodes and levels. |
| V3 order book | `src/order_book.hpp/.cpp` | Fixed price-indexed arrays, active-price bitsets, ID-to-node index, and pools. Used by `MatchingEngine`. |
| Matching | `src/matching_engine.hpp/.cpp` | Cross incoming orders against the opposite best price, FIFO within level, emit trades, rest limit remainder. |
| Allocation | `src/pool_allocator.hpp` | Fixed-capacity, non-thread-safe free-list pool. |
| SPSC/feed | `src/concurrency/spsc_queue.hpp`, `feed_thread.hpp` | Bounded ring buffer and producer-thread wrapper. |
| ITCH | `src/itch/itch_parser.hpp/.cpp` | Selected ITCH messages; streaming file callback and in-memory vector parser. |
| Tests | `tests/*.cpp` | Books, matching, SPSC/feed, and ITCH parsing. |
| Benchmarks | `benchmarks/*.cpp` | Book add/cancel latency, direct matching throughput, and queue throughput variants. |

## Data structures and expected complexity

Let `L` be active price levels, `k` be orders at a price, and `P=65,536` be V3's price domain.

| Operation | V1 | V2 | V3 |
|---|---|---|---|
| Add | `O(log L)` map lookup plus amortized deque append; expected hash index | `O(log L)` map lookup plus `O(1)` list append and pool allocation; expected hash index | `O(1)` direct array access/list append/pool allocation; expected hash index |
| Cancel | Expected `O(1)` ID lookup, `O(log L)` level lookup, `O(k)` deque scan | Expected `O(1)` ID lookup, `O(log L)` level lookup, `O(1)` unlink | Expected `O(1)` ID lookup and node unlink |
| Best price | `O(1)` map begin | `O(1)` map begin | Worst-case `O(P/64)` bitset scan |
| Match incoming order | N/A in standalone book | N/A in standalone book | Iterates resting orders consumed; each top lookup includes V3 bitset scan |

Hash-map operations have expected, not worst-case constant, complexity. V3 accepts prices `[0, 65535]` and silently skips out-of-range or pool-exhausted additions.

## Concurrency model

The SPSC queue's producer owns `write_`; consumer owns `read_`. Producer acquires the consumer index before slot reuse, writes the payload, then release-publishes the write index. Consumer acquire-loads the write index before reading the payload, then release-publishes slot reuse. Full/empty are reported as `false`; usable capacity is template size minus one. The queue only supports one producer and one consumer.

`FeedThread` calls a generator on one producer thread and retries while the queue is full, checking a stop flag during retry. Synthetic `main` consumes on the main thread. ITCH replay is synchronous and bypasses the queue. There is no dedicated engine worker abstraction in the baseline.

## Existing benchmark methodology

- `bench_order_book`: pre-generates 600,000 deterministic orders, seeds a 1,000-order window, then times 599,000 add and cancel operations per version. It reports p50/p99/p99.9 and includes clock-read overhead. Versions run once and sequentially.
- `bench_matching`: pre-generates one million alternating resting sells and crossing buys; warms a separate engine with 20,000 orders; measures one direct pass; reports total elapsed, throughput, trades, and mean time.
- `bench_spsc`: one warmup plus one measured one-million-item transfer for each of four variants; reports throughput only. It does not pin threads or report latency percentiles.

## Build, tests, and actual baseline results

The PATH-selected `C:\mingw64\bin\cmake.exe` is unusable; the installed Kitware CMake 4.4.4 works by full path. GCC 16.1.0 (MinGW-w64 x86_64, POSIX threads), Ninja, and the local CMake were used for a Release configuration. Google Test was fetched at v1.16.0 because it was not installed. The compiler exposed C++20-only structured-binding lambda capture syntax in two C++17 source files; those were changed to ordinary local reads. `enable_testing()` was added because the original `ctest` discovered zero tests despite the test executable working.

Hardware reported: Intel Core i3-1115G4, 2 cores / 4 logical processors. OS query returned Windows but did not report a version. No affinity pinning or repeated runs. The results below are single exploratory runs and are not general performance claims.

`ctest --test-dir build/gcc-release --output-on-failure`: **55/55 passed**.

`bench_order_book`: 599,000 measured adds and cancels per version, 1,000 live orders, 21 price levels. Clock overhead is included.

| Book | Add p50 / p99 / p99.9 (ns) | Cancel p50 / p99 / p99.9 (ns) |
|---|---:|---:|
| V1 | 200 / 400 / 2,400 | 100 / 300 / 2,100 |
| V2 | 100 / 200 / 600 | 100 / 200 / 700 |
| V3 | 100 / 200 / 400 | 100 / 200 / 500 |

`bench_matching`: one million orders; 500,000 trades; 251.57 ms elapsed; 3.98 M orders/s; 251.6 ns/order mean.

`bench_spsc`: one warmup plus one measured million-item run per variant. A direct GCC `-O3 -pthread` build ran because Windows Application Control blocked the CMake-produced executable. Throughput: mutex 9.1 M msgs/s; seq_cst 27.3 M msgs/s; acquire/release 29.4 M msgs/s; acquire/release plus alignment 38.1 M msgs/s. Single unpinned run on a 2-core / 4-thread CPU; throughput only.

Reproduction configure used Kitware CMake with `-G Ninja -DCMAKE_C_COMPILER=C:/mingw64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/mingw64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build/gcc-release --parallel 4` and CTest. CMake Release produced the book and matching numbers; the SPSC command was `g++ -std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror -Isrc benchmarks/bench_spsc.cpp -pthread`.

## Known limitations observed in source

- No `LICENSE` file exists in this checkout. The upstream GitHub repository also currently returns 404 for `LICENSE`, so exact license text and copyright notice could not be recovered. The README carries the requested attribution wording; licensing should be verified before distribution.
- V3 silently ignores out-of-range prices and pool exhaustion; callers receive no rejection status.
- `MatchingEngine::process` returns a vector of trades, which can allocate. Pooling does not prevent unordered-map or result-vector allocations.
- Synthetic input uses a separate producer thread, while the main thread polls the queue and processes orders; both ends busy-spin under load.
- ITCH replay applies Add Order messages only; recognized delete/cancel/execute/replace records are counted but not applied to the book.
- ITCH file parser uses a 512-byte buffer; oversized messages are skipped, and a truncated body ends parsing without a structured error.
- The project is a single book; it does not model multi-symbol partitioning, exchange sessions, or network ingress.

## Next phase

Phase 1 is documented in [DESIGN_NOTES.md](DESIGN_NOTES.md). It explains order and level representation, V1/V2/V3 tradeoffs, matching, cancellation, allocation, memory ordering, feed lifecycle, ITCH parsing, and benchmark design. No Phase 1 implementation changes are needed beyond the small build-compatibility fixes noted above.
