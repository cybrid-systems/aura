# ClosureBridgeData / IRClosure Lifetime Hardening (Issue #180)

**Status:** Design + 4-cycle hardening plan. 0 sub-items shipped.
**Date:** 2026-06-13
**Priority:** P0 Safety

## Problem (per issue body)

`ClosureBridgeData {flat*, pool*, body_id}` and copied `IRClosure`
fields store **bare pointers** to `FlatAST`/`StringPool` that live in
per-request arenas. After `arena_.reset()` (or during concurrent fiber
mutations), these pointers can become dangling.

The risk:
- Use-after-free when tree-walker primitives or IRInterpreter invoke
  bridged closures after mutation
- Data races or inconsistent state in hybrid execution
- Silent corruption or crashes in long-running agents

## Existing infrastructure (worth reusing)

- `cache_epoch_` in `type_checker_impl.cpp:1348-1352` — per-type-checker
  epoch counter; `epoch_invalidated_` flag; `last_inference_epoch_`
  comparison. This is the model for #180's epoch counter.
- `EnvId` SoA pattern (#145 Phase 2) — the bridge metadata can follow
  the same pattern (handle-based, not raw-pointer-based)
- `mark_define_dirty` / `invalidate_function` / `hot_swap_function_impl`
  — the existing invalidation paths that need to be extended

## Current ClosureBridgeData (audit)

```cpp
// src/compiler/ir.ixx:434
export struct ClosureBridgeData {
    const ast::FlatAST* flat = nullptr;          // RAW POINTER — DANGEROUS
    const ast::StringPool* pool = nullptr;        // RAW POINTER — DANGEROUS
    ast::NodeId body_id = ast::NULL_NODE;
    std::string body_source;                      // owned (safe)
};
```

The `flat*` and `pool*` are the source of the UAF risk.

## Detailed sub-item scope (4 cycles)

### Cycle 1: Epoch counter (the smallest, biggest win)
- Add a global (or per-CompilerService) `bridge_epoch_` counter
  that's incremented on every `arena_.reset()` and major mutation
- Add `bridge_epoch_` field to `ClosureBridgeData` (captured at
  construction time)
- In `apply_closure()` and the bridge callback: compare the
  bridge's `bridge_epoch_` against the current `bridge_epoch_`
- If mismatch: the arena was reset; the `flat*`/`pool*` are
  stale — fall back to re-parsing from `body_source` (the
  serialized source), or invalidate the closure entirely

**Why this is a small cycle**:
- Single field addition
- Single check in the bridge callback
- The fallback path already exists (re-parse from
  `body_source`); we're just gating it on the epoch

**Estimated 1-2 days focused work.**

### Cycle 2: shared_ptr / arena-aware handles
- Replace `const ast::FlatAST* flat` with
  `std::shared_ptr<const ast::FlatAST> flat` (or an
  arena-aware handle)
- Same for `pool`
- The `shared_ptr` keeps the FlatAST alive as long as any
  bridge holds a reference
- For arena-owned ASTs, use a `std::shared_ptr` to a stable
  container (the arena is the actual owner; the shared_ptr
  just tracks lifetime)

**Why this is a separate commit**:
The shared_ptr change touches every site that creates a
`ClosureBridgeData` (lambda lowering, cache bridge copy,
service.ixx). Mechanical but the diff is large.

**Estimated 1-2 days focused work.**

### Cycle 3: Integration with existing invalidation
- Extend `mark_define_dirty` / `invalidate_function` /
  `hot_swap_function_impl` to also invalidate affected
  bridge entries
- When a function is hot-swapped (re-lowered), all closures
  that captured its bindings must be re-bridged
- Bridge cleanup in `service.ixx` is more robust across
  `reset()` — explicitly clear all bridge data, not just
  the cache

**Why this is a separate commit**:
The invalidation is a separate concern from the epoch /
shared_ptr. It's the "active" version of the safety check
(when something is invalidated, take action) vs the
"passive" version (just check the epoch).

**Estimated 1-2 days focused work.**

### Cycle 4: Tests + invariants
- Concurrent mutate + closure invocation under fiber scheduler
- Arena reset followed by bridged closure call
- Post-mutation invariant checks for bridge pointers
- TSan + ASan runs (1000+ iterations of mutate + bridge
  invocation)
- A "doomsday" stress test: 1000 fibers, each doing
  mutate + bridge-call + arena-reset in random order,
  verify no UAF / data race

**Estimated 1-2 days focused work.**

## Test scenarios (test_issue_180.cpp, future commits)

- Cycle 1: epoch counter increments on arena reset;
  the bridge callback detects the mismatch and falls back
  to `body_source` re-parse
- Cycle 2: shared_ptr keeps the FlatAST alive even when
  the arena is reset; the bridge still dereferences correctly
- Cycle 3: hot-swap a function; all dependent bridges are
  invalidated; subsequent bridge calls re-bridge
- Cycle 4: TSan-clean for 1000+ iterations of
  mutate + bridge-invoke

## Migration plan (4 cycles, each shippable)

### Cycle 1: Epoch counter (1-2 days)
- Add `bridge_epoch_` to CompilerService (or global)
- Add `bridge_epoch_` field to ClosureBridgeData
- Check epoch in `apply_closure()` and bridge callback
- Fall back to `body_source` re-parse on mismatch
- Test: test_issue_180.cpp Cycle 1

### Cycle 2: shared_ptr (1-2 days)
- Replace `flat*` and `pool*` with `shared_ptr<const FlatAST>`
  and `shared_ptr<const StringPool>`
- Update all `ClosureBridgeData` construction sites
- Test: shared_ptr keeps the FlatAST alive

### Cycle 3: Invalidation (1-2 days)
- Extend `mark_define_dirty` / `invalidate_function` to
  invalidate bridges
- Robust `service.ixx` cleanup on `reset()`
- Test: hot-swap triggers bridge invalidation

### Cycle 4: Tests + invariants (1-2 days)
- Stress tests for concurrent mutate + bridge-invoke
- TSan + ASan runs
- "Doomsday" stress test
- Post-mutation invariant checks

## Total effort

- Cycle 1: 1-2 days
- Cycle 2: 1-2 days
- Cycle 3: 1-2 days
- Cycle 4: 1-2 days
- Total: 4-8 days focused work

The P0 safety rating makes this a high-priority follow-up.
The existing `cache_epoch_` pattern (in type_checker) is the
model for Cycle 1's implementation.

## Why design + 4 follow-ups (not one big commit)

The 4 cycles are mostly independent and each is a 1+ day
focused effort. The full PR would touch every bridge
construction site, the bridge callback, the invalidation
paths, and the test infrastructure. Following the marathon's
"design + N small cycles" pattern keeps each commit small
+ reviewable + bisectable.

The shared_ptr migration (Cycle 2) is the riskiest piece
(API change in the bridge struct). Cycle 1 (epoch) is the
quickest win and unblocks Cycles 2-4.
