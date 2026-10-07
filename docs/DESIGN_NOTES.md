# Design notes: existing baseline

These notes describe the implementation currently in `src/`. This is a source walkthrough, not a claim that the code is production-ready. Operations mentioned below refer to the current implementation and assume successful allocation and valid input.

## Order representation

`Order` is a small value object with a 64-bit ID, side, order type, 32-bit tick price, 32-bit remaining quantity, and 64-bit timestamp. Integer prices avoid floating-point comparisons. Price zero is the convention for a market order, but the engine chooses market behavior from `OrderType`; it does not use the price field to decide that.

The timestamp is retained in the order, but the book does not compare timestamps. FIFO is based on the order in which `addOrder` appends an order to a level. This distinction matters if replay or multiple producers are introduced: the current code relies on the calling order for time priority.

**Invariant:** a resting order has positive remaining quantity and appears once in exactly one side/price level and once in the ID index.

**Failure modes:** duplicate IDs overwrite the hash index while leaving an old order in its level; zero-quantity orders can be stored; invalid side/type enum values are not rejected. V3 also silently declines out-of-range prices or pool exhaustion.

## Price levels, FIFO, and book versions

### V1: ordered maps and deques

`OrderBookV1` uses a descending `std::map` for bids, ascending `std::map` for asks, and a `std::deque<Order>` at each price. The first map entry is the best price. Appending is amortized O(1) after O(log L) map lookup/creation. Best bid/ask is O(1) at map begin. Cancellation first uses the expected O(1) hash index, then pays O(log L) for a level lookup and O(k) to scan the deque. Erasing an element can also shift later deque elements.

**Hot path:** map lookup/insertion and deque append. **Invariant:** deque order is arrival FIFO. **Failure modes:** duplicate IDs replace index metadata; `removeBidLevel` and `removeAskLevel` erase a level without repairing the order index.

### V2: ordered maps, linked FIFO levels, and pools

`OrderBookV2` stores `Limit*` values in the ordered maps. A `Limit` owns a doubly linked FIFO chain. The ID index stores a direct `OrderNode*`, so once a price level is found, unlinking is O(1). Add and cancel still perform O(log L) ordered-map operations; hash indexing is expected O(1). The fixed pools allocate nodes and levels in O(1) but have finite capacity.

**Hot path:** map find/insert, pool free-list pop, list append, and hash index update. **Invariant:** node links and `count` match the chain; every indexed node belongs to the indicated level. **Failure modes:** capacity exhaustion is guarded by `assert`; in a release build assertions disappear, so a null node/level can be dereferenced. Map insertion/allocation exceptions are not rolled back transactionally.

### V3: price arrays, active-price bitsets, and pools

The active `OrderBook` replaces map lookup with arrays sized for 65,536 prices per side. A bit marks each non-empty price. Best ask scans bitset words from low to high and selects the least significant set bit; best bid scans high to low and selects the most significant set bit. Worst-case lookup is O(P/64), although nearby active levels often occupy few words. Add/cancel level access and linked-list unlink are O(1), plus expected O(1) hash indexing and occasional bitset scans when asking for the top price.

**Hot path:** hash lookup, pointer-array access, linked-list edits, pool free-list operations. **Invariant:** a side's bit is set iff its corresponding level pointer is non-null and non-empty; index entries point to live nodes. **Failure modes:** out-of-range price and exhausted pools silently drop new orders; duplicate IDs can corrupt index/book consistency; exceptions while inserting into the unordered map can leak the newly linked node from the logical index.

## Price-time matching

`MatchingEngine::process` takes an order by value and returns a `std::vector<Trade>`. A market order repeatedly consumes the best opposite order until it is filled or liquidity ends; a market remainder is discarded. A limit order first checks whether its price crosses the current opposite best. If it does, `fillAgainst` consumes best-price FIFO orders while the limit still crosses. Each trade uses the resting order's price. A remaining limit quantity is added to its own side.

With `m` resting orders consumed and `T` trades emitted, work is proportional to the repeated top-of-book queries, cancellations, and `T` vector insertions; V3's top lookup scans the active bitset. The returned vector can allocate. In the baseline there is no validation for duplicate IDs, zero quantity, or self-trade prevention.

**Invariant:** the engine copies a resting ID and execution price before cancellation because cancellation destroys the node; it does not dereference that pointer afterward. **Failure modes:** book insertion can silently drop a remainder while the caller receives no rejection; large fills can allocate in the trade vector; invalid/duplicate IDs can leave incorrect state.

## Pool allocator

`PoolAllocator<T,N>` reserves aligned slots and a free-index stack at construction. `allocate` pops one index; `deallocate` pushes the slot index back. Both are O(1). `construct` applies placement new and returns a slot on constructor failure. `destroy` invokes the destructor then returns the slot. It is fixed-capacity and not thread-safe; the book's single-caller assumption is necessary. The allocator does not verify that a pointer is currently allocated, so double deallocation can corrupt the free stack in release builds.

The allocator removes per-node heap allocation after construction for pooled types, but other operations (notably `unordered_map` growth and trade-vector growth) can allocate. Therefore the whole matching path is not allocation-free.

## SPSC queue and memory ordering

