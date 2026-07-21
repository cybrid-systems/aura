# tests/issues/

**Legacy** per-issue C++ binaries (`test_issue_N.cpp`).

## Policy

- **Do not add** new `test_issue_N.cpp` for routine ACs.
- Prefer `tests/domain/`, theme dirs (`mutation/`, `edsl/`, `fiber/`, …), or
  an existing domain/theme batch.
- Bundle members (`cmake/AuraIssueBundles.cmake`) — do not merge those without
  updating the profile member lists and `aura_issue_*_run()` entry points.

## Remaining special-case

| Target | Notes |
|--------|-------|
| `test_issue_178` (+ `_reflect` + `test_issue_178_bridge.h`) | P2996 split TU / NodeView; needs `-freflection` / light_late |

## Orphan range batches (reclaimed → theme soft smokes)

Wave 58 (#1957) folded the last four orphan range batches into theme suites:

| Former target | Sections | Theme destinations |
|---------------|----------|--------------------|
| `test_issues_1382_1395_batch` | #1382–#1387, #1391–#1395 | arena compact, linear, mutation, edsl, fiber, shape |
| `test_issues_1466_1478_batch` | #1466–#1468, #1470, #1473–#1476, #1478 | obs, arena, shape, mutation, linear |
| `test_issues_1644_1655_batch` | #1647, #1651, #1654 | obs + mutation |
| `test_issues_1903_1908_batch` | #1903 | obs (`query:envframe-dual-consistency-stats`) |

Parent policy: [`../README.md`](../README.md) · domain: [`../domain/README.md`](../domain/README.md).
