# tests/

How and where to add tests in Aura.

**Strategy & hot-path coverage:** [`STRATEGY.md`](STRATEGY.md) (#1887).
**Live layout detail:** [`domain/README.md`](domain/README.md) · [`fixtures/README.md`](fixtures/README.md) · [`legacy_test_inventory.md`](legacy_test_inventory.md) (#1957).

## Philosophy

Theme-based domain suites beat per-issue files.

| Prefer | Avoid |
|--------|-------|
| Extend `tests/domain/` (+ `cases/*.hpp` tables) | New `tests/issues/test_issue_N.cpp` |
| Issue id as **label** in banners / CHECKs | Issue id as **filename** |
| Subsystem invariants (arena, mutate, fiber…) | Copy-paste closed-loop per PR |

Issue numbers still matter for tracking (`Close #N`, AC comments, commit
trailers). They are **not** a reason to add a new top-level binary.

## Layout (terse)

| Where | Use for |
|-------|---------|
| **`domain/`** | Preferred C++ suites — see [`domain/README.md`](domain/README.md) |
| **`python/`** | Harness + runners + gate unit tests (#1932) |
| **`bench/`** | Benchmarks (C++ + Python + baseline) |
| **`e2e/`** | `.aura` E2E + commercial_readiness (#1934) |
| `fuzz/` | Unified fuzz orchestrator + corpus (#1935) |
| `suite/` | `.aura` E2E language / stdlib behavior |
| `regression/` | Curated regression fixtures + redlines |
| `fixtures/` | Shared case data (sharded JSON) — 12 KB / 50 cases per shard |
| `templates/` | Copy-paste starters (not built) |
| `issues/test_issue_*.cpp` | **Legacy** — do not add new (migrate via #1957) |
| `<theme>/` cpp dirs | Bulk theme-organize from root (#1977); extend the matching `domain/` suite instead |
| root `test_*.cpp` | Legacy + batch intermediates; justify or fold |
| root `*.py` | Thin entrypoints → `python/` / `bench/` — no new drivers |

Stable entry: `python3 tests/run.py …`, `tests/benchmark.py`,
`tests/fixture_check.py`, `tests/run_issue_tests.py`. CMake resolves sources in
order via `aura_resolve_test_cpp`: `issues/` → **`domain/`** → root.
Authoritative layout: [`docs/test_harness_pattern.md`](../docs/test_harness_pattern.md).
Policy check: `python3 tests/migrate_test_layout.py --status`.

## Decision flow (new test)

```
New query:*/engine:* schema gate?
  └─ YES → add a row to domain/cases/obs_schema_cases.hpp
           (wire bump helper if needed). STOP.

Existing domain suite cover this theme?
  fiber / hygiene / typed-mutate → test_domain_gates_batch
  obs + production schemas        → test_obs_schema_matrix
  arena / compaction (#1959)      → domain/arena/
  └─ YES → add a section / case there. STOP.

Multi-AC family already batched (compact, soa, linear, …)?
  └─ YES → extend tests/test_*_batch.cpp (or promote into domain/).

Pure language E2E?
  └─ YES → suite/ or regression/ + fixtures JSON.

Brand-new theme?
  └─ Copy templates/test_domain_pattern.cpp →
     domain/test_<theme>_<aspect>.cpp, register in CMakeLists.txt.
     Justify in commit message: why not an extension.
```

**Never** `tests/issues/test_issue_<N>.cpp` for new work.

## Harness

Single recommended header: `#include "test_harness.hpp"`.

`CHECK` / `EXPECT_*` · `TEST` / `RUN_ALL_TESTS` · `run_pilot_tests()` ·
`aura_call_expr()` (`engine:metrics` / `stats:get` demoted names) ·
`k_int_env()` (shared stress/fuzz knobs) · `AURA_ISSUE_TEST` (bundle entry) ·
`capture_stable_refs` / `validate_stable_refs` (FlatAST helpers).

`issue_test_harness.hpp` is a **deprecated shim** — do not use in new code.

## Running

```bash
python3 tests/run.py list                    # show categories
python3 tests/run.py issues --tier fast      # fast issue suites
python3 tests/run.py issues-fast             # CI quick
python3 tests/run.py fixtures                # validate fixture shards
python3 tests/run.py gradual                 # gradual typing gate
python3 tests/run.py bench                   # benchmarks
python3 tests/run.py mutation                # mutation loop
python3 tests/run.py bash                    # run-tests.sh wrapper

./build.py check                              # gate + build + default tests
./build.py test unit | integ | issues | issues-fast
./build.py gate                               # static only

ninja -C build test_domain_gates_batch test_obs_schema_matrix
./build/test_domain_gates_batch
ninja -C build test_arena_batch               # domain/arena (#1959)
```

JSON trailer for CI: `python3 tests/run.py --json <cmd>`.

## Focused regression is OK when

1. No theme fits without a forced mismatch.
2. Heavy / special link (sanitizer-only, multi-worker stress, JIT profile —
   mark `EXCLUDE_FROM_ALL` and document the ninja target).
3. True regression fixture (cheaper as `regression/*.aura` than a domain AC).
4. Commit message justifies **why not** `domain/test_<theme>_…`.

Even then, prefer `tests/test_<theme>_<aspect>.cpp`, **not**
`tests/issues/test_issue_N.cpp`.

## Related

| Doc | Purpose |
|-----|---------|
| [`STRATEGY.md`](STRATEGY.md) | Hot-path coverage matrix + SLO goals (#1887) |
| [`domain/README.md`](domain/README.md) | Domain-directory rules + suite list |
| [`legacy_test_inventory.md`](legacy_test_inventory.md) | #1957 inventory + migration waves |
| [`root_test_classification.md`](root_test_classification.md) | Theme → domain map + near-dups |
| [`fixtures/README.md`](fixtures/README.md) | Sharded fixture format + 12 KB / 50-case rule |
| [`../docs/test_harness_pattern.md`](../docs/test_harness_pattern.md) | CMake resolve order + harness policy (#1932 / #1939) |
| [`../docs/contributing.md`](../docs/contributing.md) | Repo entry → testing + workflow |
| [`../cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake) | Domain + batch target wiring |
