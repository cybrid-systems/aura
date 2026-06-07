# Issue #113 — Thread-safe GC: Phase 1 (root source registration) — PARTIAL

## Status: 🟡 Partially closed — root source registration shipped; remaining work tracked

Issue #113 is a multi-week infrastructure piece (4 phases per the design
doc). This commit ships the most isolated piece: **Evaluator ↔ GC
collector wiring** (the part that was clearly a missing link in the
existing infrastructure). The other 3 phases remain.

## Commit

| Commit | Description |
|--------|-------------|
| `b4e42cf` | `feat(113): wire Evaluator as GC root source for thread-safe GC` |

## What was already in place (pre-#113)

Substantial work had been done before this commit:

- ✅ **GC coordinator skeleton**: `src/serve/gc_coordinator.h/cpp` (full
  class with `request()`, `collect()`, `mark_from_roots()`, `sweep()`,
  `Metrics`)
- ✅ **Safepoint state machine**: `GCPhase` + `WorkerGCState` in
  `src/serve/fiber.h` (line 116)
- ✅ **Safepoint check**: `Fiber::check_gc_safepoint()` in
  `src/serve/fiber.cpp` (line 26), called from `yield()` and
  `yield(reason)` (lines 135, 163)
- ✅ **Scheduler safepoint broadcast**: `Scheduler::request_gc_safepoint`,
  `wait_for_safepoint`, `resume_from_gc` in `src/serve/scheduler.cpp`
  (lines 397-475)
- ✅ **GC collector lifecycle**: `Scheduler::gc_collector_` created in
  scheduler constructor; `metrics()` accessor
- ✅ **Messaging bridge hooks**: `g_gc_collect`, `g_gc_flush_root_set`,
  `g_heap_mutex` typedefs in `messaging_bridge.h`
- ✅ **Aura-level integration**: `gc-heap` primitive calls
  `g_gc_collect` when a collector is available (evaluator_impl.cpp
  ~13620)
- ✅ **Tests for the safepoint + coordinator**: `test_gc_safepoint_all_stop`,
  `test_gc_coordinator_basic`, `test_gc_root_no_sources`, etc. in
  `tests/test_concurrent.cpp` (20+ GC tests, all passing)
- ✅ **Binary runtime drops**: `aura_drop_pair`, `aura_drop_cell`,
  `aura_drop_closure`, `aura_alloc_string` in `lib/runtime.c` (P0-1
  from `binary_runtime_plan.md`)

## What was missing (this commit's contribution)

The most critical missing piece: **the Evaluator never registered itself
as a root source**. The `g_gc_flush_root_set` callback in
`serve_async.cpp` was a no-op stub. Without it:

- The GC could run end-to-end (safepoint → request → wait → resume)
- The mark step would have an empty root set
- The sweep would have nothing to compact
- Memory would still leak (the very thing #113 is supposed to fix)

This commit closes that gap:

1. **`Evaluator::flush_gc_roots(void* root_set_out)`** — walks
   `string_heap_`, `pairs_`, and `closures_` (filtered by
   `gc_safe_closure_id_` watermark) and pushes indices into the
   `aura::serve::GCRootSet`. Holds `heap_mutex()` so a non-fiber
   thread in serve-async mode can't race a concurrent
   `string_heap_.push_back` (or similar) with the walk.

2. **`Evaluator::gc_root_count()`** — cheap version: just returns the
   number of entries that would be marked, without allocating the
   `GCRootSet`. Used for pre-GC metrics and for tests.

3. **`g_gc_flush_root_set` callback wiring** in `serve_async.cpp` —
   now actually calls the active CompilerService's
   `evaluator.flush_gc_roots(...)`. Previously a no-op.

4. **Tests** — `tests/test_gc_evaluator_integration.cpp` (6 tests,
  all pass) plus CMakeLists.txt target.

## What's NOT shipped (remaining #113 work)

The full #113 has 3 more sub-tasks tracked in
`docs/design/thread_safe_gc.md`. Order from smallest to largest:

### Phase 2: Multi-session root registration

The current wiring flushes only the *active* session's evaluator
(via `g_current_compiler_service`). Multi-session setups need each
session's evaluator registered as a separate root source, keyed
by session_id. The `GCCollector::register_root_source` API already
supports this; the integration in `serve_async.cpp` needs to walk
the session map and register one source per session.

Estimated: 1 day.

### Phase 3: sweep_fn for vector-heap compaction

After marking, the GC needs to compact `string_heap_`, `pairs_`,
`closures_` by removing unmarked entries. The
`GCCollector::register_sweep_fn` API exists, and the
`GCSweepBuffers` struct has the mark bits, but the Evaluator
doesn't register a sweep callback. Without it, even with proper
marking, the vector heaps never shrink.

Estimated: 2-3 days.

### Arena alloc path call to `check_gc_safepoint()`

`ASTArena::allocate_raw` (in `src/core/arena.ixx`) is a hot path.
Inserting `check_gc_safepoint()` per allocation adds a `~1ns`
atomic load. The design doc recommends this so compute-heavy
fibers can't starve GC. Currently NOT called from the arena path
(only from `Fiber::yield`).

Estimated: 0.5 day + perf measurement.

### Binary runtime OpDrop → aura_drop_* lowering

Per `docs/design/binary_runtime_plan.md`: the LLVM lowering for
`--emit-binary` doesn't translate `OpDrop` instructions into calls
to `aura_drop_pair` / `aura_drop_cell` / `aura_drop_closure`.
Without this, the binary runtime path is append-only and leaks
every closure / pair / cell it allocates.

This is technically part of #113 (binary runtime ownership) but
is its own ~1 week sub-project with a separate design doc.

Estimated: 1 week.

## Verification

- `build/test_gc_evaluator_integration` — 6/6 pass
- `build/aura` — smoke test passes (`(+ 1 2)` → 3, `gc-heap` works)
- `build/test_concurrent` — pre-existing 20+ GC tests still pass
- No code changes to `src/serve/scheduler.cpp` or
  `src/serve/gc_coordinator.cpp` — the wiring is additive
  (Evaluator now feeds the coordinator, instead of the
  coordinator being inert)

## Future iterations

When work resumes on #113, the order should be:

1. **Phase 2 (multi-session)** — unblocks serve-async correctness
2. **Phase 3 (sweep_fn)** — turns the "no leak" claim from theory to practice
3. **Arena alloc-path hook** — minor perf + safety
4. **Binary OpDrop lowering** — separate sub-project

Each is a 0.5-2 day increment; together they fully close #113.
