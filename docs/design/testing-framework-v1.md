# Aura Testing Framework v1.0 (#1452 entry)

> **Issue**: [#1452](https://github.com/cybrid-systems/aura/issues/1452)  
> **Status**: foundation landed (harness + binding gate + declarative self-test)  
> **Follow-ups**: #1453 test-binding hard gate expansion · #1454 aura-pets CI · #1455 self-evolution suite  

---

## 0. Goals (aligned with governance)

| Goal | Mechanism |
|------|-----------|
| Code-as-Memory | Named assertions, ordered sections, fail-loud summary |
| SlimSurface / facade | `edsl_self_test` covers `validate-new` / `stats:get` / `engine:surface` |
| Mutation safety | Existing mutate/snapshot sections + later fuzz layer |
| TUI protected | Deferred to #1454 (aura-pets headless CI) |
| PR closed loop | `scripts/check_test_binding.py` in gate |

---

## 1. Layered architecture

```
┌─────────────────────────────────────────────────────────┐
│ L4  Integration / Pets   (#1454)  headless TUI + CI     │
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

## 3. Binding gate (`scripts/check_test_binding.py`)

If production primitive sources change, require a matching change under
`tests/` (or explicit allowlist docs-only).

```bash
python3 scripts/check_test_binding.py          # vs origin/main…HEAD
python3 scripts/check_test_binding.py --base HEAD~1
```

Wired into `./build.py gate` (non-zero on violation).

Production globs:

- `src/compiler/evaluator_primitives*.cpp`
- `src/compiler/evaluator.ixx`
- `src/compiler/observability_prims_decl.inc`

---

## 4. Out of scope for this entry issue

| Item | Tracking |
|------|----------|
| Full C++ `TestRegistry` unification | #1453 |
| aura-pets headless workflow | #1454 |
| Self-evolution reliability suite | #1455 |
| Property-based / mutation fuzz runner | #1455+ |

---

## 5. Run

```bash
./build/aura < tests/edsl_self_test.aura
./build.py test suite          # includes suite/edsl_self_test.aura
./build.py gate                # includes check_test_binding
```
