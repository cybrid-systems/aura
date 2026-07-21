# tests/

How and where to add tests in Aura.

## High-level strategy (#1887)

Hot-path coverage matrix, AI self-mod SLO, and strategy profiles:

- **[`STRATEGY.md`](STRATEGY.md)** — matrix + production-readiness goals
- Code: `src/test/test_strategy.h` / `import aura.test.strategy`
- Metrics: `(engine:metrics "query:test-strategy-stats")` (schema 1887)

New hot-path or self-mod tests should stamp `note_strategy_scenario(...)`
(or extend a domain suite that does) so coverage-bp stays honest.

## Philosophy

**Theme-based domain tests beat per-issue files.**

| Prefer | Avoid |
|--------|--------|
| Extend an existing `tests/domain/` suite | New `tests/issues/test_issue_N.cpp` |
| Table-driven cases (`cases/*.hpp`) | One binary per stats schema |
| Issue id as a **label** in comments / CHECKs | Issue id as the **filename** |
| Invariants of a subsystem (arena, mutate, fiber…) | Copy-paste closed-loop per PR |

Issue numbers still matter for tracking (`Close #N`, AC comments, commit trailers).
They are **not** a reason to add a new top-level test binary.

Legacy inventory & migration roadmap: **[#1957](https://github.com/cybrid-systems/aura/issues/1957)** —
[`legacy_test_inventory.md`](legacy_test_inventory.md)
(`python3 scripts/inventory_legacy_tests.py`).

**Root `test_*.cpp` classification** (theme → domain dest, near-dups, streamline waves):
[`root_test_classification.md`](root_test_classification.md)
(`python3 scripts/classify_root_tests.py`).

## Layout

| Path | Role | When to use |
|------|------|-------------|
| **`domain/`** | **Preferred** theme suites | New ACs; theme pilots under `domain/<theme>/` (see `domain/arena/` #1959) |
| `suite/` | Aura E2E (`.aura` scripts) | End-to-end language / stdlib behavior |
| `regression/` | Curated regression fixtures | Known-bad programs, redlines |
| `fixtures/` | Shared case data (sharded JSON) + profiles | See [`fixtures/README.md`](fixtures/README.md) (#1962) |
| `templates/` | Copy-paste starters (not built) | Scaffold a new domain suite |
| **`python/`** | Harness + runners + gate unit tests (#1932) | New Python drivers; see `python/README.md` |
| **`e2e/`** | Strengthened .aura E2E + commercial_readiness (#1934) | Machine-checkable PASS/FAIL + goldens |
| **`bench/`** | Benchmarks (C++ + Python + baseline) | SLO / microbench drivers |
| **`fuzz/`** | Unified fuzz orchestrator + corpus (#1935) | `./build.py fuzz --list` / nightly |
| **`memory/`** | Leak / soak scripts | Multi-hour memory campaigns |
| `issues/test_issue_*.cpp` | **Legacy** per-issue binaries | **Do not add new** — migrate via inventory |
| `observability/` | Bulk root obs cpp (#1977) | 180 files; prefer `domain/test_obs_schema_matrix` |
| `mutation/` | Bulk root mutation cpp (#1977) | 118 files; prefer `domain/test_domain_gates_batch` |
| `compiler_core/` | Bulk root compiler cpp (#1977) | 58 files; future `domain/compiler/` |
| `fiber/` | Bulk root fiber cpp (#1977) | 38 files; prefer `domain/test_domain_gates_batch` |
| `edsl/` | EDSL hygiene batches (#1977→batch) | `test_edsl_*_hygiene_batch` + domain gates for schema-only |
| `jit/` | Bulk root JIT/AOT cpp (#1977) | 7 files; heavy JIT stays here or root |
| `arena/` | Bulk root arena cpp (#1977) | 7 files; parallel to `domain/arena/` pilot |
| `stdlib/` | Bulk root stdlib cpp (#1977) | 5 files; prefer `suite/` for `.aura` |
| `linear/` | Bulk root linear-ownership cpp (#1977) | 2 files; prefer `test_linear_ownership_batch` |
| `shape/` | Bulk root shape/SoA cpp (#1977) | 1 file; prefer `test_soa_batch` |
| `misc/` | Uncategorized root cpp (#1977) | 2 files; manual triage |
| root `test_*_batch.cpp` | Intermediate family batches | Consolidating several related issue tests |
| root `test_*.cpp` | Older focused binaries | Only when no theme dir fits (justify) — empty since #1977 |
| root `*.py` thin entry | Stable CLI shims → `python/` / `bench/` | Do not put new drivers at root |

Layout authority + migration: [`docs/test_harness_pattern.md`](../docs/test_harness_pattern.md) (#1932 / #1939).
Idempotent mover / policy check:

```bash
python3 tests/migrate_test_layout.py --dry-run
python3 tests/migrate_test_layout.py --status   # exit 1 if root policy unclean
```

### What changed (#1932 / #1937 / #1938 / #1939 / #1977)

| Before | After |
|--------|--------|
| Python drivers scattered at `tests/*.py` | Full drivers under **`tests/python/`** |
| Bench scripts + baseline at root | **`tests/bench/`** (+ thin `tests/benchmark.py`) |
| Fuzz / leak scripts mixed in | **`tests/fuzz/`**, **`tests/memory/`** |
| Commercial .aura E2E loose | **`tests/e2e/`** (#1934) |
| 437 root `test_*.cpp` scattered flat | Theme-organized into **`tests/<theme>/`** (#1977) — parallel to `domain/` |
| Misplaced `incremental_mutation_test.aura` at root | Moved to `suite/` (#1977) |
| Stray `__pycache__/` at root | Deleted (gitignored; pure local clutter) |

**Stable CLI:** keep using `python3 tests/run.py …`, `tests/run_issue_tests.py`,
`tests/fixture_check.py`, `tests/benchmark.py` — these are **thin entrypoints**
that forward into `python/` or `bench/`. Do **not** put new full drivers at the
`tests/` root.

**Still at root by design:** `test_*.cpp` (legacy + batch; migrate via #1957
domain waves), `test_harness.hpp`, `test-binding-allowlist.txt`, strategy docs.

**Non-C++ root policy:** thin entrypoints + harness headers + allowlist +
README/STRATEGY + `migrate_test_layout.py` only. Enforced by
`python3 tests/migrate_test_layout.py --status` and
`tests/python/test_layout_1939.py`.

CMake resolves sources in order (`aura_resolve_test_cpp`):

1. `tests/issues/<name>.cpp` (legacy)
2. **`tests/domain/<name>.cpp` (preferred for new suites)**
3. `tests/<name>.cpp` (fallback)

Register new domain targets in [`cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake).

## Decision tree (new test)

```
Is this a new query:*-stats / engine:metrics schema gate?
  └─ YES → add a row to domain/cases/obs_schema_cases.hpp
           (and bump mapping in test_obs_schema_matrix.cpp if needed)
           STOP — do not create a binary

Does an existing domain suite cover this theme?
  fiber / hygiene / typed-mutate gates → domain/test_domain_gates_batch.cpp
  observability + production schemas   → domain/test_obs_schema_matrix.cpp
  └─ YES → add a section / case there. STOP

Is this a multi-AC family already batched (compact, soa, linear, …)?
  └─ YES → extend the matching tests/test_*_batch.cpp (or promote into domain/)

Is this pure language E2E (no C++ harness)?
  └─ YES → suite/ or regression/ + fixtures JSON

Still need a new C++ binary?
  └─ Create tests/domain/test_<theme>_<aspect>.cpp from templates/
     Register in cmake/AuraDomainTests.cmake
     Commit message: why a new suite (not an extension) is required

NEVER: tests/issues/test_issue_<N>.cpp for new work
```

## Naming in `domain/`

| Pattern | Example | Use for |
|---------|---------|---------|
| `test_domain_<theme>.cpp` | `test_domain_gates_batch.cpp` | Broad theme suite (default) |
| `test_domain_<theme>_<aspect>.cpp` | `test_domain_arena_compaction.cpp` | Sub-theme split when a file grows large |
| `test_<theme>_<aspect>.cpp` | `test_obs_schema_matrix.cpp` | Established suite name (ok if already wired) |
| `cases/<theme>_cases.hpp` | `cases/obs_schema_cases.hpp` | Shared tables / data-driven cases |

Rules:

- **Theme words** match inventory buckets when possible:
  `arena`, `mutation`, `fiber`, `linear`, `hygiene`, `jit`, `soa`, `obs`.
- Prefer **one suite per theme**, not one file per GitHub issue.
- Issue numbers appear in section banners and CHECK messages, e.g.
  `=== Steal + arena/GC (#812) ===`, not in the path.
- Do **not** name new files `test_issue_*`.

## Good vs bad

### Bad — per-issue binary (legacy)

```cpp
// tests/issues/test_issue_812.cpp  — DON'T ADD MORE LIKE THIS
// One executable, one issue, setup duplicated 600+ times.
int main() {
    CompilerService cs;
    auto h = cs.eval("(query:orchestration-steal-arena-gc-stats)");
    CHECK(/* schema == 812 */);
    return g_failed ? 1 : 0;
}
```

Problems: poor discoverability, huge link/compile surface, hard refactors,
inconsistent harness style.

### Good — domain suite section (preferred)

```cpp
// tests/domain/test_domain_gates_batch.cpp
void run_fiber_orchestration(CompilerService& cs) {
    std::println("\n=== Steal + arena/GC coordination (#812) ===");
    expect_hash_schema(cs, "query:orchestration-steal-arena-gc-stats", 812);
    cs.evaluator().bump_steal_arena_yield_during_compact();
    CHECK(href(cs, "query:orchestration-steal-arena-gc-stats",
               "yield-during-compact") >= 1,
          "yield-during-compact");
    // … more fiber/orch ACs for related issues in the same binary …
}
```

### Good — table-driven observability

```cpp
// domain/cases/obs_schema_cases.hpp — add a row, not a file
{812, "query:orchestration-steal-arena-gc-stats", "steal_arena_yield"},
```

### Good — broader invariant (what domain tests optimize for)

Test the **contract** (“steal yields during compact; schema stable; counters
monotonic”), not only “PR #812 landed a counter”. Related issues share one
suite so refactors keep one place green.

## When a focused regression is still OK

Add a **narrow** non-domain binary only if most of these hold:

1. **No theme fit** — none of the domain suites or case tables can host it
   without a forced mismatch.
2. **Heavy / special link** — e.g. long multi-worker stress, sanitizer-only,
   or JIT profile that would bloat the default domain binary
   (prefer `EXCLUDE_FROM_ALL` + document the ninja target).
3. **True regression fixture** — a specific historical failure that is
   cheaper as `regression/*.aura` or a tiny C++ repro than as a domain AC.
4. **Explicit justification** in the commit message
   (`why not domain/test_domain_…`).

Even then:

- Prefer `tests/test_<theme>_<aspect>.cpp` or `tests/domain/…`, **not**
  `tests/issues/test_issue_N.cpp`.
- Keep it short; if a second related AC appears, fold both into a domain
  suite or `*_batch.cpp` immediately.

## Existing domain suites (start here)

| Target | Theme |
|--------|--------|
| `test_domain_gates_batch` | Fiber / hygiene / typed-mutate behavioral gates |
| `test_obs_schema_matrix` | Schema + bump + production field-list (`cases/*.hpp`) |

Live policy detail: [`domain/README.md`](domain/README.md).  
Scaffold: [`templates/test_domain_pattern.cpp`](templates/test_domain_pattern.cpp)
(not a CMake target — copy, rename, register).

## Harness & metadata (#1960)

**Single recommended header:** `#include "test_harness.hpp"`.

| API | Use |
|-----|-----|
| `CHECK` / `EXPECT_*` | Assertions (ASan-safe owned message string) |
| `TEST` / `RUN_ALL_TESTS` | Registered cases + summary |
| `run_pilot_tests()` | Pilot-style counter summary |
| `aura_call_expr()` | `engine:metrics` / `stats:get` routing for demoted names |
| `k_int_env()` | Shared stress/fuzz env knobs |
| `AURA_ISSUE_TEST` | Bundle entry-point helper |
| `capture_stable_refs` / `validate_stable_refs` | FlatAST white-box helpers |

`issue_test_harness.hpp` is a **deprecated shim** that includes `test_harness.hpp`
— do not use it in new code.

Domain suites commonly expose `aura_issue_domain_*_run()` (bundle-friendly)
and a small `main` that calls it.

Optional headers for tooling:

```cpp
// @category: integration
// @reason: domain fiber/orch gates; extends test_domain_gates_batch
```

## Running tests (#1961)

**Preferred entry points:**

| How | What |
|-----|------|
| `./build.py check` | gate + build + default tests |
| `./build.py test <suite>` | suite dispatch (unit, integ, issues, …) |
| **`python3 tests/run.py <cmd>`** | unified Python runner (issues, fixtures, gradual, bench, mutation, bash) |
| `ninja -C build <target> && ./build/<target>` | single C++ binary |

```bash
# Categories
python3 tests/run.py list
python3 tests/run.py issues --tier fast
python3 tests/run.py issues-fast
python3 tests/run.py fixtures
python3 tests/run.py gradual
python3 tests/run.py bench -- --strict
python3 tests/run.py mutation
python3 tests/run.py bash

# Machine-readable trailer (CI-friendly)
python3 tests/run.py --json fixtures
python3 tests/run.py issues --tier fast -- --json

# Via build.py (CI / local)
./build.py test unit
./build.py test integ
./build.py test issues
./build.py test issues-fast
./build.py gate                           # static only

# Domain suites
ninja -C build test_domain_gates_batch test_obs_schema_matrix
./build/test_domain_gates_batch
ninja -C build test_arena_batch           # domain/arena (#1959)
```

### Legacy scripts (still work; prefer `tests/run.py`)

Thin entrypoints at `tests/*.py` forward into `tests/python/` or `tests/bench/`
(#1932). Prefer the unified CLI:

| Script (entrypoint) | Prefer / implementation |
|---------------------|-------------------------|
| `tests/run_issue_tests.py` | `tests/run.py issues` → `python/run_issue_tests.py` |
| `tests/fixture_check.py` | `tests/run.py fixtures` → `python/fixture_check.py` |
| `tests/check_gradual.py` | `tests/run.py gradual` → `python/check_gradual.py` |
| `tests/benchmark.py` | `tests/run.py bench` → `bench/benchmark.py` |
| `tests/python/mutation_loop.py` | `tests/run.py mutation` |
| `tests/run-tests.sh` | `tests/run.py bash` → `python/run-tests.sh` |

Shared colors/paths/report helpers: `tests/python/_aura_harness.py`.

### Fixtures (#1962)

Case matrices are **sharded** under `tests/fixtures/{regression,integ,benchmark,smoke}/`
(not monolithic `*_tests.json`). Validate with `python3 tests/run.py fixtures` or
`python3 scripts/fixtures_tool.py status`. Details: [`fixtures/README.md`](fixtures/README.md).

## Related

| Doc / script | Purpose |
|--------------|---------|
| [`legacy_test_inventory.md`](legacy_test_inventory.md) | #1957 inventory + migration waves |
| [`domain_classification.md`](domain_classification.md) | Older 5-bucket Phase-2 map (historical) |
| [`domain/README.md`](domain/README.md) | Domain-directory rules |
| [`templates/test_domain_pattern.cpp`](templates/test_domain_pattern.cpp) | New domain suite template |
| [`templates/test_issue_pattern.cpp`](templates/test_issue_pattern.cpp) | Legacy pattern (redirects to domain/) |
| [`../docs/contributing.md`](../docs/contributing.md) | Repo entry → testing + workflow |
| [`../cmake/AuraDomainTests.cmake`](../cmake/AuraDomainTests.cmake) | Domain + batch target wiring |
