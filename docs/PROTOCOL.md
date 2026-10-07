# TCP order gateway protocol

The gateway accepts one ASCII command per LF-terminated line. CRLF is accepted. Fields are separated by spaces. The maximum command line is 256 bytes excluding the line terminator. A connection may send multiple commands. Request sequence numbers must be positive and strictly increasing for each connection.

## Commands

```text
NEW <seq> <order_id> <B|S> <L|M> <price_ticks> <quantity>
CANCEL <seq> <order_id>
MODIFY <seq> <order_id> <B|S> <price_ticks> <quantity>
```

- IDs and sequence numbers are unsigned decimal integers greater than zero.
- Quantity must be in `[1, 2^32-1]`.
- Limit prices must be in `[1, 65535]` ticks. Market orders require price `0`.
- `MODIFY` applies to an existing resting order. It preserves the order ID and re-enters it at the new side/price/quantity. Re-entry loses its previous FIFO priority.
- Commands are processed in the order accepted by the single gateway event loop.

## Responses

```text
ACK <seq> ACCEPTED
ACK <seq> REJECTED
ACK <seq> REJECTED queue_full
TRADE <seq> <buy_order_id> <sell_order_id> <price_ticks> <quantity>
ERR <reason>
```

The engine sends the acknowledgement before trade reports for the same request. A market order can be acknowledged even when it finds no liquidity. A malformed command receives `ERR`; a syntactically valid command rejected by the matching engine receives a rejected acknowledgement. If the bounded command queue is full, the gateway rejects the request instead of silently dropping it. Trade and acknowledgement messages are routed to the connection that submitted the command. Responses for a disconnected client are discarded.

The server handles up to 32 concurrent clients with one `select` loop; all client parsing and queue submission happen on that one thread, preserving the SPSC producer contract. A response queue that fills backpressures the engine worker until the event loop drains it. Lines that exceed the limit receive `ERR line_too_long` when possible, then the connection closes. Non-increasing request sequences receive `ERR sequence_not_increasing`.

This protocol is intentionally small and is not FIX. The gateway does not provide authentication, TLS, persistence, replay protection across reconnects, multi-symbol routing, or production-grade exchange session semantics.
