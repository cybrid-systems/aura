# tests/edsl/

EDSL / macro hygiene / pattern-query regression drivers.

Prefer extending a batch binary over adding a new `test_*.cpp` here.

## Suites

| Target | Role |
|--------|------|
| `test_edsl_pattern_hygiene_batch` | `query:pattern` MacroIntroduced mandate, tag_arity index (#547/#554), provenance (#1914), recursive filter (#421) |
| `test_edsl_macro_hygiene_batch` | IR hygiene e2e / propagation / closure provenance, contract + violation closed loops, self-evo marker/dirty (#595) |
| `test_edsl_hygiene_unit_batch` | Smaller unit ACs: per-eval macro-inline, metadata lock, workspace marker max, subtree shared_lock, tag_arity FlatAST perf |
| `test_ir_cache_v2` | Standalone FNV-1a IR cache hash (custom `add_executable`) |
| `test_error_merr` | Light-bundle harness pilot (`aura_issue_error_merr_run`) |

## Build / run

```bash
ninja -C build test_edsl_pattern_hygiene_batch test_edsl_macro_hygiene_batch test_edsl_hygiene_unit_batch
./build/test_edsl_pattern_hygiene_batch   # optional: AURA_STRESS_ITERS=20
```

Schema-only hygiene gates also live in `tests/core/test_hotpath_matrix_batch.cpp` — do not re-add those here.

## NodeView reflection (#178 / #268)

Full P2996 split-TU suite (relocated from `tests/issues/` wave 59):

| File | Role |
|------|------|
| `test_issue_178.cpp` | module TU — real `NodeView` + `aura_issue_178_run()` |
| `test_issue_178_reflect.cpp` | non-module + `-freflection` wire roundtrip |
| `test_issue_178_bridge.h` | cross-TU bridge |

Also soft-smoked in `test_edsl_macro_hygiene_batch` (`run_178_reflect_surface_smoke`).
