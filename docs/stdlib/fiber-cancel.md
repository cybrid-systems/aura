# fiber:cancel — single-fiber cooperative cancellation (Issue #4708)

`(fiber:cancel fiber-id)` requests cancellation of **one** fiber by handle.
Before this a host could only poll a shared `stop` flag between turns (a
worker stopped mid-turn kept its claim) or use `(orch:scope-cancel-all)`,
which is scope-wide.

## Return values

| Result | Meaning |
|--------|---------|
| `#t` | Request accepted for a live fiber (scheduler **or** thread backend) |
| `#f` | No live fiber with that id (unknown / already done) |
| error object | Bad argument, or capability denial under sandbox (`fiber` / `*`) |

## Semantics

Cancellation is **cooperative**. The target observes the request at its next
safepoint and raises a catchable condition there:

- `(fiber:yield)` — the primary safepoint (checked before the hand-off);
- the fiber-parking channel prims (`channel:send` / `channel:recv`) — a
  parked fiber is nudged with a 1-byte eventfd write and re-checks the
  request when the scheduler resumes it;
- the `fiber:join` wait — a cancelled **joiner** stops waiting instead of
  parking until the target finishes.

The condition payload is the string
`"fiber-cancelled: cancel requested via (fiber:cancel)"`, so `try`/`catch`
cleanup runs on the fiber's own stack:

```scheme
(define (spin) (let l ((i 0)) (begin (fiber:yield) (l (+ i 1)))))
(define f (fiber:spawn (lambda () (try (spin) (catch (e) (list "caught" e))))))
(fiber:cancel f)          ; => #t
(fiber:join f)            ; => (caught fiber-cancelled: cancel requested via (fiber:cancel))
```

An **uncaught** cancel condition is the fiber body's result, so
`(fiber:join f)` returns an error value instead of waiting for the 200s
ceiling.

## Delivered once per request

A request is delivered at most once (`kPending` → `kDelivered`); a second
`(fiber:cancel f)` re-arms it, so a body that catches the condition and
keeps running can be interrupted again.

## Documented limitation

A body that never reaches a safepoint (a mid-turn block on a non-fiber wait,
or a tight loop with no yield) cannot be interrupted — there is no async
unwind across a running stack. This is the same cooperative protocol
documented for #2153; hosts that need hard deadlines should combine
`fiber:cancel` with the orphan-reclaim path.

## Backends

| Backend | Probe | Path |
|---------|-------|------|
| **scheduler** (`--serve-async`) | `(fiber:spawn-backend)` → `1` | `Fiber::request_cancel()` + eventfd nudge |
| **thread** (CLI file/stdin) | `(fiber:spawn-backend)` → `2` | per-fiber cancel registry + `s_fiber_results_cv` notify |

Implementation: `src/compiler/evaluator_primitives_messaging.cpp`.
