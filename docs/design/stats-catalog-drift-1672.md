# Stats catalog ↔ impl drift check (#1672)

**Issue:** [#1672](https://github.com/cybrid-systems/aura/issues/1672)  
**Builds on:** #1439 (internal stats table) · #1450 (residual public aliases) · #1671  
**Status:** P2 architecture — runtime drift diagnostic without public-surface growth.

## Problem

Three sources of “which stats exist”:

1. `kObservabilityStatsPrimitives` — catalog for `(stats:list)` / `(stats:count)`
2. Peel-tier `register_stats_impl(...)` — actual internal map
3. Residual public `Primitives` aliases (#1450)

No check that catalog entries resolve or that every impl is catalogued → silent
`(stats:get)` voids or undiscoverable impls.

## Fix (Option C)

| Piece | Detail |
|-------|--------|
| `ObservabilityPrims::stats_drift_check(ev)` | Builds hash schema **1672** |
| Registration | `register_stats_impl("stats:drift-check", …)` in `register_metrics_facade` |
| EDSL | `(stats:get "stats:drift-check")` / `(engine:metrics "stats:drift-check")` |
| Public `add()` | **None** (SlimSurface interim ceiling 520) |

### Hash fields

| Key | Meaning |
|-----|---------|
| `schema` | 1672 |
| `catalog-size` | `kObservabilityStatsPrimitives.size()` |
| `impl-size` | `legacy_stats_impls().size()` |
| `missing-impl-count` | catalog names not resolvable via impl **or** public prim |
| `missing-catalog-count` | impl names not in catalog (excludes meta `stats:drift-check`) |
| `ok` | 1 iff both drift lists empty |
| `missing-impl` | list of names |
| `missing-catalog` | list of names |

Resolution for catalog entries matches `(stats:get)`: `lookup_stats_impl` then
public `Primitives` slot (residual aliases).

## Catalog hygiene shipped with #1672

Ghost catalog entries (never resolvable) removed or renamed to match impls:

| Was | Now |
|-----|-----|
| `query:envframe-stale-stats` / `envframe-bump-stats` | removed |
| `compile:compiler-cache-stats` | `query:compiler-cache-stats` |
| `compile:compiler-incremental-stats` | `query:compiler-incremental-stats` |
| `compile:typecheck-stats` | `compile:incremental-typecheck-stats` |
| `compile:jit-stats` | `query:jit-stats` |
| `compile:arena-stats` / `compile:mutation-impact-stats` | removed |

Reverse drift (`missing-catalog`) only counts names where
`is_legacy_stats_name` is true (skips accidental non-stats keys in the map).

CI hard-asserts **`missing-impl-count == 0`**. Reverse catch-up is incremental.

## Non-goals

- Auto-rebuild catalog from the map (loses intentional ordering / issue notes).
- New public primitive `(stats:drift-check)` as `add()` name.
- Zero reverse drift in one commit (hundreds of impl-only names).

## Tests

`tests/test_stats_catalog_drift_1672.cpp`
