# Migration: `query:*-stats` → `(engine:metrics …)` (v2.0 / #1439)

## What changed

As of **#1439**, all **`query:*-stats`** and **`compile:*-stats`** names are **removed from the public primitive registry**. They are no longer listed by `(api-reference)`, and `(query:foo-stats)` is no longer a valid top-level call.

Hash builders still exist **internally** so observability data is preserved. The only supported entry points are:

| Use case | Call |
|----------|------|
| One named stats hash (compat) | `(engine:metrics "query:foo-stats")` |
| Nested CompilerMetrics groups | `(engine:metrics)` or `(engine:metrics :group "jit")` |
| Filter by name/field prefix | `(engine:metrics :prefix "query:")` |
| Dump every legacy stats hash | `(engine:metrics :all)` |
| Stdlib catalog wrapper | `(require "std/stats" all:)` → `(stats:get "…")` (routes to facade) |
| Thin product wrappers | `(require "std/engine-metrics" all:)` |

## Before / after

```scheme
;; BEFORE (v1.x — public prim)
(query:pattern-stats)
(query:macro-hygiene-stats)

;; AFTER (v2.0)
(engine:metrics "query:pattern-stats")
(engine:metrics "query:macro-hygiene-stats")

;; Prefer structured metrics when you only need counters:
(engine:metrics :group "jit")
(hash-ref (engine:metrics :group "jit") "jit_compilations")
```

C++ issue tests:

```cpp
// BEFORE
cs.eval("(query:pattern-stats)");

// AFTER
cs.eval("(engine:metrics \"query:pattern-stats\")");
```

## What was *not* removed

Names that are **not** `query:` / `compile:` stats stay public, e.g.:

- `gc-stats`, `arena:defrag-stats`, `ast:generation-stats`
- Structural `query:*` (node/children/find/…) — use `(query :op …)` (#1435)
- `stats:list` / `stats:count` / `engine:metrics` themselves

## Agent / stdlib guidance

1. Prefer **`(engine:metrics)`** groups for new Agent loops.
2. Use **by-name** only when an existing issue contract requires a specific schema hash (e.g. `schema == 778`).
3. **Never** add a new `query:*-stats` primitive — extend `CompilerMetrics` + facade fields instead (`scripts/check_primitive_surface.py` freezes this).

## Tooling

| Script | Role |
|--------|------|
| `scripts/find_top_stats.py` | Rank remaining string references |
| `scripts/migrate_top_stats_to_facade.py` | Rewrite call sites (top-20 or all via local bulk) |
| `scripts/check_primitive_surface.py` | Freeze gate: no new `*-stats` public names |

## Related

- Design: [design/primitives-surface-refactor.md](design/primitives-surface-refactor.md) §4–5 (Phase 5)
- Issues: #1433 (facade), #1434 (top-20 deprecate), **#1439** (remove public regs)
- Stdlib: `lib/std/engine-metrics.aura`, `lib/std/stats.aura`
