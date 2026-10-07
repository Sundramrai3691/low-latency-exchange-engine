# Benchmark methodology and measured results

## Environment and limits

Measurements collected on 2026-10-08 on an Intel Core i3-1115G4 (2 cores, 4 logical processors), GCC 16.1.0 MinGW-w64 POSIX threads, Kitware CMake 4.4.4, Ninja, Windows, Release configuration. The local OS version query did not return a version. No CPU affinity was set. Unless a section says otherwise, each benchmark was run once. These figures describe this machine and run, not portable performance guarantees.

The CMake project applies `-O2` to the engine library; benchmark translation units use `-O3`. Timer overhead remains in per-operation samples. The TCP benchmark was compiled directly with `-O3` for all listed source files because Windows Application Control blocked the CMake-produced benchmark executable. These methodology differences are retained in each result section.

## Existing order-book benchmark

Command: `build/gcc-release/bench_order_book.exe`. It pre-generates 600,000 deterministic orders, seeds 1,000 live orders across 21 levels, and times 599,000 add and cancel operations per version. It reports one p50/p99/p99.9 distribution for each operation. `high_resolution_clock` calls are included in each measurement.

| Book | Add p50 / p99 / p99.9 | Cancel p50 / p99 / p99.9 |
|---|---:|---:|
| V1 | 200 / 400 / 2,400 ns | 100 / 300 / 2,100 ns |
| V2 | 100 / 200 / 600 ns | 100 / 200 / 700 ns |
| V3 | 100 / 200 / 400 ns | 100 / 200 / 500 ns |

## Existing matching benchmark

Command: `build/gcc-release/bench_matching.exe`. It warms a separate engine with 20,000 orders, then times one million pre-generated alternating resting sells and crossing buys.

| Orders | Trades | Elapsed | Throughput | Mean per order |
|---:|---:|---:|---:|---:|
| 1,000,000 | 500,000 | 251.57 ms | 3.98 M orders/s | 251.6 ns |

## Existing SPSC benchmark

The CMake executable was blocked by Windows Application Control. A direct GCC build was made with `g++ -std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror -Isrc benchmarks/bench_spsc.cpp -pthread`; one warmup and one measured one-million-item pass per variant completed.

| Variant | Measured throughput |
|---|---:|
| Mutex queue | 9.1 M msgs/s |
| SPSC seq_cst | 27.3 M msgs/s |
| SPSC acquire/release | 29.4 M msgs/s |
| SPSC acquire/release + `alignas(64)` | 38.1 M msgs/s |

This is a single unpinned run; it reports throughput only.

## Direct versus queued engine pipeline

Command: `build/gcc-release/bench_pipeline.exe`. It processes 500,000 alternating orders in direct mode and through an `ExchangePipeline<4096>` with one producer thread and one engine consumer thread. Workload construction is outside timing. A monotonic timestamp is written before processing/enqueue and latency ends after `MatchingEngine::process` completes. Each operation includes clock reads. One measured pass per mode.

| Mode | Throughput | p50 | p95 | p99 | p99.9 | Trades |
|---|---:|---:|---:|---:|---:|---:|
| Direct baseline | 3.19 M orders/s | 300 ns | 500 ns | 600 ns | 1,000 ns | 250,000 |
| Queued single-writer | 2.61 M orders/s | 1.42 ms | 2.47 ms | 3.82 ms | 4.00 ms | 250,000 |

This run built a backlog and showed queueing delay dominating end-to-end latency. The queue provides bounded backpressure and producer/engine thread separation; it did not improve throughput or latency in this workload. The high tail is an observed outcome, not a target.

## TCP gateway and parser

Command used for the completed run: direct GCC build of `bench_gateway.cpp`, `tcp_server.cpp`, all engine sources, with `-std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror -Isrc -pthread -lws2_32`; then `bench_gateway_direct.exe 1000`.

The parser timing is a separate in-process loop over 1,000 `NEW` lines. The TCP measurement sends one `NEW` command and waits for its acknowledgement before sending the next. It therefore avoids intentional burst overflow and measures client-observed loopback TCP → parser → SPSC command queue → engine → SPSC response queue → acknowledgement latency. One pass, no warmup or affinity control.

| Metric | Measured result |
|---|---:|
| Parser p50 / p95 / p99 / p99.9 | 100 / 100 / 100 / 200 ns |
| Accepted TCP requests | 1,000 |
| Gateway throughput | 14.00 K requests/s |
| TCP-to-ACK p50 / p95 / p99 / p99.9 | 64.5 / 95.2 / 163.7 / 549.8 µs |

The simple `select` server is notified of engine responses through a loopback UDP wakeup socket. This result reflects the local Windows socket stack and one sequential client; it is not a multi-client saturation result.

## Re-running

```sh
cmake --preset gcc-release
cmake --build --preset gcc-release
ctest --preset gcc-release
./build/gcc-release/bench_order_book
./build/gcc-release/bench_matching
./build/gcc-release/bench_spsc
./build/gcc-release/bench_pipeline
./build/gcc-release/bench_gateway 1000
```

Collect several runs before drawing conclusions. Compare workloads, build flags, thread placement, and warmup policy consistently. Do not transplant these results to a different machine or workload.
