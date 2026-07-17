# MultiFiberMailbox — multi-agent coordination

**Issues:** #1585 (production), #1211 (Phase 1 scaffold)  
**API:** `src/serve/multi_fiber_mailbox.h`  
**Module inventory:** `src/serve/multi_fiber_mailbox.ixx` (phase tag only)

## Capabilities

| Feature | API |
|---------|-----|
| Multi-attach | `attach(Fiber*)` / `detach` |
| Priority queue | High/Critical → front; Normal/Low → back |
| Push + backpressure | `push` → `Ok` / `Backpressure` / `Closed` at `high_water` |
| Broadcast | `broadcast` (single msg, wake all) / `broadcast_fanout` (per-attacher copy) |
| Blocking recv | `recv(wait, timeout_ms, for_fiber)` — yields `BlockingIO`/`Explicit` |
| Stats | process-wide `g_mf_mailbox_stats` + `query:mf-mailbox-stats` (schema 1585) |

## Steal / GC

`recv` parks with `Fiber::yield(BlockingIO|Explicit)` so work-steal and GC
safepoints remain available while waiting.

## Agent surface

```
(engine:metrics "query:mf-mailbox-stats")
;; pushes pops broadcasts backpressure-rejects attaches phase schema
```

## Tests

`tests/test_multi_fiber_mailbox.cpp` — priority, broadcast, timeout, backpressure,
concurrent fibers, stats primitive.
