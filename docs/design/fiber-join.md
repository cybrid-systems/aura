# Fiber::join — structured multi-fiber coordination

**Issue:** #1584  
**Surfaces:** `src/serve/fiber.h`, `src/serve/fiber.cpp`, `Scheduler::add_joiner` / `on_fiber_done`

## Problem

EDSL `(fiber:join fid)` already blocked on completion (#119), but C++ callers
lacked a first-class `Fiber::join` with timeout/cancel for multi-agent
orchestration frameworks.

## API

```cpp
enum class JoinStatus { Ok, Timeout, Cancelled, Invalid };
struct JoinResult { JoinStatus status; uint64_t wait_us; };

static JoinResult Fiber::join(Fiber* target,
                              std::optional<uint64_t> timeout_ms = nullopt);
static JoinResult Fiber::join(std::span<Fiber* const> targets,
                              std::optional<uint64_t> timeout_ms = nullopt);

void Fiber::request_cancel() noexcept;
bool Fiber::is_cancel_requested() const noexcept;
```

## Semantics

| Path | Behavior |
|------|----------|
| Target already `Done` | Immediate `Ok` |
| Fiber context + scheduler | `add_joiner` + `BlockingIO` yield (or Explicit yields with timeout) |
| Host thread | 1 ms poll until Done / timeout |
| Joiner cancel | `Cancelled` (joiner's `request_cancel`) |
| null / self-join | `Invalid` |

On target completion, `Scheduler::on_fiber_done` wakes registered joiners via
eventfd (existing #119 path). Join yields `BlockingIO` / `Explicit` so
work-steal and GC safepoints remain available.

## Metrics

`Fiber::join_total`, `join_timeout_total`, `join_cancel_total`,
`join_wait_us_total`, `join_wait_us_max`.

## Tests

`tests/test_fiber_join.cpp` — single, batch, timeout, cancel, invalid, multi-worker.

## Related

- EDSL `(fiber:join)` — evaluator_primitives_messaging.cpp (#119)
- #1595 linear ownership on join path (follow-up)