`SPSCQueue<T,N>` requires a power-of-two `N` and trivially copyable `T`; one slot is reserved, giving usable capacity `N-1`. Producer owns `write_`, consumer owns `read_`. Each thread loads its own index relaxed. The producer acquires `read_` before deciding whether a slot has been freed, writes the payload, then release-stores `write_`. The consumer acquire-loads `write_` before reading the payload, then release-stores `read_` to publish that the slot is reusable. The two acquire/release handoffs protect payload visibility and slot reuse.

The queue is SPSC only: multiple producers racing to write, or multiple consumers racing to read, violate its ownership assumptions. Full and empty are explicit `false` returns. `alignas(64)` separates the indices to reduce false sharing on common cache-line sizes; 64 bytes is a hardware assumption, not a language guarantee that every CPU uses that line size.

**Invariant:** only the producer advances `write_`; only the consumer advances `read_`; a slot is not read before publication or overwritten before consumption. **Failure modes:** violating the SPSC contract causes data corruption; callers that ignore `false` lose work; busy retries can consume CPU. `size()` is an instantaneous pair of atomic observations and should be treated as approximate during concurrent activity.

## Feed thread and shutdown

`FeedThread` runs a `std::function<bool(Order&)>` generator. A produced order is retried while the queue is full, and the stop flag is checked during this retry. `stop()` clears the running flag and joins. A generator that blocks cannot be interrupted by `stop()`. An exception escaping the thread function terminates the process. Calling `start()` twice without stopping first is not guarded and can fail when assigning to an already joinable `std::thread`.

Synthetic `main` uses the main thread as queue consumer and matching-engine caller; the mutable book is not shared. A stop while blocked on a full queue can abandon the just-generated order. The engine's own state is otherwise not synchronized and must have one caller at a time.

## ITCH parser

The parser reads a two-byte big-endian frame length, then dispatches on a message type byte. Integer fields are assembled from bytes in big-endian order. It recognizes A/F add, D delete, X cancel, E execution, and U replace. Unknown types return no message. `parse()` returns a vector for a supplied buffer; `parseFile()` streams through a 512-byte stack buffer and invokes a callback. Both paths use `parseOne`'s minimum body lengths. `toOrder()` divides ITCH price units by 100 to convert ten-thousandths of a dollar into cents.

**Invariant:** offsets and minimum sizes must match the wire format before field reads. **Failure modes:** malformed frame lengths and truncated input stop or skip without a structured error; any side byte other than `B` becomes Sell; F messages share A's minimum body length and ignore the trailing MPID; price conversion truncates finer-than-cent ticks. In `main.cpp`, only Add messages affect the book; D/X/E/U records are counted but not applied.

## Benchmarks

`bench_order_book` pre-generates deterministic operations and times each add/cancel separately while maintaining a 1,000-order window; it reports p50/p99/p99.9 once per version. Clock overhead is inside each sample. `bench_matching` warms a separate engine, then times one million pre-generated alternating passive/aggressive orders and reports throughput and mean time. `bench_spsc` performs warmup plus one measured million-item two-thread transfer for each of four queue implementations and reports throughput only. No benchmark currently reports repeated-run confidence, CPU affinity, or environment metadata from inside the executable. Results are sensitive to scheduling and clock implementation.

## Interview concepts to be ready to explain

- Price-time priority comes from best-price selection plus FIFO append order at each level.
- Direct node pointers turn per-level cancellation from a scan into constant-time unlinking, while a map can still dominate lookup complexity.
- A bounded SPSC ring uses producer/consumer ownership and acquire/release publication to transfer payloads without a mutex.
- Pool allocation bounds capacity and reduces allocator work, while hash maps and result vectors still allocate.
- Throughput is total work per time; percentile latency captures the distribution and tail behavior of individual operations.
- A benchmark is meaningful only with a reproducible workload, build mode, hardware/compiler context, warmup policy, repetitions, and honest reporting of timer overhead.

## Phase 2: queued single-writer pipeline

`ExchangePipeline<N>` reuses the existing `SPSCQueue<Order,N>` and `MatchingEngine`. The producer calls `submit`; `false` is an explicit full-queue result, so callers choose whether to retry, reject, or back off. The engine-owning thread calls `processOne`; only it touches the mutable book. This keeps locks out of matching. In the synthetic executable, `FeedThread` is the producer and main is the engine thread. Shutdown stops the producer and the consumer drains the known number of produced orders.

The queue remains SPSC. Adding producer threads directly is invalid; multiple client connections must be serialized by one gateway event-loop thread or use a different queue design. The feed wrapper spins on queue-full and is interruptible through its running flag. A caller that stops the feed while its queue remains full may abandon the already-generated order; the shutdown test verifies the thread joins rather than claiming that this pending item is delivered.

`bench_pipeline` compares direct matching with one producer feeding the bounded queue and one consumer matching on the benchmark thread. It records submission-to-post-processing latency and total throughput for 500,000 orders. The measured run is in the README and `docs/BASELINE.md`: direct was 3.19 M orders/s with p50 300 ns; queued was 2.61 M orders/s with p50 1.42 ms. Queueing delay dominates the tail in that run because producer/consumer scheduling allows the queue to build. The result shows this architecture's separation/backpressure tradeoff and does not claim a latency gain.

Interview concepts from this phase: SPSC ownership, release/acquire visibility, queue-full backpressure, single-writer state ownership, and the distinction between service time and queueing latency.
