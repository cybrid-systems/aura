# Incremental Compilation v3 — Design (Issue #169)

**Status:** Design document + Phase 1 (config flag)
**Date:** 2026-06-12
**Workstream:** 1 of #143 (Fine-grained Incremental Compilation)

## Problem

Current `dep_graph_` (in `src/compiler/service.ixx`) tracks **forward
dependencies** (function A calls B) but not **reverse dependencies**
(B is called by A). Impact analysis for mutations uses forward
walks only, which means:

- `invalidate_function("foo")` does a BFS through `dep_graph_[foo].called_by`
  to find all transitively-affected functions. This works for direct
  calls but misses:
  - **Closures that capture `foo`** (the closure's IR was lowered
    before the mutation, but the closure body references `foo`'s
    captured state)
  - **ADT constructors** that use `foo` in field initializers
  - **Strategy definitions** that reference `foo` (E2 evolve paths)

- `ir_cache_v2_` uses **per-define** caching keys (canonical source
  string + FNV-1a hash). Two defines with identical source but
  different usage context share a cache entry → either over- or
  under-invalidation.

- `pre_cache_workspace_defines` (in `populate_ir_cache_v2_from_workspace`)
  re-processes every define in the workspace on every call, even
  those that haven't changed since the last pre-cache.

## Goals (per #169)

1. **Reverse dep tracking** + precise impact analysis
2. **AST diff + affected-define calculation** in `set-code` / `mutate:*`
3. **Expr-level caching keys** in `ir_cache_v2_` (source_span +
   inferred type sig) — 3-5x speedup claim
4. **Incremental `pre_cache_workspace_defines`** that skips unchanged
5. **Config flag `incremental-strictness`** (conservative vs aggressive)

## Proposed Architecture

### Goal 5 (Phase 1, this commit) — `incremental-strictness` config flag

Simple enum + accessor + service-level setting. Three modes:

```cpp
enum class IncrementalStrictness : std::uint8_t {
    Conservative = 0,  // invalidate MORE than strictly necessary
    Balanced     = 1,  // default; use the existing BFS on dep_graph_
    Aggressive   = 2,  // invalidate LESS; trust the new precise
                       //   impact analysis (Goals 1-2)
};
```

The flag is read by code that needs to decide between "safe
over-invalidation" (Conservative) and "trust the new precise
analysis" (Aggressive). Default is Balanced (the existing behavior).

The flag is **additive** — no behavior change in Balanced mode.
Conservative and Aggressive are forward-looking, will be wired in
by the Goals 1-2 implementations.

### Goal 1 (deferred) — Reverse dep tracking

Augment `dep_graph_` with reverse edges. Each entry stores both
`calls` (forward) and `called_by` (reverse). On `invalidate_function(name)`:

- Walk `called_by` to find direct reverse dependents
- BFS from each reverse dependent to find transitive reverse dependents
- Same O(n) as the forward walk, just inverted

This catches:
- Closures that capture `name` (the closure's `dep_graph_` entry
  would have `name` in its `called_by` if we track capture edges)
- ADT constructors referencing `name`
- Strategy references

**Design decision needed**: capture semantics for closures. Is
`lambda (x) (* x foo)` "calling foo" (transitive dep) or "capturing
foo" (reverse dep via closure)? Recommendation: BOTH — the closure's
parent env references `foo`, the closure body uses `foo`, so both
edges exist.

### Goal 2 (deferred) — AST diff + affected-define calculation

When `set-code` is called with a new source string, instead of
"invalidate everything", diff the new AST against the old AST
(both stored as FlatAST) and compute the affected set: defines
that were added, removed, or had their body changed.

Affected-set algorithm:
1. Lex + parse new source to get new FlatAST
2. For each Define in new AST, check if it exists in old AST
   (by name) and has the same body (by node-hash)
3. Removed defines: in old but not in new
4. Changed defines: in both but body differs
5. New defines: in new but not in old
6. Invalidate only the affected set

