# Concurrent Channel System

**Status**: Design
**Author**: Ani

## Why Channels

Aura currently has `fiber:spawn`/`fiber:join` but no way for concurrent
execution units to communicate mid-flight. Channels provide CSP-style
message passing: `channel:send` blocks until recv, `channel:recv` blocks
until send — no polling, no shared state.

Works in both stdin and serve modes (no fiber dependency).

## Design

```
;; Create a channel (returns channel-id)
(define ch (channel:create))

;; Send to channel (returns true, blocks if buffer full)
(channel:send ch "hello")

;; Receive from channel (blocks until message available)
(channel:recv ch)  → "hello"

;; Non-blocking try-receive (returns immediate or empty)
(channel:try-recv ch)

;; With buffer size (default 0 = rendezvous/synchronous)
(define ch-buf (channel:create 5))  ;; buffered, 5 items

;; Close channel (drains remaining, new recv returns empty)
(channel:close ch)
```

## Implementation

Backed by `std::mutex` + `std::condition_variable`:
- `channel:send` → locks mutex, pushes to deque, notifies recv
- `channel:recv` → locks mutex, waits on condvar if empty
- Buffered: `send` only blocks if full
- Unbuffered/rendezvous: `send` blocks until paired with `recv`
