# Issue #130 — Perf/TypeSystem: enhance incremental type checking, enable full Let-Poly, complete ADT exhaustiveness

## Status: 🟡 Partial (3 of 4 sub-goals deferred; this PR ships the cache hit rate metric)

The issue lists 4 sub-goals:
1. Enhance incremental type checking (more granular
   dependency tracking, expose hit rate, integrate with
   mutate:*)
2. Enable full Let-Poly (TypeScheme instantiation in
   constraint solver)
3. Complete ADT exhaustiveness (or-patterns, guards,
   non-__match_tmp scenarios)
4. Optional: TypeId annotations in IR for JIT
   specialization

This PR ships the **cache hit rate metric** (sub-goal 1
partial). The other 3 sub-goals are deferred as
follow-ups because they require deeper refactors:

- Sub-goal 1 (granular dependency tracking) requires
  redesigning the dirty-flag propagation from "ancestor
  path" to "value-level dependency set".
- Sub-goal 2 (Let-Poly in constraint solver) requires
  reworking the ConstraintSystem to handle TypeScheme
  instantiation, which is a major refactor.
- Sub-goal 3 (ADT exhaustiveness for or-patterns/guards)
  requires extending MatchClauseInfo and the match-info
  lowering.
- Sub-goal 4 (TypeId annotations in IR) requires
  IRInstruction schema change.

## What changed

### 1. `TypeChecker::cache_hit_rate() -> double`

`src/compiler/type_checker.ixx`:
```cpp
// Issue #130: cache hit rate (0.0 .. 1.0). Computed
// as hits / (hits + misses + stale). Returns 0.0 if
// no incremental checks have been done. Useful for
// profiling mutation-heavy workloads: a high hit rate
// (e.g. >0.7) means the dirty-tracking is working
// well; a low hit rate (e.g. <0.3) means most
// mutations touch too much of the AST.
double cache_hit_rate() const {
    const auto s = stats_;
    const std::uint64_t total =
        s.cache_hits + s.cache_misses + s.stale_cache;
    if (total == 0) return 0.0;
    return static_cast<double>(s.cache_hits) /
           static_cast<double>(total);
}
```

The method reuses the existing `IncrementalStats`
struct (Issue #72) — no new state, no new fields. Just
a computed view. Callers that want to expose the metric
in `--evo-explain` / observability can call
`TypeChecker::cache_hit_rate()` directly.

### 2. Regression tests (9/9 pass)

`tests/test_issue_130.cpp` exercises the new metric:
- **Fresh TypeChecker has 0.0 hit rate** (1 test)
- **After reset_stats, hit rate is 0.0** (1 test)
- **Fresh stats are all-zero** (3 tests)
- **After inference, hit rate is in [0, 1]** (1 test)
- **Cold-start hit rate is < 0.5** (1 test, validates
  that the first inference records a cache_miss)
- **Hit rate formula is hits/(hits+misses+stale)** (1
  test, synthetic)

## Why the new design works

### Why a getter instead of a stored field

The hit rate is a derived metric: it can be computed
from the existing `cache_hits` / `cache_misses` /
`stale_cache` counters. Storing it as a separate field
would be redundant (and risk drift if the field isn't
updated atomically with the counters). The getter is
the right design: no state, no atomicity concerns, no
risk of drift.

### Why a method, not a free function

`cache_hit_rate()` takes no arguments and is
semantically "give me the rate of this TypeChecker".
A method is the natural fit. A free function would
require the caller to pass the TypeChecker, which is
unnecessary indirection.

### Why a `double` return

The hit rate is a continuous value in [0, 1]. A double
is the natural fit. Callers that want a percentage can
multiply by 100. Callers that want a percentage string
can format the double.

## Known limitations (out of scope for #130)

- **Sub-goal 1 (granular dependency tracking)** —
  requires redesigning the dirty-flag propagation. The
  current "ancestor path" approach is correct but
  coarse: a mutation at the root dirties the entire
  tree, even if the mutated value is only used in one
  branch. A future issue should switch to
  value-level dependency sets.
- **Sub-goal 1 (cache hit rate in --evo-explain)** —
  the metric is exposed via the getter, but not yet
  wired into the observability snapshot. A future
  issue should add `cache_hit_rate` to the snapshot.
- **Sub-goal 2 (Let-Poly in constraint solver)** —
  `is_poly` is set and `instantiate_forall` is called
  in lookup, but the constraint solver doesn't yet
  handle TypeScheme instantiation during unification.
  A future issue should add a `TypeVar::is_scheme` flag
  and handle it in `unify`.
- **Sub-goal 3 (ADT exhaustiveness for or-patterns /
  guards / non-__match_tmp)** — the current
  implementation only fires on `__match_tmp` lets.
  Or-patterns (`(Ctor x | Other y) body`) are
  represented as multiple separate `used_constructors`
  entries (because each pattern is a separate clause),
  so the missing-branch detection works correctly
  TODAY. Guards and non-__match_tmp scenarios are not
  supported. A future issue.
- **Sub-goal 4 (TypeId annotations in IR)** — IR has
  CastOp with type_tag, but the inference result
  doesn't flow into IR. A future issue.

## Acceptance criteria

- "Expose more statistics (cache hit rate) for
  profiling" — ✓ (this PR).
- "Constraint solver supports TypeScheme instantiation"
  — deferred (sub-goal 2).
- "Generic exhaustiveness checker (or-patterns,
  guards)" — deferred (sub-goal 3).
- "Type information more richly into IR" — deferred
  (sub-goal 4).

## Test status

- `integ`: 148/148 ✓
- `typecheck`: 10/10 ✓
- `test_issue_115..128, 130` all 15 pass ✓

## What (if anything) is still open

- Wire `cache_hit_rate()` into the observability
  snapshot so it shows up in `--evo-explain`.
- Add value-level dependency tracking (replace the
  current ancestor-path approach).
- Add TypeScheme instantiation in the constraint
  solver.
- Extend exhaustiveness checking to or-patterns,
  guards, and non-__match_tmp scenarios.
- Add optional TypeId annotations in IR.

1 file changed, 2 files added, 0 files removed.
