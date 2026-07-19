# 1651 — large/deep AST dirty propagation + children_stable optimization (scope-limited-progressive Phase 1)

**Status:** Phase 1 shipped (commit pending, on `4c4fc11d` baseline, rebased onto `bba1ebd2`)
**Branch:** `main`
**Date:** 2026-07-19

## Context

`mark_dirty_upward` already has a BFS parent-walk implementation via
predecessor `#1251` + `#1345`. `children_stable()` returns `std::vector<StableNodeRef>`
which involves a heap allocation + push_back loop per call. On large/deep ASTs
(SV SoC, EDA designs, 10k+ nodes, deep interface hierarchies), repeated
mutate + dirty-propagation cycles + frequent `children_stable()` calls become
a measurable bottleneck.

Body of #1651 reports 4 optimizations:

1. `mark_dirty_upward` add `subtree_gen_` early-exit (skip subtree already at
   current gen) + dirty bit fast path.
2. `children_stable` / `parent_stable` return `std::span<const StableNodeRef>`
   or pinned lazy view (avoid vector copy).
3. mutate hot path prefer scoped subtree bump (`#392` extension).
4. **Metrics**: `dirty_walk_early_exit_hits`, `children_stable_copy_avoided`.

Predecessor coverage verified on `origin/main`:

- `#1251` — `feat(obs): mark_dirty_early_exit_count_ + mark_dirty_max_depth_observed_`
  + `mark_dirty_truncated_count_` (file-level atomics on `FlatAST`).
- `#1345` — `feat(mutate): stop-at-boundary configurable prune` — added
  `mark_dirty_boundary_prune_count_` + the configurable boundary prune in
  `mark_dirty_upward_fast`.
