# catch(...) silent-swallow audit (#1669)

**Issue:** [#1669](https://github.com/cybrid-systems/aura/issues/1669)  
**Builds on:** #615 (`[SILENCE-PRIM-#…]` convention) · #1488 / #1668 (audit-pass shape)  
**Status:** P1 audit — full `src/compiler` classified; unmarked = gate fail.

## Problem

EDSL primitives use `PrimFn → EvalValue`. A bare `catch (...)` that returns a
default (or continues) **without** converting to an error sentinel can hide
failures from Aura callers.

## Classification

| Class | Meaning | Action |
|-------|---------|--------|
| **A** Intentional-return-value | Catch → `#f` / `PRIM_ERROR` / `false` / typed zero with documented contract | Mark `[SILENCE-PRIM-#615]` |
| **B** Intentional-state-change | Catch → mutate report/flag/break best-effort trail | Mark + document |
| **C** Potential-silent-swallow | Catch → default with no signal | Fix (error sentinel / rethrow / metrics) + follow-up |

## Tooling

```bash
python3 scripts/audit_catch_silent_swallow.py          # unmarked report
python3 scripts/audit_catch_silent_swallow.py --strict # CI gate
python3 scripts/audit_catch_silent_swallow.py --all    # full inventory
./build.py catch-silent-swallow                        # unit tests + --strict
./build.py gate                                        # includes this audit
```

Rule: every `catch (...)` in `src/compiler/**/*.{cpp,ixx}` must have
`SILENCE-PRIM` in the catch body window (≤12 lines).

## Audit results (2026-07-18 / #1669)

**0 class-C sites.** All ~31 live `catch (...)` sites are class A or B.

| Cluster | Class | Contract |
|---------|-------|----------|
| `math` regex ×4 | A | `bump_regex_error` → PRIM_ERROR (#668) |
| `pair` string→num | A | `#f` on parse fail |
| `workspace` snapshot-N / eval | A | `#f` / void → rollback path |
| `query_workspace` predicates | A | filter de-select / display fallback |
| `types` hot_swap | A | `ok=false` bool return |
| `compile_03` metrics | B | zeros already init |
| `compile_04` EDA guard ×2 | A | `#f` + `eda_guard_uncaught_exception_total` |
| `agent` rate-limit / analytics / tasks | A/B | defaults / `tr.error` / join-status |
| `ast` snapshot OOM / restore | A | `has_flat=false` / fallthrough |
| `eval_flat` snapshot / restore | A | same as ast |
| `persist` wire_read | B | break partial mutation trail |
| `typecheck` visitor | A | `type_ok=false` |
| `ir_executor` coerce ×3 | A | 0 / 0.0 + log |
| `query.ixx` stoll | A | empty `ReplaceTemplate` |
| `service.ixx` cache dir / literal parse | A/B | best-effort / fallthrough cascade |

No #1671+ follow-ups required.

## Tests

| File | Role |
|------|------|
| `tests/test_audit_catch_silent_swallow.py` | Marker detection + src/compiler clean |

## AC map (#1669)

| AC | Status |
|----|--------|
| 1 Read + classify all catch(...) | Done |
| 2 Mark A/B with SILENCE-PRIM | Done |
| 3 Fix C or file follow-ups | N/A (0 C) |
| 4 Audit script + gate | Done |
| 5 Docs | this file |

## Non-goals

- Replacing intentional `#f` parse contracts with exceptions.
- Catching `std::exception` variants (only `catch (...)` hygiene).
- Runtime exception-to-EDSL automatic wrapping (separate track).
