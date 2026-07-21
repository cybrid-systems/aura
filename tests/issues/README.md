# tests/issues/

**Legacy** per-issue C++ binaries (`test_issue_N.cpp`).

## Policy

- **Do not add** new `test_issue_N.cpp` for routine ACs.
- Prefer `tests/domain/`, theme dirs (`mutation/`, `edsl/`, `fiber/`, …), or
  an existing `test_issues_*_batch.cpp` / domain batch.
- **~325** files are **bundle members** (`cmake/AuraIssueBundles.cmake`) —
  do not merge those without updating the profile member lists and
  `aura_issue_*_run()` entry points.
- **~290** files were orphaned (not in bundles/fixtures/CMake). Range
  batches below reclaimed consecutive waves; remaining orphans are
  candidates for later domain migration or deletion after inventory.

## Range batches (orphan reclaim → CI)

These reclaimed **orphans** (not in bundles/fixtures) into default-build targets:

| Target | Sections kept green |
|--------|---------------------|
| `test_issues_1382_1395_batch` | #1382–#1387, #1391–#1395 (full range) |
| `test_issues_1466_1478_batch` | #1466–#1468, #1470, #1473–#1476, #1478 |
| `test_issues_1644_1655_batch` | #1647, #1651, #1654 |
| `test_issues_1903_1908_batch` | #1903 |

AC-drift orphans extracted back to standalone files remain **unregistered**
(not CI) until fixed or moved into domain suites.

```bash
ninja -C build test_issues_1382_1395_batch
./build/test_issues_1382_1395_batch
```

Parent policy: [`../README.md`](../README.md) · domain: [`../domain/README.md`](../domain/README.md).
