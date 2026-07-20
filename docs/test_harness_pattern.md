# Test harness pattern — Issue #1932

How Aura organizes tests for long-term maintainability.

## Layout (target)

```
tests/
├── domain/           # Preferred C++ theme suites
├── suite/            # Aura language E2E (.aura)
├── regression/       # Curated redlines
├── fixtures/         # Shared JSON shards
├── templates/        # Copy-paste C++ starters (not built)
├── issues/           # Legacy per-issue C++ (do not add new)
├── python/           # Harness, runners, gate unit tests
├── bench/            # Benchmarks (C++ + Python drivers + baseline)
├── fuzz/             # Fuzz drivers / corpus / reproducers
├── memory/           # Long-run leak / soak scripts
├── bundles/          # Generated issue-test bundles
├── run.py            # Thin entry → python/run.py
└── README.md
```

## Public CLI (stable)

Users and CI keep using:

```bash
python3 tests/run.py list
python3 tests/run.py issues-fast
python3 tests/run.py fixtures
./build.py test issues-fast
./build.py gate
./build.py check
```

Thin entrypoints at `tests/*.py` forward into `tests/python/` or
`tests/bench/` so path moves do not break scripts or docs.

## Migration

```bash
python3 tests/migrate_test_layout.py --dry-run
python3 tests/migrate_test_layout.py --apply   # idempotent leftover moves
```

## Contribution checklist

1. Prefer `tests/domain/` for new C++ coverage (see `tests/README.md`).
2. Put new Python drivers under `tests/python/`, `tests/bench/`,
   `tests/fuzz/`, or `tests/memory/` — **not** the top-level `tests/` root.
3. Keep or add a thin entrypoint only when a public path must stay stable.
4. Run `./build.py gate` before push.

## Related

- Parent issue: [#1932](https://github.com/cybrid-systems/aura/issues/1932)
- Subtasks: #1937 (design + script), #1938 (fuzz/bench/memory moves)
- Strategy matrix: `tests/STRATEGY.md` (#1887)
