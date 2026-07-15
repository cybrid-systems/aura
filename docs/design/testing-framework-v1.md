# Aura Testing Framework v1.0 (#1452 entry)

> **Issue**: [#1452](https://github.com/cybrid-systems/aura/issues/1452)  
> **Status**: foundation landed (harness + binding + pets headless CI)  
> **Follow-ups**: #1455 self-evolution suite  
> **#1453 done**: hard binding + `check_test_coverage` + TestRegistry catalog  
> **#1454 done**: aura-pets headless TUI regression (`pets` suite + CI_CORE)

---

## 0. Goals (aligned with governance)

| Goal | Mechanism |
|------|-----------|
| Code-as-Memory | Named assertions, ordered sections, fail-loud summary |
| SlimSurface / facade | `edsl_self_test` covers `validate-new` / `stats:get` / `engine:surface` |
| Mutation safety | Existing mutate/snapshot sections + later fuzz layer |
| TUI protected | #1454 `./build.py test pets` + TUI binding globs |
| PR closed loop | `scripts/check_test_binding.py` in gate |

---

## 1. Layered architecture

```
┌─────────────────────────────────────────────────────────┐
│ L4  Integration / Pets   (#1454 ✓) headless TUI + CI    │
├─────────────────────────────────────────────────────────┤
│ L3  Property / Fuzz      (#1455+) mutation fuzz / chaos │
├─────────────────────────────────────────────────────────┤
│ L2  EDSL declarative     (#1452)  edsl-test-harness +   │
│                                   edsl_self_test.aura   │
├─────────────────────────────────────────────────────────┤
│ L1  C++ unit / issue     existing test_issue_*.cpp +    │
│                         test_harness.hpp                │
└─────────────────────────────────────────────────────────┘
```

---

## 2. EDSL harness (`lib/std/edsl-test-harness.aura`)

Immediate evaluation (no quoted `test-suite` body) — avoids known
`data_to_flat` issues with large quoted forms.

```scheme
(require "std/edsl-test-harness" all:)
(section "Query")
(it "def-use" …)
(describe "Governance" (lambda () (it "…" …)))
(summary)   ; → #t iff zero failures
```

API: `section` · `it` · `report` (alias) · `describe` · `fixture-set-code` ·
`summary` · `reset-totals`.

---

## 3. Binding gate (#1452 foundation → **#1453 hard gate**)

### Tools

| Script | Role |
|--------|------|
| `scripts/check_test_binding.py` | Prim production sources ↔ `tests/` pairing |
| `scripts/check_test_coverage.py` | Umbrella: binding + registry freshness |
| `scripts/gen_test_registry.py` | Lightweight TestRegistry → `docs/generated/test-registry.json` |
| `tests/test-binding-allowlist.txt` | Rare exceptions (prefer tests over allowlist) |
| `tests/test_test_binding_gate.py` | Unit tests for the gate |

```bash
python3 scripts/check_test_binding.py
python3 scripts/check_test_binding.py --require-name-mention   # stricter
python3 scripts/check_test_coverage.py
python3 scripts/gen_test_registry.py                          # regenerate catalog
python3 scripts/gen_test_registry.py --check                  # CI freshness
python3 tests/test_test_binding_gate.py
```

Wired into `./build.py gate` via `cmd_test_binding` (non-zero on violation).

### Production globs (force pairing)

- `src/compiler/evaluator_primitives*.cpp`
- `src/compiler/evaluator.ixx` / `evaluator_ctor.cpp` / `evaluator_eval_flat.cpp`
- `src/compiler/service.ixx`
- `src/compiler/observability_prims_decl.inc`, `primitives_*.h`, `security_capabilities.h`
- `lib/std/stats.aura`, `engine-metrics.aura`, `edsl-test-harness.aura`

### TestRegistry (lightweight)

Generated JSON indexes all `tests/test_*.cpp` with `@category` / `@reason` /
optional issue number. Not a runtime C++ registry — discovery + CI freshness only.

---

## 4. L4 aura-pets headless regression (#1454)

| Piece | Role |
|-------|------|
| `scripts/run_pets_regression.py` | Hard gate: demos + C++ smokes + suite |
| `tests/test_aura_pets_smoke.cpp` | Multi-demo TUI smoke (in `issues_fast`) |
| `tests/test_cyber_cat_smoke.cpp` | #1358 cyber_cat (also in `issues_fast`) |
| `tests/suite/aura_pets.aura` | Suite runner coverage |
| `examples/{snake,tetris,cyber_cat}.aura` | Headless demos (`--load`) |
| Binding globs | `src/tui/*`, `lib/std/tui/*`, `evaluator_primitives_tui.cpp`, demos |

```bash
./build.py test pets
python3 scripts/run_pets_regression.py
python3 scripts/run_pets_regression.py --demos-only
./build/aura --load examples/cyber_cat.aura
```

Wired into `CI_CORE` (every `./build.py ci`) and PR fast-tier builds via
`tests/fixtures/issues_fast.json`.

## 5. Out of scope (later issues)

| Item | Tracking |
|------|----------|
| Self-evolution reliability suite | #1455 |
| Property-based / mutation fuzz runner | #1455+ |
| Full in-process C++ TestRegistry class | future (catalog is enough for now) |
| Interactive TTY snapshot / golden frames | future (headless smoke is enough for now) |

---

## 6. Run

```bash
./build/aura < tests/edsl_self_test.aura
./build.py test suite          # includes suite/edsl_self_test.aura + aura_pets
./build.py test pets           # #1454 headless TUI regression
./build.py gate                # includes check_test_binding
```
