
# Baseline architecture

```mermaid
flowchart LR
  subgraph Input
    Synthetic["Synthetic order generator"]
    Feed["FeedThread producer"]
    Queue["Bounded SPSCQueue<Order>"]
    ITCH["ITCHParser"]
    File["ITCH binary file"]
  end
  Synthetic --> Feed --> Queue
  File --> ITCH
  Queue --> Loop["ExchangePipeline::processOne"]
  ITCH --> Main
  Loop --> Engine["MatchingEngine, owned by consumer thread"]
  Main["Synchronous ITCH replay"] --> Engine
  Engine --> Book["OrderBook V3"]
  Book --> Levels["Price-indexed levels + active bitsets"]
  Book --> Index["Order ID index"]
  Book --> Pools["OrderNode / Limit pools"]
  Engine --> Trades["Trade vector returned to caller"]
```

## Components

- **Order model:** `Order` carries ID, side, limit/market type, integer price ticks, remaining quantity, and timestamp. `Trade` represents a match emitted by the matching engine.
- **Book variants:** V1 uses ordered price maps and deques. V2 uses ordered maps, FIFO linked lists, an ID index with node pointers, and pools. V3 uses direct arrays indexed by a bounded price domain, bitsets for active prices, an ID index, and pools. V3 is the book embedded in `MatchingEngine`.
- **Matching:** incoming orders consume the opposite best price while crossing conditions hold. Each price level is FIFO; trades use the resting order's price. Remaining limit quantity can rest on the book.
- **Allocation:** V2/V3 use fixed-capacity `PoolAllocator` instances for order nodes and price levels. The allocator itself has a free-list stack and is not thread-safe.
- **Concurrent synthetic input:** `FeedThread` is one producer for `SPSCQueue`; the main thread consumes orders and invokes the engine. The queue's full state is reported to the producer as `false`; `FeedThread` retries by spinning.
- **ITCH input:** `ITCHParser` converts selected binary message records. In file mode, `main.cpp` sends Add Order records to matching; other recognized records are counted only.
- **Outputs:** matching trades are accumulated by the caller. The baseline has no separate event stream, gateway, or engine-owned worker thread.

`ExchangePipeline<N>` is the queued single-writer path used by synthetic mode: one producer enqueues orders and one designated consumer calls `processOne`, which owns and mutates the matching engine. Queue-full is explicit as a false return; `FeedThread` retries and shutdown can interrupt that retry. This is a thread-affinity contract, not a class that creates its own engine thread. ITCH replay remains synchronous.

The queued path returns processing results to the caller thread. It has no independent response queue because the caller is also the engine thread in this phase. A network gateway needs a dedicated engine worker and a second SPSC event channel so the gateway thread can continue servicing sockets.
