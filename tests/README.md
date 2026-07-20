# tests/

How and where to add tests in Aura.

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

## Layout

| Path | Role | When to use |
|------|------|-------------|
| **`domain/`** | **Preferred** theme suites | New ACs for compiler/runtime themes |
| `suite/` | Aura E2E (`.aura` scripts) | End-to-end language / stdlib behavior |
| `regression/` | Curated regression fixtures | Known-bad programs, redlines |
| `fixtures/` | Shared JSON / profiles / data | Bundle profiles, integ lists, golden inputs |
| `templates/` | Copy-paste starters (not built) | Scaffold a new domain suite |
| `issues/test_issue_*.cpp` | **Legacy** per-issue binaries | **Do not add new** — migrate via inventory |
| root `test_*_batch.cpp` | Intermediate family batches | Consolidating several related issue tests |
| root `test_*.cpp` | Older focused binaries | Only when no domain suite fits (justify) |

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
  fiber / steal / Guard  → domain/test_domain_fiber_orchestration.cpp
  hygiene / macro / dirty epoch → domain/test_domain_hygiene_dirty.cpp
  typed mutate / type-system → domain/test_domain_typed_mutate.cpp
  observability matrix     → domain/test_obs_schema_matrix.cpp
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
| `test_domain_<theme>.cpp` | `test_domain_fiber_orchestration.cpp` | Broad theme suite (default) |
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
// tests/domain/test_domain_fiber_orchestration.cpp
void run_suite(CompilerService& cs) {
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
| `test_domain_fiber_orchestration` | Fiber / steal / Guard / orch health |
| `test_domain_hygiene_dirty` | Macro hygiene / dirty-epoch markers |
| `test_domain_typed_mutate` | Typed mutate / type-system obs gates |
| `test_obs_schema_matrix` | Schema + bump matrix (`cases/obs_schema_cases.hpp`) |

Live policy detail: [`domain/README.md`](domain/README.md).  
Scaffold: [`templates/test_domain_pattern.cpp`](templates/test_domain_pattern.cpp)
(not a CMake target — copy, rename, register).

## Harness & metadata

- Prefer `#include "test_harness.hpp"` + `CHECK` / `TEST` / `RUN_ALL_TESTS`.
- Domain suites commonly expose `aura_issue_domain_*_run()` (bundle-friendly)
  and a small `main` that calls it.
- Optional headers for tooling:

  ```cpp
  // @category: integration
  // @reason: domain fiber/orch gates; extends test_domain_fiber_orchestration
  ```

- Optional `AURA_ISSUE_TEST(N, "…", { … })` in a domain file when a
  bundle entry-point symbol is required (see `test_harness.hpp`).

## Run

```bash
./build.py check                          # gate + build + tests
./build.py test unit                      # unit slice
./build.py test integ                     # integration
./build.py test issues                    # issue / domain targets
./build.py test issues-fast               # fixtures/issues_fast.json subset
ninja -C build test_domain_fiber_orchestration
./build/test_domain_fiber_orchestration
```

Static gate (no full build): `./build.py gate`.

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
