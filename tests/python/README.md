# tests/python/

Shared Python harness, unified runner, and gate unit tests
(Issues #1932 / #1939).

| File | Role |
|------|------|
| `run.py` | Primary CLI (`python3 tests/run.py …` thin entry at root) |
| `run_issue_tests.py` | C++ issue/domain/bundle binary runner |
| `_aura_harness.py` | Shared ROOT/BUILD helpers + RunReport |
| `issue_tier.py` | fast/full tier selection |
| `fixture_store.py` / `fixture_check.py` | fixtures shard loader + validator |
| `*_cases.py` | integ / smoke / regression case adapters |
| `e2e_harness.py` / `run_e2e.py` | commercial_readiness E2E helpers (#1934) |
| `test_*_gate.py` / `test_layout_*.py` / `test_audit_*.py` | gate unit tests |

Thin entrypoints at `tests/*.py` remain stable for docs and CI.
Policy check: `python3 tests/migrate_test_layout.py --status`.
