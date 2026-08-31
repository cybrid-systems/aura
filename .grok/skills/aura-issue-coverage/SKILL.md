---
name: aura-issue-coverage
description: How Aura issues ship contracts — runtime tests, JSON manifests, custom check_*.py. Use when adding an issue, AC, source-cite, coverage linter, check_*.py, or scripts/coverage/manifests JSON. Triggers: new issue, source-cite, check script, coverage linter, AC1, manifest, /aura-issue-coverage.
---

# Aura issue coverage

Read [tests/COVERAGE.md](../../../tests/COVERAGE.md) and follow its decision tree. Do not invent a parallel process.

## Default (almost every issue)

1. **Behavior** → extend the existing suite in `tests/<src-module>/` (`HOMES.md`). No new `test_*_<issue>.cpp`.
2. **Source still contains X** → `scripts/coverage/manifests/<N>.json` (`contains` / `contains_any` / `forbid`). Stop.
3. Do **not** add `scripts/coverage/checks/check_*.py` unless the contract needs a window, regex, AST, or statement order.
4. Do **not** add `./build.py <name>-coverage` or a `build.py` “script is wired” row. Gate auto-discovers manifests and remaining `check_*.py`.
5. Do **not** add C++ `CHECK(src.find("Issue #N"))` as the only lock; that belongs in the manifest.

## If you must write Python

File: `scripts/coverage/checks/check_<feature>_<N>.py`. It must not be “`needle in file`” only. `check_coverage_policy.py` fails substring-only and thin `runner.py --issue N` wrappers.

## Forbidden

- `tests/issues/test_issue_N.cpp`, `tests/**/test_*_<digits>.cpp`
- `docs/design/<N>-*`
- new `scripts/check_*.py` (directory frozen)

Gate: `./build.py gate --changed`. Manifest-only: `python3 scripts/coverage/runner.py --issue N`.
