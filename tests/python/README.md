# tests/python/

Shared Python harness, unified runner, and gate unit tests (Issue #1932).

| File | Role |
|------|------|
| `run.py` | Primary CLI (`python3 tests/run.py …` thin entry at root) |
| `run_issue_tests.py` | C++ issue/domain/bundle binary runner |
| `_aura_harness.py` | Shared ROOT/BUILD helpers + RunReport |
| `issue_tier.py` | fast/full tier selection |
| `fixture_store.py` / `fixture_check.py` | fixtures shard loader + validator |
| `*_cases.py` | integ / smoke / regression case adapters |
| `test_*_gate.py` / `test_audit_*.py` | unit tests for gate scripts |

Thin entrypoints at `tests/*.py` remain stable for docs and CI.
