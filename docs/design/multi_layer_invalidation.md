# Multi-Layer Invalidation Strategy (Issue #166)

**Status:** Design document (Phase 1 of #166)
**Author:** #166 work
**Date:** 2026-06-12

## Problem

Aura's hybrid execution model (IR Pipeline as fast path + Tree-walker as
fallback) plus incremental compilation has a multi-layer cache invalidation
gap. The 5 affected layers and their current state:

| Layer | Cache/State | Current Invalidation | Gap |
|-------|-------------|----------------------|-----|
| AST | Type cache in `synthesize_flat` | `mark_dirty_upward()` via DefUseIndex | Many mutation paths still incomplete (see `incremental_dirty_propagation.md`) |
| IR | `ir_cache_` + `ir_cache_v2_` (source_hash + dirty) | `mark_define_dirty` + BFS on `dep_graph_` | Only define-level; does not cover IR internal Capture or runtime-created closures |
| JIT | `jit_cache_` (per-function native pointers) | `invalidate_function` | Not atomic with IR cache; no deopt for in-flight native execution |
| Runtime Closures | `runtime_closures_` (IRInterpreter) + Tree-walker `closures_` | None (bare pointers) | `ClosureBridgeData {flat*, pool*, body_id}` has lifetime tied to arena; mutation can invalidate pointers |
| Hybrid Boundary | `closure_bridge_` callback | `apply_closure` dispatch | No epoch/version to detect stale bridges |

The risk: stale IR bytecode / JIT code / closures executed after mutation,
plus potential use-after-free in `ClosureBridgeData`.

## Proposed Architecture (Long-term)

A lightweight global **epoch / version** mechanism that increments on every
mutation, and is checked by every cache lookup. On mismatch, the entry is
treated as stale and re-built.

```
mutation → mutation_epoch_++ → on next cache access: check entry.epoch_ vs
mutation_epoch_, if mismatch → re-build (or invalidate + return nullptr
→ caller falls back to tree-walker or re-lower)
```

The epoch is **monotonic** (only ever incremented, never decremented) and
**process-global** (single counter, no per-thread or per-layer counters).
The granularity is "1 epoch = 1 mutation" (coarse but cheap).

Each cache entry stores `std::uint64_t last_seen_epoch_`. The check is
`entry.last_seen_epoch_ != mutation_epoch_` — single uint64 compare,
negligible overhead.

## Mitigations (5 phases)

### Mitigation #1 (Phase 1) — Epoch counter + IR cache integration