**Design decision needed**: what to do with defines that REFERENCE
a changed define? (e.g., (f) is in both but (g) calls (f) — is (g)
affected by (f)'s change?). Recommendation: walk reverse dep
graph from each changed define to find transitive dependents.

### Goal 3 (deferred) — Expr-level caching keys

Change `ir_cache_v2_` key from `(name, source_hash)` to
`(source_span, inferred_type_id, type_signature)`. Two different
expression-level usages of the same source code (e.g., same source
in two different type contexts) get separate cache entries.

The 3-5x speedup comes from the fact that:
- Per-define caching conflates "same source, different type context"
- Per-expr caching distinguishes them → higher cache hit rate
- AI self-mod loops hit the same expressions repeatedly with similar
  type contexts → cache hits compound

**Design decision needed**: how to compute the type signature.
Options:
- (a) The whole inferred type's index (TypeId) — coarse but simple
- (b) A hash of the type's free vars + depth — finer but more
      complex
- (c) The shape_id from the per-function shape_map — uses existing
      infra (ShapeProfiler from #63)

Recommendation: (a) for v3, (b) or (c) for v3.1 if needed.

### Goal 4 (deferred) — Incremental `pre_cache_workspace_defines`

Currently `populate_ir_cache_v2_from_workspace` re-processes every
define. With Goal 1+2, we have a precise affected set. The
incremental version:

1. Check `dep_graph_` for the workspace's defines
2. For each define, check if its `dep_graph_[name].signature` matches
   the previous call's signature
3. If match, skip (cache entry is fresh)
4. If mismatch, re-process

**Design decision needed**: what is the "signature" for skipping?
Options: (a) source hash, (b) inferred type hash, (c) both.
Recommendation: both — must match both source AND inferred type to
skip.

## Implementation Order (recommended for fresh-session pickup)

1. **Phase 1 (this commit)**: Goal 5 — `incremental-strictness` config flag
2. **Phase 2**: Goal 1 — reverse dep tracking
3. **Phase 3**: Goal 2 — AST diff + affected-define calculation
4. **Phase 4**: Goal 3 — expr-level caching keys
5. **Phase 5**: Goal 4 — incremental pre-cache

Each phase is independently shippable. Phases 2-5 are 1-3 weeks
of focused work each. **Total: 2-3 weeks** (matches the #169 AC).

## Tradeoffs

### Why the config flag is the right Phase 1

- **No behavior change**: Balanced mode == existing behavior
- **Forward-looking**: Conservative/Aggressive modes are
  pre-wired but no consumer reads them yet
- **Low risk**: additive (new enum + accessor + service field)
- **Enables testability**: Goals 2-4 can test their new code paths
  in Balanced mode (existing) and Conservative/Aggressive modes
  (new) without breaking anything

### Why not Goals 1-2 in this session

- Each requires careful design + test verification + perf benchmark
- 40+ hours continuous session is not the right time
- The build was just repaired; shipping more code increases risk
  of introducing new regressions
- Design decisions (capture semantics, diff algorithm, etc.) need
  Anqi's input for the right direction

### Why the existing `dep_graph_` is OK for Balanced mode

The current forward BFS catches the most common case (direct
callers). Goal 1 (reverse deps) catches the edge cases (closures,
ADT, strategies). Balanced mode keeps the existing behavior;
Conservative/Aggressive will be wired in once Goals 1-2 land.

## What this fix enables

**Before**: every mutation invalidates more defines than strictly
necessary. AI self-mod loops re-lower unchanged defines each time,
wasting CPU.

**After** (when Goals 1-4 land): precise impact analysis + expr-level
caching + incremental pre-cache → 3-5x faster AI self-mod loops.

**Today (Phase 1)**: nothing changes behaviorally. The flag is
available for the future implementations to read.

## Commits This Phase

- TBD: design doc + enum + accessor + service field + tests

## Phases 2-5 (deferred to fresh session)

- Phase 2: reverse dep tracking (1-2 weeks)
- Phase 3: AST diff + affected-define (2-3 weeks)
- Phase 4: expr-level caching keys (1-2 weeks)
- Phase 5: incremental pre-cache (1 week)
