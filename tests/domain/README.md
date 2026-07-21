# tests/domain/

**Preferred** home for new C++ regression / AC tests.

Parent policy: [`../README.md`](../README.md).  
Migration inventory: [`../legacy_test_inventory.md`](../legacy_test_inventory.md) (#1957).

## What belongs here

| Belongs in `domain/` | Does not |
|----------------------|----------|
| Theme invariants (arena, mutate, fiber, hygiene, …) | Full language E2E → `suite/` |
| `query:*-stats` schema/bump gates | Golden Aura programs → `regression/` |
| Multi-issue AC bundles for one subsystem | New `issues/test_issue_N.cpp` |
| Shared case tables under `cases/` | One-off JIT stress (maybe root + `EXCLUDE_FROM_ALL`) |

## Current suites

| File | Theme | Extend when… |
|------|--------|----------------|
| `test_obs_schema_matrix.cpp` | Obs + production schemas | Standard total/hits/savings, field-list, or production flag surfaces |
| `cases/obs_schema_cases.hpp` | Case table for obs matrix | **Default** for new stats surfaces |
| `cases/production_sweep_cases.hpp` | Production field-list cases | Folded production flag surfaces (run by obs matrix) |
| `test_domain_gates_batch.cpp` | Fiber / hygiene / typed-mutate gates | Behavioral bump+readback gates (was 3 theme binaries) |
| **`arena/`** | Arena / compaction pilot (#1959) | See [`arena/README.md`](arena/README.md) — reference theme dir |

Harness for all of the above: `#include "test_harness.hpp"` only (#1960).

## How to add an AC (shortest path)

### 1. New stats surface only

1. Add a row to `cases/obs_schema_cases.hpp` (or `production_sweep_cases.hpp`
   for production field-list gates).
2. If the matrix needs a bump helper, wire `bump_slug` in
   `test_obs_schema_matrix.cpp`.
3. Rebuild: `ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix`.

### 2. Behavioral gate (fiber / hygiene / typed mutate)

1. Open `test_domain_gates_batch.cpp`.
2. Add a section under the matching `run_*` block:

   ```cpp
   std::println("\n=== Short title (#NNNN) ===");
   // setup → act → CHECK invariants
   ```

3. Keep helpers (`href`, `expect_schema`) local; do not fork a new binary.

### 3. Brand-new theme suite

1. Prefer extending `test_domain_gates_batch.cpp` or the arena family
   batches first.
2. Only if the theme is large enough to justify a binary: copy
   [`../templates/test_domain_pattern.cpp`](../templates/test_domain_pattern.cpp)
   → `test_domain_<theme>_<aspect>.cpp` and register in `CMakeLists.txt`:

   ```cmake
   aura_add_issue_test(test_domain_<theme>_<aspect>)
   aura_issue_test_link_llvm_jit(test_domain_<theme>_<aspect>)
   add_dependencies(all_test_issue_targets test_domain_<theme>_<aspect>)
   ```

3. Justify the new file in the commit message.

## Style

- Issue id = **label** in banners and CHECK messages, not the filename.
- Prefer **invariants** (“counters monotonic”, “schema stable”, “no stale ref
  after compact”) over “counter X was added in PR Y”.
- Share `CompilerService` setup; avoid re-copying 50-line mains.
- Use `test_harness.hpp` `CHECK` macros.
- For bundle compatibility, expose `int aura_issue_domain_<name>_run()` and a
  thin `main` that calls it (see existing suites).

## Do not

- Grow `tests/issues/test_issue_*.cpp`.
- Add `test_issue_*.cpp` under `domain/` either.
- Duplicate an obs schema test that already fits `obs_schema_cases.hpp`.

See also: [`../README.md`](../README.md) · [`../../docs/contributing.md`](../../docs/contributing.md).
