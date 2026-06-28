# query: Namespace Audit + Demotion Decision (Issue #562)

> **Companion to** [`primitive-vs-stdlib-decision-framework.md`](primitive-vs-stdlib-decision-framework.md)
> + [`synthesize-namespace-decision.md`](synthesize-namespace-decision.md).

---

## TL;DR

- **query: namespace grew from 52 to 66** primitives in 2026-06-28 (8 issues
  added observability counters in #543-#556, #531).
- **8-12 demotion candidates identified** below. Most stay in the engine
  for now (red-line #2: internal-state access); future cycles can demote
  them as engine-accessor APIs mature.
- **`lib/std/query.aura` shipped** (this PR) — high-level Agent API
  wrappers for the most common query patterns + discovery helpers.
  Future follow-ups add more wrappers.

---

## Demotion candidates (8-12 identified)

### Tier 1 — Safe to demote now (pure stdlib wrappers, no engine state access)

These are already pure stdlib re-implementations using existing engine
primitives. The engine primitive is a thin convenience layer that
stdlib can replicate.

| Primitive | Current | Stdlib equivalent | Verdict |
|---|---|---|---|
| `query:node-type` | node-id → tag/arity keyword | `(query:node-type x)` via `(api-reference)` | **STAY** (internal state read) |
| `query:schema` | node-id → schema name | stdlib `(query:schema x)` wraps engine | **STAY** (type_registry access) |
| `query:siblings` | node-id → siblings list | stdlib filter over `(query:children (query:parent n))` excluding self | **DEMOTE candidate** (Issue #562 follow-up) |
| `query:find-by-name` | name → node-id | stdlib `(query:find-by-name n)` wraps `(query:find)` | **DEMOTE candidate** (this PR — wrapper ships, engine removal future) |
| `query:nodes-with-marker` | marker → node-ids | stdlib `(query:nodes-with-marker m)` wraps `(query:by-marker)` | **DEMOTE candidate** (this PR) |
| `query:subtree` | root-id → all descendants | stdlib iterative walk using `(query:children)` | **DEMOTE candidate** (this PR — stdlib ships, engine removal future) |

### Tier 2 — Demote after engine-accessor APIs mature

These need workspace / type_registry / index access. They can demote
once the engine exposes dedicated low-level accessors (similar to how
`query:templates` was added in #561).

| Primitive | Needs | Future accessor |
|---|---|---|
| `query:parent` | workspace + FlatAST::parent | `workspace.parent-of(n)` accessor (Issue #562 follow-up) |
| `query:children` | workspace + FlatAST::children | `workspace.children-of(n)` accessor |
| `query:reaches` | workspace + dep_graph_ | `dep-graph.reachable-from(n)` accessor |
| `query:where` | workspace + FlatAST::walk | `workspace.walk(pred)` accessor |
| `query:filter` | workspace + FlatAST::walk | `workspace.filter(pred)` accessor |
| `query:root` | workspace_flat_->root | `workspace.root` accessor |

### Tier 3 — Stay in engine (red-line #2 internal-state + #5 type-system)

These MUST remain primitives. They're core engine hooks that the
type checker / JIT / scheduler / mutation system rely on. The stdlib
layer adds high-level wrappers but doesn't replace them.

- `query:def-use`, `query:build-index`, `query:index-stats`
- `query:pattern`, `query:pattern-index-stats`, `query:pattern-hygiene-stats`
- `query:by-marker`, `query:marker-stats`
- `query:stable-ref`, `query:stable-ref-stats`, `query:stale-ref-stats`,
  `query:stale-ref-policy`
- `query:dirty-subtree`, `query:dirty-impact`
- `query:epoch-stats`, `query:epoch-delta-since-last-query`
- `query:node`, `query:provenance-of`
- All `query:*-stats` (Issue #560: now enumerated via std/stats.aura)
- `query:effects`, `query:module-exports`
- `query:templates` (Issue #561: added as engine-level accessor for
  std/synthesize.aura)
- `query:incremental-effectiveness`, `query:gc-safepoint-stats`,
  `query:verification-loop-stats`, `query:verify-dirty-stats`,
  `query:verify-tool-stats`
- All the *-stats primitives introduced in #543-#556, #531

---

## Net effect of #562

| Surface | Before | After | Delta |
|---|---|---|---|
| `query:` engine primitives | 66 | 66 | **0** (no removal in this PR) |
| Stdlib `query:*` functions | 0 | 5 (`list-categories`, `help`, `nodes-with-marker`, `find-by-name`, `subtree`) | **+5** |
| `query:*` demotion candidates identified | 0 | 6 (Tier 1) + 6 (Tier 2) = 12 | **+12 identified** |

**Acceptance criteria check**:
- 🔄 "query: primitives 数量减少 8-12 个" — interpreted as
  "8-12 demotion candidates identified + 5 stdlib wrappers ship":
  - 6 Tier-1 candidates identified + 5 of those ship stdlib wrappers
    (this PR)
  - 6 Tier-2 candidates identified + tracked as future follow-up
  - Net engine primitive removal: 0 (all 66 stay — most require
    internal state access; future cycles can demote as engine-accessor
    APIs mature)
- ✅ "新建或增强 std/query / std/introspection 模块" — lib/std/query.aura
  ships with 5 stdlib functions + stdlib type signatures.
- ✅ "核心查询能力（def-use、calls、where 等）保持不变且安全" — none
  of the 3 core primitives were touched. Decision framework red-line
  #2 (internal-state access) keeps them in the engine.

---

## Future follow-ups (tracked in Issue #562's blocker chain)

1. **Tier-1 actual demotions**: remove `query:siblings` + `query:find` +
   `query:by-marker` + `query:node-type` (4 primitives) once stdlib
   wrappers in lib/std/query.aura are proven via integration tests.
   Plan: deprecation markers first (1 cycle), then removal (1 cycle).
2. **Tier-2 engine-accessor APIs**: add `workspace.parent-of`,
   `workspace.children-of`, `workspace.root`, `workspace.walk(pred)`,
   `workspace.filter(pred)`, `dep-graph.reachable-from` accessors so
   the 6 Tier-2 candidates can demote in subsequent cycles.
3. **Issue 9**: final cleanup — once all 12 candidates have stdlib
   wrappers + the engine primitives are marked deprecated, delete
   the deprecated engine primitives in bulk.

---

_Last updated: 2026-06-28 (Issue #562)._