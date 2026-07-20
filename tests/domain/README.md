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
| `test_domain_fiber_orchestration.cpp` | Fiber / steal / Guard / orch | New orchestration observability or safety gates |
| `test_domain_hygiene_dirty.cpp` | Macro hygiene / dirty-epoch | Provenance, marker, hygiene query ACs |
| `test_domain_typed_mutate.cpp` | Typed mutate / type-system | Coercion, renarrow, ownership post-mutate |
| `test_obs_schema_matrix.cpp` | Obs schema matrix | Standard total/hits/savings or field-list schemas |
| `cases/obs_schema_cases.hpp` | Case table for obs matrix | **Default** for new stats surfaces |

## How to add an AC (shortest path)

### 1. New stats surface only

1. Add a row to `cases/obs_schema_cases.hpp`.
2. If the matrix needs a bump helper, wire `bump_slug` in
   `test_obs_schema_matrix.cpp`.
3. Rebuild: `ninja -C build test_obs_schema_matrix && ./build/test_obs_schema_matrix`.

### 2. Behavioral gate for an existing theme

1. Open the matching `test_domain_*.cpp`.
2. Add a section:

   ```cpp
   std::println("\n=== Short title (#NNNN) ===");
   // setup → act → CHECK invariants
   ```

3. Keep helpers (`href`, `expect_schema`) local or shared; do not fork a new binary.

### 3. Brand-new theme suite

1. Copy [`../templates/test_domain_pattern.cpp`](../templates/test_domain_pattern.cpp)
   → `test_domain_<theme>_<aspect>.cpp`.
2. Register in [`../../cmake/AuraDomainTests.cmake`](../../cmake/AuraDomainTests.cmake):

   ```cmake
   aura_add_issue_test(test_domain_<theme>_<aspect>)
   aura_issue_test_link_llvm_jit(test_domain_<theme>_<aspect>)  # if CS/JIT needed
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
