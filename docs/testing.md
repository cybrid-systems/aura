# Testing & coverage

## Quick start

```bash
./build.py gate          # static checks
./build.py check         # gate + build + CI test matrix
./build.py test smoke    # single suite
./build.py test e2e      # commercial_readiness golden E2E (#1934)
python3 tests/run.py list
python3 tests/run.py e2e
```

Layout / contribution policy: [`tests/README.md`](../tests/README.md),
[`test_harness_pattern.md`](test_harness_pattern.md).

## LLVM source-based coverage (Issue #1933)

Aura supports **Clang source-based coverage** (`-fprofile-instr-generate`
`-fcoverage-mapping`) integrated with `llvm-profdata` / `llvm-cov`.

### Local usage

```bash
# Full pipeline: configure instrumented build → run smoke → HTML+JSON
./build.py coverage --html

# Tools / preset only (no long rebuild)
./build.py coverage --check-tools

# Report from an existing instrumented run
./build.py coverage --html --skip-build

# Soft floor (default 0). Raise as coverage improves:
./build.py coverage --html --min-line-pct 10
```

Artifacts land under:

```
build_coverage/coverage/
├── html/index.html     # browsable report
├── coverage.json       # llvm-cov export
├── summary.txt         # llvm-cov report text
└── summary.json        # compact schema-1933 totals
```

### CMake

```bash
cmake --preset coverage
cmake --build --preset coverage --target aura -j$(nproc)
```

`cmake/AuraCoverage.cmake` is included from the root `CMakeLists.txt` and
activates when `-DAURA_ENABLE_COVERAGE=ON` (the `coverage` preset sets this
and forces `clang`/`clang++`).

### Exclude paths

Reports ignore `tests/`, `third_party/`, and build trees via
`-ignore-filename-regex` (see `scripts/llvm_cov_report.py`).

### CI

Nightly job `coverage` runs `./build.py coverage --html` and uploads
`build_coverage/coverage` as an artifact. PR gate stays fast and only
verifies tooling via unit tests (`tests/python/test_llvm_cov_1933.py`).

### Thresholds

Initial AC is **pipeline + report**. Hard floors are optional:

| Scope | Flag | Default |
|-------|------|---------|
| Overall lines | `--min-line-pct N` | 0 (soft) |
| Module substr | `--require-module evaluator:0` | 0 |

Raise floors (e.g. `evaluator:75`) once the instrumented matrix regularly
clears them on `main`.

### Python harness

Gate unit tests under `tests/python/` are plain unittest (no coverage.py
required for AC). Optional: `python3 -m coverage run -m unittest …` for
local Python harness inspection.
