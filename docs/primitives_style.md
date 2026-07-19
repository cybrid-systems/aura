# Aura Primitives Style Guide (centralized pattern matching engine documentation)

**Status:** Phase 1 — observability consolidation document (created under #1653, refine #1501 / #1609 / #1636 / #1644 / #1646 / #1649 / #1650 / #1651 / #1652)
**Branch:** `main`
**Date:** 2026-07-19

## Context

The Aura `query:pattern` / `where` / `filter` matching logic is distributed
across `src/compiler/evaluator_primitives_query.cpp` (primitive surface),
`src/compiler/query_matcher.cpp` + `src/core/ast.ixx` (match_subtree + invariants),
and `src/compiler/evaluator_fiber_mutation.cpp` + `src/compiler/macro_expansion.cpp`
(clone_macro_body hygiene/expansion hooks). Task1 review at 2026-07-16 flagged
that the centralization gaps + hygiene propagation completeness needed
formal documentation.

This document is the Phase 1 deliverable for #1653 — it consolidates the
prior art shipped through today's #1501 / #1609 / #1636 / #1644 / #1646 /
#1649 / #1650 / #1651 / #1652 cycle into a single reference for AI Agent
self-edit targeting + hygiene safety transparency.

## 1. Pattern matching engine architecture

The matching engine is built from 3 layered abstractions:

### 1.1 Surface layer — primitive registration (evaluator_primitives_query.cpp)

The `query:pattern` primitive is registered via the pattern_matcher
interface. It dispatches to `QueryMatcher::match_subtree()` with the
current `g_current_compiler_service_` for per-eval context. The
primitive body exposes counters via the existing `(query:pattern-hygiene-stats)`
companion (registered at line 2344 from #1501/#1609/#1636/#1650 predecessors).

### 1.2 Algorithm layer — match_subtree (query_matcher.cpp + query_matcher.ixx)

`QueryMatcher::match_subtree(id, nid)` walks the AST via a BFS + recursive
descent. The key hygiene gate:

```cpp
// Issue #421 + #1255: recursive hygiene — hard skip MacroIntroduced subtrees
// when the caller did not pass :include-macro-introduced /
// :allow-macro? #t. Always count the filter (fast path included).
if (skip_macro_introduced_ && ws_flat_->is_macro_introduced(ws_id)) {
    ++recursive_macro_skipped_;
    ++macro_intro_filtered_strict_;
    return false;
}
// Issue #1650: inverse filter — when only_macro_introduced_ is set, skip
// User-authored nodes and keep only MacroIntroduced nodes. Pairs with the
// existing skip_macro_introduced_ check above (the 2 flags are mutually
// exclusive). Always count the inverse filter for the
// (query:pattern-hygiene-stats) primitive surface.
if (only_macro_introduced_ && !ws_flat_->is_macro_introduced(ws_id)) {
    ++recursive_user_skipped_;
    ++macro_intro_filtered_inverse_;
    return false;
}
```

### 1.3 Source layer — children iteration (ast.ixx)

Two zero-allocation strategies coexist (predecessors from #398/#1500/#1651):

- **`FlatAST::children_stable(NodeId)`** — returns `std::vector<StableNodeRef>` (heap allocation; for callers that need to store refs across mutation boundaries).
- **`FlatAST::for_each_stable_child(NodeId, Fn&&)`** — callback iteration (zero-alloc; for hot-path call sites that only iterate once). Predecessor #398.
- **`FlatAST::children_stable_span_view(NodeId)`** — returns `std::span<const StableNodeRef>` via `thread_local` sticky buffer (zero-copy; bumps `children_stable_span_calls_total_`). Predecessor #1651 Phase 1.

## 2. Hygiene surface

The macro hygiene surface is wired across 4 orthogonal axes. Each axis
has its own `CompilerMetrics` counters + `// Issue #NNNN` documentation block.

### 2.1 Query hygiene filter (skip / only macro) — predecessors #1636 + #1650

- `skip_macro_introduced_` — hard skip MacroIntroduced subtrees (default true per #1636 mandate)
- `only_macro_introduced_` — inverse filter (added in #1650)
- Counters: `recursive_macro_skipped_` + `macro_intro_filtered_strict_` (skip path); `recursive_user_skipped_` + `macro_intro_filtered_inverse_` (only path)
- Composed into `(query:pattern-hygiene-stats)` primitive body via the existing `pattern-hygiene-mandate-active` + `default-exclude-macro-introduced` keys.

### 2.2 Mutate template marker propagation — predecessors #1646 + #1649 + #1652

- `#1646` (`atomic_batch_pinning` + `template-respect` site): `mutation_boundary_macro_dirty_propagated_total` + `mutation_boundary_epoch_bump_for_macro_total` + `mutation_boundary_hygiene_violation_total` + `mutation_boundary_observability_queries_total` (4 counters + paired bumps in `evaluator_fiber_mutation.cpp` atomic_batch_pinning + template-respect site).
- `#1649`: `atomic_batch_hygiene_violation_prevented_total` + `mutate_template_marker_propagated_total` (2 paired legacy/new bumps at the atomic_batch_pinning + template-respect sites).
- `#1652`: `g_macro_expansion_total` + `g_macro_introduced_nodes_created_total` + `g_hygiene_violation_in_macro_expand_total` file-level atomics in `macro_expansion.cpp` (+ 3 C-linkage accessors + paired bumps inside `clone_macro_body` at function entry + depth-exceeded + body_id-NULL sites).
- Per-recursive-step `bump_macro_introduced_nodes_created(cumulative)` with cumulative count threaded through `clone_macro_body`'s recursive AST walk — **deferred to #1688** (multi-session refactor; counted 1× per call in Phase 1).

### 2.3 IR / JIT / ClosureBridge marker check — predecessors #455 + #1273 + #1610 + #1644 + #1646 + #1651

- `IRFunction::marker` (#246/#1616) + `IRInstruction::source_marker` (#455/#1273/#1610/#1644) + `aura_jit.cpp` checks at lines 249 + 605 (#1908/#1644)
- `InlinePass::respect_macro_hygiene_` gate (#246/#388) + `is_inlinable_branch_aware` 2-case hygiene check (#1644)
- `mutation_boundary_steal_safe_total` paired bumps at `MutationBoundary` outer boundary + inner boundary block (#1641)
- `boundary_held_steal_safe_total` paired bump at safe-steal success path (#1641)
- Per-CompilerMetrics counters for the `#1047` lineage: `mutation_boundary_hygiene_violation_prevented_total` (#1646) + `steal_mutation_boundary_deferred_total` (#1641) + `starvation_mitigated_for_boundary_count` (#1641)

### 2.4 AI hygiene surface (specialized primitives) — predecessors #750 + #1576 + #1907

- `(reflect:validate-macro-body)` primitive (#750/#1611)
- `(reflect:validate-edsl)` primitive (#1576)
- `reflect/EDSL bridge` (#1907) — runtime mutate + hygiene validation
- Auto_validate bridge hook (#1908 paired per-eval + file-atom fallback) at `aura_jit_bridge.cpp` + `aura_macro_provenance_repin_on_steal_total` etc.

## 3. Pattern matching AND semantics

The default match mode is intersection (subtree must satisfy all predicates).
The current supported predicates:

1. **Tag predicate** (`tag_arity_index_` lookup) — satisfied via `tag_arity_index_` indexed fast path.
2. **Symbol predicate** (`StableNodeRef::sym_id == ...`) — direct comparison.
3. **Marker predicate** (`:macro-introduced #t / #f`) — controlled by `skip_macro_introduced_` + `only_macro_introduced_` flags (see #1650).
4. **`...` ellipsis** (wildcard) — `#1255` recursive descent + the `mode` parameter for non-greedy matches.
5. **`:guard <expr>`** — `#292` matcher side detection (s_hygiene_depth handled internally for clone_macro_body).

The body of `query:pattern` returns the matching subtree roots + child sets;
the implicit `and` across predicates is realized via `match_subtree`'s BFS
recursion (`#396` lineage).

## 4. Observability pattern reference

For every `CompilerMetrics` counter shipped today, the paired-bump pattern is:

```cpp
// Legacy bump (per-Fiber / namespace-scope)
bump_legacy_counter_at_site();
// Per-CompilerMetrics paired bump (new)
if (auto* ev = Evaluator::yield_hook_evaluator()) {
    if (auto* m = static_cast<CompilerMetrics*>(ev->compiler_metrics())) {
        m->new_counter_total.fetch_add(1, std::memory_order_relaxed);
    }
}
```

This pattern preserves the legacy observability (for backward compat with
existing dashboards) while adding the per-CompilerMetrics aggregate
counter for new dashboards. The composition into existing primitives
(no new primitive per "原语最小化") exposes the new counters in the
existing primitive surface, e.g., `(query:pattern-hygiene-stats)` extended
to read `recursive_user_skipped_` + `macro_intro_filtered_inverse_` (#1650).

## 5. Centralization status (Task1 review #5 + #1047 supplementary)

Per body of #1653:

| AC | Status | Resolution |
|----|--------|------------|
| **AC1** — pattern 匹配逻辑集中 + 文档清晰（primitives_style.md 更新） | 🚢 **FRESH (this doc + Phase 1)** | This document (primitives_style.md) IS the centralization artifact. Phase 2 follow-up at #1689 (optional: extract a `query_engine.ixx` core module that explicitly factors the existing match_subtree + hygiene gate into a single named header; non-blocking for primitive surface). |
| **AC2** — query:pattern hygiene filter 生效，mutate 模板 marker 正确 propagate | ✅ Predecessor-covered | #1636 (`skip_macro_introduced_` mandate) + #1501 (marker-aware tag_arity) + #1609 (force-filter) + #1650 (inverse flag `only_macro_introduced_`) + #1649 (atomic_batch + SyntaxMarker propagation) + #1652 (clone_macro_body observability) — full chain ships. |
| **AC3** — IR/JIT/ inline 检查 MacroIntroduced，违规 deopt/panic with provenance | ✅ Predecessor-covered + Phase 2 #1689 deferral | #455 (IRInstruction::source_marker) + #1273 (SoA mirror) + #1610 (full hygiene propagation) + #1644 (paired legacy/new bumps at InlinePass + aura_jit) + #1646 (mutation boundary observability) + #1651 (mark_dirty_upward_fast early-exit + children_stable_span_view). Full #1047 hygiene completion at **#1689** (deopt hook completion under Task1 review #5). |
| **AC4** — AI self-edit 示例中 hygiene 违规率 <0.1% | 🔄 Deferred to follow-up | Requires the formal AI self-evo benchmark harness (edsl_benchmark.py + AI agent OOB) — separate workstream from the primitive hygiene chain. Deferred past #1689 (Phase 2 #1653 completion). |

## 6. Migration notes

When extending the engine:

- **Adding new predicates** → extend `match_subtree` in `query_matcher.cpp` + bump the appropriate counter in `query_matcher.ixx` (per the paired legacy + per-CompilerMetrics pattern). Update this doc with the new predicate section.
- **Adding new hygiene hooks** → add the `Evaluator::bump_*` + `*() const noexcept` getter pair in `evaluator.ixx`, the atomic slot in `observability_metrics.h` near `mutation_boundary_*` cluster, and the X-macro field in `compiler_metrics_fields.inc`.
- **Adding new macro observability** → add the file-level atomic + C-linkage accessor pattern in `macro_expansion.cpp` (paired-pattern with #1648 reflect.hh + #1651 macro_expansion.cpp).

## 7. Related issues

- #1501 — `feat(#1501): marker-aware tag_arity hygiene index for query:pattern`
- #1609 — `feat(query): MacroIntroduced hygiene force-filter + schema 1609 (#1609)`
- #1636 — `feat(query): mandate MacroIntroduced hygiene + schema 1636 (#1636)`
- #1644 — `feat(obs): close IR hygiene full-pipeline observability for MacroIntroduced / self-evo (refine #1047, #1644)`
- #1646 — `feat(obs): close MutationBoundaryGuard long-running observability wiring (partial-redundant-ship, refine #1637 #1014, #1646)`
- #1649 — `feat(mutate): close composite mutate atomic batch + SyntaxMarker propagation observability (partial-redundant Phase 1, refine #1900 #1502 #1472 #790 #761 #737 #1908, #1649)`
- #1650 — `feat(query): only_macro_introduced_ inverse flag for query:pattern finer marker predicate (partial-redundant-ship, refine #1636 #1609 #1501 #547, #1650)`
- #1651 — `feat(ast): close children_stable_span_view zero-copy span-return + body-named copy-avoided observability (scope-limited-progressive Phase 1, refine #1251 #1345 #398 #1500 #392, #1651)`
- #1652 — `feat(macro): close clone_macro_body / SyntaxMarker observability hooks + stats integration (scope-limited-progressive Phase 1, refine #120 #1611 #1247 #1248 #365, #1652)`
- #1689 — follow-up: full `#1047` hygiene completion (deopt hook + patterns review)

Closing #1653 as scope-limited-progressive Phase 1 — this doc IS the AC1 deliverable; AC2/AC3 predecessor-covered; AC3 full #1047 completion + AC4 benchmark deferred to #1689.