**Scope:** ~2-3 hours focused
**Risk:** Low (additive, doesn't touch existing logic)
**Files:** `src/compiler/service.ixx`

- Add `std::atomic<std::uint64_t> mutation_epoch_{0};` to `CompilerService`
- `IRCacheEntry` gets `std::uint64_t last_seen_epoch_ = 0;`
- On mutation (in `invalidate_function` or `mutate_lock_and_invalidate`):
  `mutation_epoch_.fetch_add(1);` (atomic)
- On `ir_cache_` lookup: if `entry.last_seen_epoch_ < mutation_epoch_`,
  treat as stale → re-lower (or fallback)
- On `jit_cache_` lookup (in `eval_ir`): if `entry.last_seen_epoch_ < mutation_epoch_`,
  force re-compile

**Verification:** `test_issue_166.cpp` (NEW) — execute code, mutate, verify
the next execution doesn't use stale IR/JIT.

### Mitigation #2 — Strengthen `invalidate_function` + deopt in-flight

**Scope:** ~4-8 hours
**Risk:** High (deopt in concurrency-heavy code is dangerous)
**Files:** `src/compiler/service.ixx`, `src/compiler/ir_executor_impl.cpp`

- Acquire `mutate_mtx_` in SHARED mode during mutate (already done — #59 Iter 3)
- Drain in-flight JIT compiles by waiting on a generation counter
- Force deopt to the tree-walker fallback when stale JIT code is detected
- Add per-IR-frame `defuse_version_at_call_` snapshot; on return, if version
  changed, re-eval the frame from source

### Mitigation #3 — Arena ownership / lifetime for `ClosureBridgeData`

**Scope:** ~4-8 hours
**Risk:** High (affects every IRModule emit; high regression risk)
**Files:** `src/compiler/lowering_impl.cpp`, `src/compiler/service.ixx`,
`src/compiler/ir.ixx`

- Replace `flat*` / `pool*` raw pointers in `ClosureBridgeData` with
  `std::shared_ptr<FlatAST>` / `std::shared_ptr<StringPool>` (or
  arena-stable index + version check)
- Track the `arena_epoch_` at capture time; on each access, check the
  current arena epoch vs the captured one. Mismatch → re-capture
- This decouples closure lifetime from raw pointer aliasing

### Mitigation #4 — Comprehensive test coverage

**Scope:** ~4-8 hours
**Risk:** Test-only (safe)
**Files:** `tests/test_issue_166.cpp`

- High-frequency mutate + concurrent execution + JIT scenario
- 50+ iterations of mutate-then-execute to surface rare races
- Stress tests for `apply_closure` dispatch path
- Verification of `closure_bridge_` callback lifetime

### Mitigation #5 — Design doc + issue tracking

**Scope:** 2-3 hours
**Risk:** None (documentation)
**Files:** This document + follow-up issues

## Implementation Order

1. **Mitigation #1 (Phase 1, ships tonight)** — design doc + epoch counter
2. **Mitigation #5 (also tonight)** — update this doc as work proceeds
3. **Mitigation #2-#4 (defer to fresh session)** — too risky at 37+ hours
   marathon. Need design review + careful testing.

## Tradeoffs

### Why a single global epoch (not per-cache or per-mutation-type)

- **Cost:** atomic increment per mutation is one CAS, sub-nanosecond.
- **Simplicity:** one counter, one check site per cache, no coordination.
- **Over-invalidation:** if multiple mutations happen between two cache
  lookups, the epoch bumps multiple times. The cache entry is stale
  either way. So over-invalidation is correct.
- **Under-invalidation:** a mutation that doesn't affect a given function
  (e.g., mutate a var in func A, but func B's IR was cached) still
  invalidates B. This is a known cost — over-invalidation is the
  price of a simple global counter. Per-cache or per-sym invalidation
  would be more precise but requires the dep_graph_ to be perfectly
  accurate (which it isn't, per the issue body).

### Why not the existing `defuse_version_`

`defuse_version_` is per-Evaluator-instance, lives on `Evaluator`, and is
incremented on every `mutate:*` primitive. The issue with using it as the
global epoch:
- It's per-Evaluator, not per-CompilerService
- It's not atomic (the existing pattern uses `std::uint64_t` with no
  atomic semantics; concurrent mutate in different fibers could race)
- It's tied to the def-use index, which is one of the layers we want
  to track (using it as the epoch creates a circular dependency)

A separate `mutation_epoch_` on CompilerService, with explicit
`fetch_add(1)` at every mutation, decouples the epoch from the def-use
subsystem. This is cleaner.

## Open Design Questions (for fresh-session pickup)

1. **Resolution granularity:** should the epoch be per-mutation (1
   increment per `typed_mutate` call) or per-primitive-call (1
   increment per `mutate:rebind` etc. inside the typed_mutate)?
   Recommendation: per-primitive-call (more precise).

2. **Deopt vs re-eval:** when a stale JIT code is detected (Mitigation #2),
   should we deopt to the tree-walker, or re-eval the IR from source?
   Recommendation: deopt (cheaper, reuses tree-walker fallback).

3. **Closure bridge lifetime:** for Mitigation #3, should we use
   `shared_ptr` or arena-stable index + version check? The arena
   approach is more cache-friendly but harder to reason about.
   Recommendation: `shared_ptr` (simpler, safer).

4. **Backward compat:** the new epoch check is a hot-path overhead.
   Should it be opt-in (config flag) or always-on? Recommendation:
   always-on (it's one uint64 compare, ~1ns; the regression risk of
   opt-in is higher than the perf cost).

## Commits This Phase

- TBD: Phase 1 implementation (Mitigation #1 — epoch counter)
- TBD: tests/test_issue_166.cpp
- TBD: this design doc

## Phase 2 (deferred)

- Mitigation #2: deopt + in-flight drain
- Mitigation #3: ClosureBridgeData ownership
- Mitigation #4: comprehensive tests
- Mitigation #5: doc updates as phases land