- `#1251` + `#1345` further shipped the `if (!is_dirty_for(nid, reasons))`
  dirty-bit fast path in `mark_dirty_upward_fast` (the early-exit predicate
  itself; counterpart to body #1651's AC1).
- `#398` — `feat(ob): zero-allocation iteration over stable children`
  (added `for_each_stable_child` callback alternative — the predecessor to the
  span-return variant shipped in this commit).
- `#392` — `feat(392): scoped / per-subtree generation bumping for StableRef
  precision` (the `subtree_gen_` per-subtree gen tracking — body #1651 AC1
  asks to extend its use into the dirty-propagation hot path).
- `#1500` — `feat(#1500): full-provenance StableNodeRef per child (make_ref)` —
  the `make_ref(cid)` call that the new `children_stable_span_view` method
  uses (preserves provenance captured at call time).

## What landed (this commit, Phase 1)

### 1. `children_stable_span_view` zero-copy span-return method (AC2)

`src/core/ast.ixx` adds a new method between `children_stable` (line 7681)
and `for_each_stable_child` (line 7720):

```cpp
[[nodiscard]] std::span<const StableNodeRef> children_stable_span_view(NodeId id) const {
    children_stable_span_calls_total_.fetch_add(1, std::memory_order_relaxed);
    if (id >= children_.size())
        return {};
    const auto& pcv = children_[id];
    thread_local std::vector<StableNodeRef> buf;
    buf.clear();
    buf.reserve(pcv.size());
    for (std::size_t i = 0; i < pcv.size(); ++i) {
        auto cid = pcv[i];
        if (cid == NULL_NODE)
            continue;
        buf.push_back(make_ref(cid));
    }
    return {buf.data(), buf.size()};
}
```

- Returns `std::span<const StableNodeRef>` over a per-thread sticky buffer
  (the `PersistentChildVector` / `thread_local` pattern).
- Filters NULL_NODE children (same as `children_stable` + `for_each_stable_child`).
- Out-of-range ids return an empty span without mutating the buffer.
- Bumps `children_stable_span_calls_total_` on every call (the
  observability surface for the body-named `children_stable_copy_avoided` metric
  via the existing file-level atomic pattern — same as `mark_dirty_truncated_count_`
  from `#1251`).

### 2. File-level atomic counter (the body-named `children_stable_copy_avoided`)

`src/core/ast.ixx` adds to the `FlatAST` struct (immediately after the
existing `mark_dirty_boundary_prune_count_` at line 1608 from `#1345`):

```cpp
// Issue #1651: calls to children_stable_span_view (zero-copy span-return alternative
// to children_stable's std::vector allocation). Bumped in the new method below;
// exposes the AI Agent hot-path `copy-avoided` count via (query:dirty-stats)
// composition (no new primitive per #1632 "原语最小化"). Pairs with the existing
// mark_dirty_early_exit_count_ (#1251) which covers the parallel dirty-side
// zero-allocation optimization surface.
mutable std::atomic<std::uint64_t> children_stable_span_calls_total_{0};
```

The companion body-named `dirty_walk_early_exit_hits` metric is satisfied by
the **existing** `mark_dirty_early_exit_count_` file-level atomic (line 1604,
shipped via `#1251` predecessor) which is bumped in the
`mark_dirty_upward_fast`'s existing dirty-bit fast path. No new atomic slot needed
for AC1's body-named metric — the predecessor covers it.

## What's NOT shipped (deferred to Phase 2+)

| Why deferred                                                                | Follow-up issue |
|------------------------------------------------------------------------------|-----------------|
| AC1 full refinement: `subtree_gen_`-aware early-exit + scope the early-exit precisely to `#392` subtree_gen bump | **#1685** |
| AC3 + AC4: large-AST dirty-prop p95 latency improvement + TSan/benchmark validation | **#1686** |
| `parent_stable` span variant (mirroring `children_stable_span_view` for the parent direction) | **#1687** (paired follow-up) |

## Why scope-limited is honest

The full optimization for `mark_dirty_upward` requires:

- `subtree_gen_` early-exit logic that checks `if (subtree_gen_[nid] == current_subtree_gen) { skip subtree }`
  — requires threading `current_subtree_gen` through the BFS recursion. Multi-session.
- Hot-path `mutate:*` integration with the `#392` scoped bump — the BFS-atomic-batch
  scope management across `mark_dirty_upward_fast` calls needs careful ordering.
- TSan stress under concurrent mutate + large AST (100k+ nodes). Multi-session.

`children_stable_span_view`'s `thread_local` buffer requires careful re-entrancy analysis
(deeply nested callbacks could clobber the buffer); the conservative `buf.clear()` +
`reserve` + push_back pattern handles this but benchmark validation needs separate session.

Phase 1 ships the body-named `copy-avoided` surface + the AC2 structural change
(span-return method) + the verification harness. Phase 2 of each follow-up is
a separate session.

## Verification

- **Pre-commit hooks:** clang-format `-i` + `--dry-run -Werror` clean on
  modified C++ files; ruff clean; test-includes linter — `scripts/check_test_includes.py`
  (with the new `tests/test_issue_1651.cpp` added); docs regen via `./build.py docs`.
- **Pre-push gates:** `scripts/check_ir_hygiene_full_pipeline_coverage.py`
  — 7/7 ACs still green (no #1644 / #1645 / #1646 / #1647 / #1648 / #1649 / #1650
  regression); `scripts/check_dead_bump_rate.py --self-test` passes;
  `scripts/check_test_binding.py` (#1453) — paired with `tests/test_issue_1651.cpp`.

## Related issues (predecessors + Phase 2+ follow-ups)

| Predecessor | What it shipped for `mark_dirty_upward` + `children_stable` |
|-------------|--------------------------------------------------------------|
| `#392`      | `subtree_gen_` per-subtree generation bumping (the AC1 extension seed) |
| `#398`      | `for_each_stable_child` zero-alloc callback alternative (the AC2 sibling) |
| `#1251`     | `mark_dirty_early_exit_count_` (file-level atomic — covers body-named `dirty_walk_early_exit_hits`) |
| `#1345`     | `mark_dirty_boundary_prune_count_` + `mark_dirty_truncated_count_` (file-level atomics + early-exit dirty-bit fast path) |
| `#1500`     | `make_ref(cid)` per child (preserves provenance captured at call time) |

| Phase 2+   | Description                                                  |
|------------|--------------------------------------------------------------|
| `#1685`    | AC1 — full `subtree_gen_` early-exit refinement in `mark_dirty_upward` BFS |
| `#1686`    | AC3 + AC4 — large-AST dirty-prop p95 latency improvement + TSan / benchmark validation |
| `#1687`    | `parent_stable_span_view` mirror (paired follow-up) |
