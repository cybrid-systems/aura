# Sanitizers (Issue #325)

Aura's test infrastructure ships with a 3-sanitizer
matrix (TSan + ASan + UBSan) for catching concurrency
races, use-after-frees, and undefined behavior in the
hot paths. The `tests/run_sanitizer_matrix.sh` script
is the canonical entry point.

## Quick start

```sh
# Run the full 3-sanitizer matrix on the 4 default targets.
tests/run_sanitizer_matrix.sh all

# Or just one sanitizer:
tests/run_sanitizer_matrix.sh thread      # TSan only
tests/run_sanitizer_matrix.sh address     # ASan only
tests/run_sanitizer_matrix.sh undefined   # UBSan only
tests/run_sanitizer_matrix.sh both        # TSan + ASan (legacy)

# Coverage (llvm-cov / gcov):
tests/run_sanitizer_matrix.sh coverage

# Custom targets:
tests/run_sanitizer_matrix.sh all "test_issue_180 test_issue_226"
```

## What each sanitizer catches

| Sanitizer | Flag | Catches |
|---|---|---|
| **TSan** (ThreadSanitizer) | `-fsanitize=thread` | Data races, lock-order inversions, use-after-free in concurrent code |
| **ASan** (AddressSanitizer) | `-fsanitize=address` | Heap use-after-free, stack buffer overflow, double-free, memory leaks |
| **UBSan** (UndefinedBehaviorSanitizer) | `-fsanitize=undefined` | Signed integer overflow, null pointer dereference, misaligned access, shift out of range |

## Test binaries exercised

The matrix runs the 4 representative test binaries
(one per major area):

| Binary | Area | Issue |
|---|---|---|
| `test_issue_180` | Closure-bridge lifetime | #180 |
| `test_issue_218` | Reflection/serialize | #218 |
| `test_issue_226` | Unified harness | #226 |
| `test_issue_321` | Multi-fiber stress | #321 |

All four were already TSan-tested in the prior session;
this script adds the ASan + UBSan coverage + the
combined "all" mode.

## Compiler flag matrix

```sh
TSAN_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1"
ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"
UBSAN_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -g -O1"
COV_FLAGS="-fprofile-arcs -ftest-coverage -fno-omit-frame-pointer -g -O0"
```

All sanitizers use `-fno-omit-frame-pointer` (required
for sane stack traces) and `-g` (debug symbols). TSan +
ASan use `-O1` (debug-friendly optimization). Coverage
uses `-O0` (the simplest, most accurate line coverage).

## Build directories

The script writes to separate build directories per
sanitizer so they can coexist:

- `build_tsan/` — TSan build
- `build_asan/` — ASan build
- `build_ubsan/` — UBSan build
- `build_cov/` — Coverage build

Each `build_*` directory is configured with
`cmake ..` on first use, then `ninja <target>` to
rebuild only what changed.

## CI integration

The script is designed to be the entry point for a CI
job. The recommended setup is:

```yaml
sanitizer_matrix:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v7
    - name: Run sanitizer matrix
      run: tests/run_sanitizer_matrix.sh all
    - name: Upload artifacts
      uses: actions/upload-artifact@v7
      with:
        name: sanitizer-logs
        path: |
          /tmp/cmake_*.log
          /tmp/ninja_*.log
          /tmp/run_*.log
```

A passing run exits 0. A failing run exits 1 with a
tail of the failing test's log.

## Known-failures handling

The script does NOT skip on pre-existing sanitizer
findings. Each new finding surfaces immediately. If
the finding is pre-existing (i.e. surfaced in a prior
session), the recommended path is:

1. Capture the finding's stderr in the issue tracker
2. File a follow-up issue to investigate + fix
3. Reference the issue in the test that surfaces it
   (so the test stays visible in CI)

The `build.py:SUITE_SKIP` map handles the
suite-level skip for tests that are still under
investigation; the sanitizer matrix is a different
mechanism (it surfaces findings, doesn't suppress
them).

## Hot files for coverage

The issue body names 3 hot files for coverage
reporting:

- `src/compiler/evaluator_impl.cpp` (or its successor
  `src/compiler/evaluator_primitives_compile.cpp`)
- `src/serve/fiber.cpp`
- `src/compiler/aura_jit_bridge.cpp`

The `coverage` mode runs `gcov` on these 3 files and
prints the per-file coverage percentage. The
percentage is approximate (the build is
`-O0 -fprofile-arcs`, which gives line coverage but
not branch coverage).

## Limitations

- Sanitizer builds are slower than regular builds
  (TSan 2-3x slowdown, ASan 2x, UBSan ~1.5x)
- TSan + ASan can be combined ("both" mode) but
  UBSan with TSan is unsupported by LLVM
- Coverage mode requires `gcov` in PATH (the script
  doesn't install it; CI should pre-install)
- The script does NOT pre-clean the build
  directories; stale .o files can cause spurious
  warnings on rebuild

## See also

- `tests/run_issue_180_tsan.sh` — original 180/226
  TSan/ASan script (legacy; this script
  supersedes)
- `tests/run_issue_218_tsan.sh` — original 218
  TSan/ASan script (legacy; this script
  supersedes)
- `tests/run_concurrent_tsan.sh` — multi-fiber
  TSan script (#321)
- `docs/incremental_dirty_propagation.md` — the
  subsystem that benefits most from the sanitizer
  matrix
