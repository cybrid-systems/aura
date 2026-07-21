# Test harness pattern — Issue #1932 / #1939

How Aura organizes tests for long-term maintainability.

## Layout (final)

```
tests/
├── domain/           # Preferred C++ theme suites
├── suite/            # Aura language E2E (.aura)
├── regression/       # Curated redlines
├── fixtures/         # Shared JSON shards
├── templates/        # Copy-paste C++ starters (not built)
├── issues/           # Legacy per-issue C++ (do not add new)
├── python/           # Harness, runners, gate unit tests  ← full drivers
├── e2e/              # Strengthened .aura E2E + commercial_readiness (#1934)
├── bench/            # Benchmarks (C++ + Python drivers + baseline)
├── fuzz/             # Fuzz orchestrator / drivers / corpus (#1935)
├── bundles/          # Generated issue-test bundles
├── run.py            # Thin entry → python/run.py
├── migrate_test_layout.py  # Idempotent mover + --status (#1937 / #1939)
└── README.md
```

**#1939 final cleanup:** full Python harness lives only under `python/` /
`bench/` / `fuzz/` / `e2e/`. Top-level `tests/*.py` are **thin
entrypoints** (stable CLI). C++ `test_*.cpp` bulk remains at root until domain
migration waves (#1957).

## Public CLI (stable)

Users and CI keep using:

```bash
python3 tests/run.py list
python3 tests/run.py issues-fast
python3 tests/run.py fixtures
./build.py test issues-fast
./build.py gate
./build.py check
./build.py coverage --html          # LLVM source coverage (#1933)
./build.py coverage --check-tools
```

See [`docs/testing.md`](testing.md) for coverage details (preset, artifacts, CI).

## Strengthened .aura E2E (#1934)

Commercial readiness and high-value language paths live under
`tests/e2e/commercial_readiness/` with **machine-checkable** PASS/FAIL
lines and `E2E-PASS` markers. Python helpers:

| API | Role |
|-----|------|
| `e2e_harness.run_aura_file` | `aura --load` + capture |
| `check_e2e_pass` | zero FAIL, no crash, E2E-PASS |
| `check_golden` / `check_pass_labels` | golden PASS labels |
| `./build.py test e2e` | full suite + goldens |

Goldens: `tests/fixtures/e2e_golden/all.json` (single consolidated file;
suite entries are keyed by `commercial_readiness_<name>` stem).
Update with:

```bash
python3 tests/python/run_e2e.py --update-golden
```

Details: [`tests/e2e/README.md`](../tests/e2e/README.md).

## Fuzzing (#1935)

Unified entrypoint:

```bash
./build.py fuzz --list
./build.py fuzz --all --quick
./build.py fuzz --only core,corpus,hygiene_prop --continue-on-error
```

Drivers live under `tests/fuzz/drivers/`; corpus under `tests/fuzz/corpus/`.
See [`docs/fuzzing.md`](fuzzing.md).

## Benchmark SLO (#1569 / #1936)

```bash
./build.py bench --strict
./build.py bench --strict --tolerance 5 --statistical
python3 tests/bench/benchmark.py --update --rationale "why"
```

Relative + median-of-N comparison; see [`docs/benchmark.md`](benchmark.md).

Thin entrypoints at `tests/*.py` forward into `tests/python/` or
`tests/bench/` so path moves do not break scripts or docs.

## Migration & policy check

```bash
python3 tests/migrate_test_layout.py --dry-run
python3 tests/migrate_test_layout.py --apply    # idempotent leftover moves
python3 tests/migrate_test_layout.py --status   # inventory; exit 1 if unclean
python3 tests/python/test_layout_1939.py        # unit invariants
```

### Subtask map

| Issue | Role | Status |
|-------|------|--------|
| #1937 | Design + `migrate_test_layout.py` | Done (parent #1932) |
| #1938 | fuzz / bench moves | Done (parent #1932) |
| #1939 | Final cleanup, docs, root policy | This doc + README “What changed” |

## Contribution checklist

1. Prefer `tests/domain/` for new C++ coverage (see `tests/README.md`).
2. Put new Python drivers under `tests/python/`, `tests/bench/`,
   `tests/fuzz/`, or `tests/e2e/` — **not** the top-level
   `tests/` root.
3. Keep or add a thin entrypoint only when a public path must stay stable.
4. Run `python3 tests/migrate_test_layout.py --status` and `./build.py gate`
   before push.

## Related

- Parent issue: [#1932](https://github.com/cybrid-systems/aura/issues/1932)
- Final cleanup: [#1939](https://github.com/cybrid-systems/aura/issues/1939)
- Strategy matrix: `tests/STRATEGY.md` (#1887)
- C++ bulk migration inventory: `tests/legacy_test_inventory.md` (#1957)
