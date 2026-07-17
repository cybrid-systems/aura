# Test harness pattern & path map (Issue #1570)

> **Audience**: new contributors writing `test_issue_*` / domain tests or running CI gates.  
> **Truth**: code + CMake + `build.py` > this doc.  
> **Related**: [design/testing-framework-v1.md](design/testing-framework-v1.md) · [contributing.md](contributing.md) · [ci-performance.md](ci-performance.md)

---

## 0. Path map — what does **not** exist

Review notes sometimes cite paths that were never landed (or were renamed). Use this table:

| Mythical / 404 path | Actual location | Role |
|---------------------|-----------------|------|
| `.github/ci_pipeline.yml` | **`.github/workflows/ci.yml`** (+ `nightly.yml`, `release.yml`) | GitHub Actions CI |
| `src/test/benchmark_gate.ixx` | **`tests/benchmark.py`** + **`build.py` `bench` / `test_bench`** | Benchmark runner + SLO gate (#1569) |
| `src/test/test_issue_pattern.ixx` | **`tests/test_harness.hpp`** + **`docs/test_harness_pattern.md`** (this file) + template below | Issue-test pattern |
| `src/test/*` | **`tests/`** (C++), **`tests/domain/`** (preferred suites), **`cmake/AuraDomainTests.cmake`** | All unit/issue tests |

There is **no** `src/test/` tree and **no** C++20 module named `benchmark_gate` / `test_issue_pattern`. Do not invent them for new work.

---

## 1. CI layout (actual)

```
.github/workflows/
  ci.yml        # PR + main: gate → build-test (+ bench SLO) · s0-smoke · sanitizers · security
  nightly.yml   # heavier / scheduled
  release.yml   # release packaging
```

### Jobs in `ci.yml` (short)

| Job | Command | Purpose |
|-----|---------|---------|
| `gate` | `python3 build.py gate` | docs + ruff + clang-format + fixtures + primitive surface + test-binding |
| `build-test` | `python3 build.py ci` then `python3 build.py bench --strict` | full build + CI_CORE/SAFETY/FUZZ/ISSUES + **benchmark SLO** (#1569) |
| `s0-smoke` | `AURA_PRIMITIVES=s0` suite | slim public surface |
| `asan-*` / `ubsan-*` | sanitizer builds | memory / UB (main-heavy) |

Env flags you will see:

| Env | Meaning |
|-----|---------|
| `AURA_CI_STRICT_BENCH=1` | Hard-fail benchmark regressions (#1569) |
| `AURA_ISSUES_TIER=fast\|full` | PR subset vs main full issue tier |
| `AURA_RUNTIME_DIR` | Where `lib/runtime.c` is found for AOT |
| `AURA_USE_MOLD` / `AURA_LINK_JOBS` | Fast link + link pool (#873/#874) |

Local parity:

```bash
./build.py gate              # same static gate as CI `gate` job
./build.py ci                # build + CI suite matrix (no separate bench unless you run it)
./build.py bench --strict    # same SLO gate as CI build-test final step
```

---

## 2. Benchmark “gate” (not a C++ module)

| Piece | Path |
|-------|------|
| Runner | `tests/benchmark.py` |
| Cases fixture | `tests/fixtures/benchmark_tests.json` (via `benchmark_cases.py`) |
| Baseline | `tests/benchmark_baseline.json` |
| Build entry | `./build.py bench [--strict]` · `./build.py test bench` |
| Unit tests (no `aura` binary) | `python3 tests/test_benchmark_gate.py` |
| Docs | [contributing.md § Benchmark SLO](contributing.md) · [benchmark.md](benchmark.md) |

```bash
./build.py build
./build.py bench --strict          # hard fail on SLO violation
python3 tests/test_benchmark_gate.py
# Accept a new performance surface intentionally:
python3 tests/benchmark.py --update   # commit baseline + PR rationale
```

---

## 3. Issue-test pattern (`test_issue_*` / domain)

### 3.1 Registration

1. Source: `tests/<name>.cpp` **or** preferred `tests/domain/<name>.cpp`
2. CMake: `aura_add_issue_test(<name>)` in `CMakeLists.txt` or `cmake/AuraDomainTests.cmake`
3. If the test needs FlatHashTable / JIT symbols: `aura_issue_test_link_llvm_jit(<name>)`
4. Optional: `add_dependencies(all_test_issue_targets <name>)`
5. Headers: `// @category: unit|integration` and `// @reason: Issue #N — …` for `scripts/gen_test_registry.py`（#1572：pre-commit / `./build.py test-registry --fix` 自动刷 `docs/generated/test-registry.json`）

Helper definition: `CMakeLists.txt` → `function(aura_add_issue_test NAME)`  
Domain batch list: `cmake/AuraDomainTests.cmake`

### 3.2 Harness API (`tests/test_harness.hpp`)

```cpp
#include "test_harness.hpp"
// CHECK(cond, "msg");   // increments g_passed / g_failed
// return g_failed == 0 ? 0 : 1;
```

Globals: `aura::test::g_passed`, `aura::test::g_failed`.

### 3.3 Recommended AC structure (every new issue test)

| Section | What to assert |
|---------|----------------|
| **AC query / schema** | `(engine:metrics "query:…-stats")` hash shape, `schema` = issue number |
| **AC functional** | Happy path + denial / error path |
| **AC metrics** | Counters bump (process-wide or `CompilerMetrics`) |
| **Stress** | Multi-thread or 100–1000 iter loop where relevant |
| **Sibling** | Does not break an adjacent primitive (optional) |

Copy-paste skeleton: [`tests/templates/test_issue_pattern.cpp`](../tests/templates/test_issue_pattern.cpp)

### 3.4 Run

```bash
# Single binary (after cmake build)
cmake --build build --target test_capability_effects_enforcement -j
./build/test_capability_effects_enforcement

# Unified issue runner
python3 tests/run_issue_tests.py --list
python3 tests/run_issue_tests.py --filter 1565
python3 tests/run_issue_tests.py --tier fast --changed
./build.py test issues          # full / tier from env
./build.py test issues-fast     # PR-style subset
ctest -L issue -R 1565          # CTest label filter
```

### 3.5 Binding gate (#1453)

Production prim sources (`src/compiler/evaluator_primitives*.cpp`, `evaluator.ixx`, …) **must** ship with a paired `tests/` change. See `scripts/check_test_binding.py` and `docs/design/testing-framework-v1.md`.

---

## 4. New-developer checklist (gate + bench)

```bash
# 0. once
./scripts/install-githooks.sh

# 1. build
./build.py build

# 2. static gate (same as CI job `gate`)
./build.py gate

# 3. optional unit slice
./build.py test unit

# 4. benchmark SLO (same as CI build-test final step)
./build.py bench --strict

# 5. issue-test pattern smoke (pick any green binary)
./build/test_issue_1543   # or another target you just built
```

If `gate` fails on docs: `./build.py docs` and commit `docs/generated/*`.  
If `bench --strict` fails after intentional work: update baseline with rationale (see §2).

---

## 5. Related files (quick index)

| Concern | Files |
|---------|--------|
| CI workflow | `.github/workflows/ci.yml` |
| Build / test matrix | `build.py` (`cmd_gate`, `cmd_ci`, `cmd_bench`, `SUITES`) |
| Issue runner | `tests/run_issue_tests.py`, `tests/issue_tier.py` |
| C++ harness | `tests/test_harness.hpp` |
| Domain tests | `cmake/AuraDomainTests.cmake`, `tests/domain/` |
| Benchmark | `tests/benchmark.py`, `tests/test_benchmark_gate.py` |
| Registry | `scripts/gen_test_registry.py` → `docs/generated/test-registry.json` |

---

## 6. Non-goals

- Do not reintroduce `src/test/*.ixx` modules for gates.
- Do not add a second CI entrypoint YAML that duplicates `workflows/ci.yml`.
- Domain suites (`tests/domain/`) remain preferred over unbounded `test_issue_N.cpp` growth when ACs fit a matrix.
