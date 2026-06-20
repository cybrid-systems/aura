# std::meta (C++26 P2996) Migration Roadmap (Issue #257)

**Status:** Roadmap document + Phase 0 + Phase 1 observability foundation
**Date:** 2026-06-20
**Phase:** 0-1 of 4 (pilot phases shipped; 2-3 deferred until P2996 lands in a compiler)

## Problem

Aura relies heavily on hand-written reflection mechanisms:

- `FlatAST::generation_` + `node_gen_` + `is_valid()` +
  `StableNodeRef` for reference stability (a candidate
  candidate for compile-time validation generation).
- `FlatAST::children()` + `parent_of()` +
  `mark_dirty_upward()` for AST traversal + dirty
  propagation (candidates for compile-time SoA
  accessor generation).
- `reflect_members<T>()` + `auto_serialize<T>()` for
  the reflection-driven serialization path (P2996 will
  deprecate these hand-rolled mechanisms).
- Manual `bump_generation()` sync points throughout
  `ast.ixx` (candidates for automatic generation from
  `reflect_members<T>()`).

As more node types, mutation primitives, and traversal
patterns are added, this manual approach becomes
increasingly painful — every new feature risks
introducing subtle inconsistencies (missed bump sites,
out-of-order invalidations, missing field indices).

C++26 `std::meta` (P2996) offers a way out: it lets
code be generated at compile time from type
descriptions, eliminating the manual synchronization
burden. **However, std::meta is in pre-Cologne and not
implemented in any shipped compiler** (GCC trunk
r16-8246 doesn't ship it). Full adoption cannot happen
today.

## Proposed Phased Roadmap

### Phase 0 — Reference stability observability foundation (Issue #255) ✅ SHIPPED

Scope-limited close: instrument the existing
hand-written mechanism with 4 atomic counters so users
can audit how often each part fires.

- `bump_generation_count_` — total generation bumps
- `is_valid_check_count_` — total `is_valid()` calls
- `stable_ref_invalidations_` — StableNodeRef that
  went stale (ref.gen != current gen when checked)
- `atomic_batch_commits_` — atomic batches committed

Aura primitive: `(compile:invalidations-stats)`.
Snapshot: `CompilerSnapshot.{bump_generation_count,
is_valid_check_count, stable_ref_invalidations,
atomic_batch_commits}`.

**Verified:** 16/16 test_issue_255, full regression
`35322/35322` checks, 0 failures. Issue #255 closed
(`state_reason: completed, scope-limited`).

### Phase 1 — AST operation observability foundation (Issue #256) ✅ SHIPPED

Same observability approach for the AST traversal +
dirty-propagation path:

- `children_call_count_` — total `children()` calls
- `parent_of_call_count_` — total `parent_of()` calls
- `mark_dirty_upward_call_count_` — total
  `mark_dirty_upward()` invocations
- `mark_dirty_total_nodes_` — total nodes touched
  across all `mark_dirty_upward()` calls. Divided by
  `mark_dirty_upward_call_count_` gives the average
  dirty-propagation depth per mutation — **the key
  metric for whether the std::meta refactor is worth it**.

Aura primitive: `(compile:ast-ops-stats)`.
Snapshot: `CompilerSnapshot.{children_call_count,
parent_of_call_count, mark_dirty_upward_call_count,
mark_dirty_total_nodes}`.

**Verified:** 16/16 test_issue_256, full regression
`35338/35338` checks, 0 failures. Issue #256 closed
(`state_reason: completed, scope-limited`).

### Phase 2 — Query system (Issue #257 follow-up) 🔴 DEFERRED

The Query system (`query:*` primitives in
`evaluator_impl.cpp`) walks the AST with hand-written
visitors for each query type (dependencies, callers,
type-info, module-exports, etc.). When P2996 lands, the
common walk pattern can be auto-generated from
`reflect_members<FlatAST>()`.

**Pre-requisite:** compiler with P2996 (`std::meta`)
support — GCC trunk / Clang mainline. Track
[P2996 status](https://github.com/cplusplus/papers/issues/1534).

### Phase 3 — Broader adoption (Issue #257 follow-up) 🔴 DEFERRED

Once the Query system pattern is established,
propagate to:

- `reflect_members<T>()` → `std::meta::members_of(^T)`
- `auto_serialize<T>()` → `std::meta::value_of()` /
  `std::meta::name_of()` / template generator
- `MutationRecord` + `Patch` roundtripping
- `CompilerMetrics` + `CompilerSnapshot` field
  generation (today hand-maintained in two places)
- `bump_generation()` sync point generation from
  `reflect_members<T>()` (every field that affects
  reference stability should bump automatically)

## What's NOT in scope for any phase

- **Reflection on closures** (Lambda expressions in
  Aura, not C++ lambdas — handled separately by
  Aura's own reflection in `reflect/reflect_schema.hh`).
- **Cross-module reflection** (P2996 has limitations
  here; we may need a hybrid approach for some
  boundaries).
- **Performance-critical hot paths** that are
  measurement-sensitive (any std::meta refactor in a
  hot path needs a quantitative benchmark first — the
  Phase 0/1 observability is the baseline).

## Pre-requisite Tracking

The roadmap activates when **any** of these happens:

- GCC ships std::meta (track
  [GCC P2996 status](https://gcc.gnu.org/git/?p=gcc.git;a=blob;f=libstdc%2B%2B-v3/include/std/meta)).
- Clang ships std::meta (track
  [Clang libc++ status](https://libcxx.llvm.org/Status.html)).
- A third-party implementation (e.g. from
  [Eric Niebler's cppfront](https://github.com/ericniebler/cppfront)
  or a research prototype) reaches a usable state.

Until then, the Phase 0 + 1 observability is the
shippable foundation: it instruments the existing
hand-written mechanism so any future std::meta
refactor has a quantitative baseline to compare
against.

## What this document is NOT

This is **not** a promise to ship Phase 2/3 by any
particular date. The whole point of Phase 0/1 as
scope-limited slices is to keep the migration option
open without committing to a timeline that depends on
an external compiler-implementation milestone.

## Cross-references

- Issue #255 — reference stability observability
  foundation (Phase 0).
- Issue #256 — AST operation observability foundation
  (Phase 1).
- `docs/design/ir_soa_migration.md` — sister roadmap
  for the IR layer SoA/DOD migration (different layer,
  same scope-limited-close pattern).
- `docs/roadmap.md` — overall Aura roadmap.
