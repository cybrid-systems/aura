# tests/fixtures/

JSON test cases (`regression/`, `integ/`, `benchmark/`, `smoke/`).

Load: `tests/python/fixture_store.py` → `load_case_array(kind)`.
Validate: `python3 tests/run.py fixtures` (enforces 12 KB / 50 cases per shard).
