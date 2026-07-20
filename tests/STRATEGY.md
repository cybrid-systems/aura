# Aura test strategy — hot paths & AI self-mod

**Authority (code):** [`src/test/test_strategy.h`](../src/test/test_strategy.h) ·
[`src/test/test_strategy.ixx`](../src/test/test_strategy.ixx)  
**Issue:** #1887  
**Observability:** `(engine:metrics "query:test-strategy-stats")` (schema **1887**)

This document formalizes the **high-level** coverage strategy for production
readiness. It complements (does not replace):

- Theme domain suites — [`tests/README.md`](README.md), [`domain/`](domain/)
- Bundle / issue-tier subtraction — #881 / #883
- Unified harness — [`test_harness.hpp`](test_harness.hpp) (#1960)
- Fixture shards — #1962

## Goals

| Goal | Target |
|------|--------|
| Hot-path matrix visibility | Every critical scenario named + issue-linked |
| AI self-mod confidence | Closed-loop stamps + `self_mod` SLO (≥1000 loops in stress) |
| Agent discoverability | Metrics via `query:test-strategy-stats` |
| Low binary sprawl | Prefer **domain/** extensions over new `test_issue_*` |

## Hot-path coverage matrix

Defined as `kHotPathMatrix` / `HotPathScenario` in `test_strategy.h`:

| Scenario id | Title | Primary issues | Recommended suite |
|-------------|-------|----------------|-------------------|
| `mutate-steal-gc-old-closure` | Mutate + steal + GC + old closure | **#1624**, **#1627** | `domain/test_domain_fiber_orchestration` |
| `invalidate-jit-deopt` | Invalidate + JIT deopt | #1623, #740 | `test_eval_relower_hotpath_1623` |
| `fiber-guard-shape-epoch` | GuardShape / epoch + fiber | #836, #1627 | `domain/test_domain_fiber_orchestration` |
| `typed-mutation-invariant` | TypedMutationAudit invariants | #1614, #1544 | `domain/test_domain_typed_mutate` |
| `type-prop-invariant-corr` | TypeProp ↔ invariant | #1884, #1872 | `test_type_prop_invariant_correlation_1884` |
| `aot-hotupdate-audit` | AOT hot-update audit | #1882, #590 | `test_aot_hotupdate_typed_audit_1882` |
| `self-evolution-loop` | Self-evolution loop stats | #1883, #595 | `domain/test_obs_schema_matrix` |
| `render-hotpath-mutation` | Render hot-path under mutate | #1563, #1674 | `test_render_hotpath_stability_under_mutation` |

**P0 linkage (AC):** matrix rows explicitly reference production-readiness /
incremental issues **#1624** and **#1627** (and others). When those issues move,
update the matrix constants in the same PR.

## Strategy profiles

`StrategyProfile` selects which scenarios a harness should stamp:

| Profile | Use when |
|---------|----------|
| `Minimal` | Smoke / typed-mutation only |
| `HotPathCore` | mutate/steal/GC + invalidate/deopt + fiber guard |
| `AiSelfMod` | audit + type-prop corr + AOT + self-evo loop |
| `Full` | Full matrix (CI strategy suite) |

```cpp
#include "test/test_strategy.h"
using namespace aura::test::strategy;

select_profile(StrategyProfile::AiSelfMod);
note_hotpath_scenario(HotPathScenario::TypedMutationInvariant, /*pass=*/true);
note_self_mod_loop(true);
// After suite:
// coverage_hit_rate_bp(), self_mod_slo_met(), hotpath_coverage_slo_met()
```

Or with modules:

```cpp
import aura.test.strategy;
```

## Metrics

| Metric | Meaning |
|--------|---------|
| `coverage-hit-rate-bp` | Unique scenarios hit / 8 × 10000 |
| `total-hits` / `total-pass` / `total-fail` | Aggregate scenario stamps |
| `self-mod-loops` / `self-mod-loops-ok` | Closed-loop iterations |
| `self-mod-slo-met` | `self_mod_loops >= 1000` |
| `hotpath-coverage-slo-met` | coverage-bp ≥ 5000 |
| `matrix-count` | 8 |
| `schema` | 1887 |

## Production-readiness coverage goals

1. **Hot path:** Full strategy CI should drive `hotpath-coverage-slo-met == 1`
   (or document gaps).
2. **Self-mod stress:** Long-run harness should meet `self-mod-slo-met` and keep
   TypedMutationAudit fail path always-on (#1882).
3. **Observability:** New hot-path work should either extend a domain suite **or**
   stamp the matching `HotPathScenario` so dashboards stay honest.
4. **No orphan binaries:** New PRs cite this strategy (PR template checkbox).

## How to add a scenario

1. Append `HotPathScenario` + `kHotPathMatrix` row (bump `Count`).
2. Point `related_issue_*` at real issues (≥1 P0/P1).
3. Prefer an existing domain suite as `recommended_suite`.
4. Update this table + `query:test-strategy-stats` docs if keys change.
5. Stamp from the suite via `note_hotpath_scenario`.

## Related

- Module layering: [`docs/architecture.md`](../docs/architecture.md) (#1885)
- Naming / comments: [`docs/naming_convention.md`](../docs/naming_convention.md) (#1886)
- Contributing: [`docs/contributing.md`](../docs/contributing.md)
