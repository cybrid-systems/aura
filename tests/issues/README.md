# tests/issues/

**Legacy** location for per-issue `test_issue_N.cpp` binaries.

## Policy

- **Do not add** new sources here. Prefer theme dirs (`tests/edsl/`,
  `tests/mutation/`, `tests/fiber/`, …) or `tests/domain/`.
- Bundle members still resolve via `aura_resolve_test_cpp` (issues → domain →
  `tests/<theme>/` → root).

## Status (wave 59 / #1957)

All orphan range batches and special-case issues have been folded into theme
suites. The last holdout **#178** (NodeView P2996 split TU) lives under:

- `tests/edsl/test_issue_178.cpp` + `_reflect.cpp` + `_bridge.h`
- soft surface: `tests/edsl/test_edsl_macro_hygiene_batch.cpp` (wave47)
- full binary: `test_issue_178` / light_late bundle

Parent: [`../README.md`](../README.md).
