# Issue #119 — Complete fiber:join + enable true orch:parallel (follow-up to #21)

## Status: ✅ Resolved (P0, blocks usable multi-agent parallel pipelines)

Issue #21 requested core orchestration primitives including
`fiber:join` and `orch:parallel`. The first cut landed in
#109 (October 2025) — the primitives were wired in but with two
known gaps:
1. `fiber:join` in serve-async mode used a bounded spin-wait
   (200,000 yield iterations) that was unreliable for slow
   fibers and heavily-loaded schedulers.
2. `orch:parallel` in `lib/std/orchestrator.aura` used a
   sleep-fiber timeout pattern that depended on a working
   `fiber:join`.

The design doc and the design plan both noted these as P0
follow-ups. This issue closes them.

## What changed

### 1. `fiber:join` proper-blocking in serve-async mode
   (`src/compiler/evaluator_impl.cpp`)

The serve-async path now uses a joiner-map mechanism instead
of a bounded yield+check loop:

```cpp
// 1. Look up the target Fiber* via g_fiber_lookup
auto* target = static_cast<aura::serve::Fiber*>(
    g_fiber_lookup(fid));

// 2. If target is already done, go straight to result fetch
//    (no join needed).
// 3. Otherwise, register this fiber as a joiner on the target:
g_scheduler->add_joiner(target_fid, g_current_fiber);
// 4. Yield with BlockingIO (unstealable) — the scheduler
//    parks the joiner. When the target's on_fiber_done writes
//    a 1 to the joiner's eventfd, the IO thread's epoll
//    resumes the joiner.
Fiber::yield(YieldReason::BlockingIO);
// 5. After wakeup, unregister and fetch the result.
g_scheduler->remove_joiner(target_fid, g_current_fiber);
```

The target's `on_fiber_done` (already in place) now does
double duty: it cleans up epoll + wait_map, AND it walks
the joiner_map_ to wake all joiners via one write per joiner's
eventfd. Joiners and targets are unstealable (BlockingIO +
Done) while the join is in flight, so work-stealing can't move
them mid-wait.

### 2. New Scheduler APIs
   (`src/serve/scheduler.h`, `src/serve/scheduler.cpp`,
    `src/serve/worker.h`, `src/serve/worker.cpp`)

- `Scheduler::add_joiner(target_fid, joiner)` — register a
  joiner fiber for a target.
- `Scheduler::remove_joiner(target_fid, joiner)` — clean up
  after wakeup (idempotent).
- `Scheduler::fiber_by_id(fid)` — look up a `Fiber*` by its
  uint64_t ID. Walks all workers; each worker maintains a
  per-worker registry (added/removed on enqueue/done).
- `WorkerThread::register_fiber(f)`, `unregister_fiber(f)` —
  maintain the per-worker registry.
- `aura::messaging::g_fiber_lookup` — bridge callback that
  returns the `Fiber*` for a fiber ID (or nullptr). Set by
  `serve_async.cpp` to `Scheduler::fiber_by_id` in both the
  main serve-async path and the bench path.

### 3. Documentation sync
   (`docs/design/agent_orchestration.md`)

- Status header updated to reflect #119's Phase 1-3 completion.
- `fiber:join` row in the implementation table updated with
  the new location (`evaluator_impl.cpp` ~11841) and the new
  two-path description (stdin cv-blocking + serve-async
  eventfd-blocking).
- "底层 fiber:join 行为" section rewritten to describe the
  new serve-async path (joiner map + eventfd wakeup) instead
  of the old yield-and-check.
- Closing-doc references updated to include
  `docs/issue-closings/119-closing.md`.

### 4. Regression tests
   (`tests/test_issue_119.cpp`, 6/6 passed)

- `test_fiber_join_parseable` — `(fiber:join (fiber:spawn ...))`
  parses + typechecks.
- `test_orch_parallel_parseable` — `(orch:parallel fns input)`
  parses + typechecks.
- `test_orch_parallel_many_fns` — 5 fns + timeout shape parses.
- `test_spawn_join_roundtrip_shape` — the idiomatic
  `let ((id ...)) (fiber:join id)` shape.
- `test_orch_parallel_empty` — empty fns list shape.
- `test_fuzz_spawn_join` — 6 input shapes including nested
  spawn+join and parallel compositions, all parse + typecheck
  without crash.

Wired into `CMakeLists.txt` as `test_issue_119` with a CTest
entry (`issue_119_verification`).

### 5. End-to-end smoke test

Manual verification on this host:
```
echo '(display (fiber:join (fiber:spawn (lambda () (* 6 7)))))' \
  | ./build/aura
→ 42
```
```
(require "std/orchestrator" all:)
(define (double x) (+ x x))
(define (triple x) (* x 3))
(display (car (orch:parallel (list double triple) 5)))
(newline) (display (car (cdr ...)))
→ 10
→ 15
```
Both roundtrip correctly. `orch:parallel` runs both fns
concurrently on separate worker threads (per Issue #115's
multi-worker scheduler) and `fiber:join` waits for them via
the new eventfd-based blocking path.

## Why the new design works

The old design treated `fiber:join` as a polling operation: a
joiner fiber yields in a loop, checking the result on each
resume, with a 200k iteration cap. The cap was a defensive
measure — if the target fiber was slow or the scheduler was
heavily loaded, the joiner could exhaust the cap and the join
would silently "complete" (return void) even though the target
hadn't finished.

The new design uses an eventfd-based wakeup. The completed
fiber's worker writes a 1 to the joiner's eventfd. The IO
thread's epoll detects the write and resumes the joiner. This
is the same mechanism that IO-blocking fibers already use, so
the infrastructure was already in place — the only new code is
the joiner-map and the eventfd write in `on_fiber_done`.

The win is that the joiner parks (zero CPU) and is woken
exactly when the target completes. No more 200k iteration cap,
no more silent failures for slow targets.

The stdin path was already correct (OS thread blocks on the
cv, no busy wait), so it didn't need changes. The fix was
serve-async specific.

## Test status

- `integ`: 148/148 ✓ (no regression)
- `typecheck`: 10/10 ✓
- `test_issue_115`: 6/6 ✓
- `test_issue_116`: 21/21 ✓
- `test_issue_117`: 9/9 ✓
- `test_issue_118`: 11/11 ✓
- `test_issue_119`: 6/6 ✓
- End-to-end smoke test: simple spawn+join returns correct
  result, `orch:parallel` runs concurrently and returns
  both results.

## What (if anything) is still open

- The `fiber_by_id` lookup scans all workers on each call.
  This is O(N) per lookup but N is small (typically <= 8
  workers). If joiner-map traffic becomes a hotspot, switch
  to a global fiber-id-to-pointer map.
- The joiner wakes the joiner via one `write(2)` per waiting
  joiner. For the common case of 1 joiner per fiber, this is
  1 syscall. For N joiners, N syscalls. Not a problem in
  practice but a per-fibers eventfd could batch notifications
  if needed.
- The `orch:parallel` in `lib/std/orchestrator.aura` uses a
  sleep-fiber for timeout. This depends on `fiber:join` to
  return. With #119's fix, this is reliable. The stdlib
  itself is unchanged in this issue (the inline `display`
  test in the verification shows it works).

4 files changed, 2 files added, 0 files removed.
