# Primitives SlimSurface v2 — Governance + CI Gate

> **Issue**: [#1448](https://github.com/cybrid-systems/aura/issues/1448) (P0 infrastructure)  
> **Status**: landed (infrastructure). Surface shrink to ≤420 continues in demotion batches.  
> **Supersedes / pairs with**: [primitives-surface-refactor.md](primitives-surface-refactor.md),  
> [primitive-vs-stdlib-decision-framework.md](primitive-vs-stdlib-decision-framework.md),  
> [agentic-slim-surface-rectification.md](agentic-slim-surface-rectification.md)

---

## 0. TL;DR

| Control | Mechanism | Hard / soft |
|---------|-----------|-------------|
| No *new* blocked-pattern names | `scripts/check_primitive_surface.py` freeze vs baseline | **Hard** (gate / CI) |
| Budget toward ≤420 public names | `--strict` interim ceiling 700 → target 420 | Hard above ceiling; soft note above target |
| Observability is data | `register_stats_impl` + `(engine:metrics)` — not new public `*-stats` | **Hard** freeze |
| Deprecation is visible | `PrimMeta.deprecated` + dispatch counter | Hard (runtime + test) |
| Category taxonomy | `PrimMeta.category` required for new / demoted names | Soft → hard in docs gen |

**Public `add()` surface today**: ~616 names (see inventory).  
**Internal stats catalog** (`stats:list` / `stats:count`): ~390 (already ≤420 target for catalog).  
**SlimSurface epic target**: public engine primitives ≤ **420**.

---

## 1. Goals (Issue #1448)

1. Engine primitives converge to **≤420** public names.  
2. All observability goes through **facade** (`engine:metrics` / `stats:list` catalog).  
3. New primitives require **review + stdlib-first** (red-line check).  
4. Infrastructure: script gate, deprecation dispatch, design norm, CI, tests.

This issue ships **infrastructure**, not the full demotion to 420. Demotion batches delete / hide names under the freeze.

---

## 2. Acceptance criteria map

| AC | Deliverable | Status |
|----|-------------|--------|
| 1. `--strict` | `scripts/check_primitive_surface.py --strict` | Done — budget + facade report |
| 2. `PrimMeta.deprecated` + dispatch | Field + `invoke_prim_with_telemetry` counter | Done |
| 3. This design doc + category rules | `docs/design/primitives-slim-surface-v2.md` | Done |
| 4. CI gate | `./build.py gate` runs freeze + **strict** + unit tests | Done |
| 5. `stats:count` ≤420 target | Catalog size via `stats:count` (≤420 now) | Met for catalog; public shrink continues |
| 6. Convergence test | `tests/test_primitives_surface_convergence.cpp` | Done |

---

## 3. Registry governance

### 3.1 Blocked patterns (no *new* names)

Enforced by freeze baseline `docs/generated/stats-primitives-baseline.json`:

- `*-stats`, `*-stats-hash`, `*-stats-*`
- Convenience prefixes: `string-`/`string:`, `json-`/`json:`, `math-`/`math:`,  
  `vector-`/`vector:`, `path-`/`path:`, `time-`/`time:`
- `ast:ref-*` (Issue #393)

**Allowlist is empty.** Intentional growth:

```bash
python3 scripts/check_primitive_surface.py --update-baseline
```

…only with PR justification linking red-line criteria.

### 3.2 Budget (`--strict`)

| Constant | Value | Meaning |
|----------|------:|---------|
| `TARGET_BUDGET` | 420 | Epic end state |
| `INTERIM_HARD_CEILING` | 700 | Fail if public `add()` count grows past this |

Strict also reports:

- category histogram (`query` / `mutate` / `compile` / `workspace` / `engine` / `stats-like` / `other`)
- remaining public `add()` `*-stats` (debt; prefer facade)

### 3.3 Categories (`PrimMeta.category`)

| Category | Use |
|----------|-----|
| `general` | Default core language / AI work surface |
| `eda` / `sva` / `verification` | Domain packs (may be s0-gated) |
| `deprecated` | Compat aliases still registered; prefer op-dispatch / facade |
| (empty) | Legacy; new work must set a category |

**Rules:**

1. New `add()` should set `category` (and `schema` / `doc` when agent-visible).  
2. Demotion marks aliases `deprecated=true` and `category="deprecated"`.  
3. `api-reference` groups deprecated names under `*deprecated*` (see gen_docs).  
4. Preferred agent surface: `(query :op …)`, `(mutate :op …)`, `(workspace :op …)`,  
   `(engine:metrics …)`, stdlib `lib/std/surface`.

### 3.4 Red-line before any new primitive

Follow [primitive-vs-stdlib-decision-framework.md](primitive-vs-stdlib-decision-framework.md).  
Default = **stdlib**. Promote only on red-line + PR review.

**PR checklist (process + gate):**

- [ ] Red-line criterion cited in PR description  
- [ ] Prefer `lib/std` wrapper if not red-line  
- [ ] No new blocked-pattern name (freeze will fail)  
- [ ] If observability: extend `CompilerMetrics` + facade, not a new `*-stats`  
- [ ] Test binding for behavior change  

---

## 4. Deprecation dispatch (runtime)

```text
invoke_prim_with_telemetry(name, call)
  → if PrimMeta.deprecated:
       deprecated_prim_dispatch_total_++
       CompilerMetrics::prim_write_side_deprecation_hits++
  → capability gates …
  → call()
```

- **Still executes** (compat).  
- Agents observe debt via `Evaluator::deprecated_prim_dispatch_total()` and  
  `(engine:metrics)` field `prim_write_side_deprecation_hits`.  
- Prefer removing callers; then hard-delete the name in a demotion PR.

Known bulk markers: #1434 top-20 stats, #1435 `query:*` aliases, #1436 `mutate:*`,  
#1437 `workspace:*`.

---

## 5. Observability facade-only

| Path | Role |
|------|------|
| `ObservabilityPrims::register_stats_impl` | Internal catalog / metrics fields |
| `(stats:list)` / `(stats:count)` | Catalog of internal stats names |
| `(engine:metrics …)` | Live counters (schema 2 groups) |
| Public `add("…-stats")` | **Frozen** — no new names |

Migration: [migration-stats-to-metrics.md](../migration-stats-to-metrics.md).

---

## 6. Tooling

```bash
# Default freeze (CI / gate)
python3 scripts/check_primitive_surface.py

# SlimSurface governance (#1448)
python3 scripts/check_primitive_surface.py --strict

# Refresh inventory JSON
python3 scripts/check_primitive_surface.py --write

# Intentional freeze growth (rare)
python3 scripts/check_primitive_surface.py --update-baseline

# Unit + synthetic injection
python3 tests/test_primitive_surface_gate.py

# Runtime convergence ACs
./build/test_primitives_surface_convergence
```

`./build.py gate` runs freeze + **strict** + Python unit tests.

---

## 7. File map

| Path | Role |
|------|------|
| `scripts/check_primitive_surface.py` | Freeze + `--strict` |
| `docs/generated/stats-primitives-baseline.json` | Frozen blocked-pattern set |
| `docs/generated/primitive-inventory.json` | Full inventory snapshot |
| `src/compiler/evaluator.ixx` | `PrimMeta`, dispatch deprecation counter |
| `src/compiler/evaluator_primitives_observability.cpp` | Stats catalog + deprecation marks |
| `tests/test_primitive_surface_gate.py` | Pattern / freeze unit tests |
| `tests/test_primitives_surface_convergence.cpp` | Runtime SlimSurface ACs |
| `docs/design/primitives-slim-surface-v2.md` | This document |

---

## 8. Out of scope (follow-ups)

- Hard-delete of grandfathered convenience / remaining public stats (demotion PRs).  
- Lowering `INTERIM_HARD_CEILING` toward 420 as shrink lands.  
- Full PR-bot “stdlib wrapper attached?” automation beyond freeze (process).  

---

*Issue #1448 SlimSurface infrastructure. Keep this doc as the v2 governance source of truth; demotion batches link here.*
