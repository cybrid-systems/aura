# Fuzzing infrastructure — Issue #1935

## Layout

```
tests/fuzz/
├── common.py           # shared paths + FuzzResult + CLI helpers
├── run_all.py          # orchestrator (--list / --all / --only)
├── corpus_tools.py     # corpus status / sync-repro
├── corpus/             # seed .sexpr files (versioned)
├── reproducers/        # crash dumps from drivers
├── drivers/            # individual fuzz_*.py + prop_hygiene_mutate.py
└── README.md
```

## Quick start

```bash
./build.py build
./build.py fuzz --list
./build.py fuzz --all --iters 100          # or reduced set
./build.py fuzz --only corpus,core --quick
./build.py fuzz --only hygiene_prop --iters 50 --seed 1

python3 tests/fuzz/corpus_tools.py status
python3 tests/fuzz/corpus_tools.py sync-repro
```

Environment:

| Variable | Meaning |
|----------|---------|
| `AURA_BIN` | Path to instrumented/plain `aura` binary |
| `FUZZ_TIMEOUT` | Per-case timeout seconds (default 5) |

## Common interface

Drivers live under `tests/fuzz/drivers/`. Prefer:

- `--quick` reduced workload
- `--seed N` reproducibility
- `--iters N` when supported (property fuzzer)
- Non-zero exit on **crashes** (soft eval errors may be OK)

The orchestrator (`run_all.py`) discovers registered names in `REGISTRY`
and aggregates exit codes + timing.

## Nightly CI

`.github/workflows/nightly.yml` job `fuzz-extended` runs:

```bash
python3 build.py fuzz --only core,corpus,hygiene_prop --quick --continue-on-error
```

and uploads `tests/fuzz/reproducers/` when present.

## Adding a fuzzer

1. Add `tests/fuzz/drivers/fuzz_my_feature.py` (import `common`)
2. Register in `tests/fuzz/run_all.py` `REGISTRY`
3. Run `./build.py fuzz --only my_feature --quick`

## Property-based test

`drivers/prop_hygiene_mutate.py` generates random pure arithmetic programs
and asserts **no process crash**. Extend with stronger semantic oracles as
the multi-`set-code` path stabilizes.

## Related

- Coverage: [`docs/testing.md`](testing.md) (#1933)
- Layout: [`docs/test_harness_pattern.md`](test_harness_pattern.md)
