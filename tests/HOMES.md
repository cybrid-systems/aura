# Test homes — where new ACs go

**Agents: read this before creating any new `tests/**/*.cpp`.**

Issue numbers belong in comments (`// Issue #NNNN`) and coverage manifests —
**not** in filenames. Do not create `test_foo_2622.cpp` or `test_issue_2622.cpp`.

## Decision tree (hard)

```
1. Schema-only gate (query:*-stats / engine:* key)?
   → add a row in tests/compiler/obs_schema_cases.hpp
     (+ production_sweep_cases.hpp if production flag)
   → STOP. No new .cpp.

2. Same feature family already has a suite under tests/<src-module>/?
   → open that file, add AC functions + call them from main / run_all.
   → STOP.

3. Family has a *_batch / matrix suite (see map below)?
   → extend the batch. STOP.

4. Truly new feature surface with no home?
   → create tests/<src-module>/test_<module>_<feature>.cpp
     (module + feature words only — NO trailing issue number)
   → register once in CMakeLists.txt:
        aura_add_issue_test(test_<module>_<feature>)
        aura_issue_test_link_light(...)   # default
        add_dependencies(all_test_issue_targets ...)
   → Issue #N in the file banner only.

5. NEVER:
   - tests/issues/test_issue_N.cpp
   - tests/**/test_*_<3–5 digit issue>.cpp   (blocked by pre-commit)
   - tests/test_*.cpp at repo root for new work
```

## Preferred homes (theme → file)

| Theme / keywords | Prefer this home | Dir |
|------------------|------------------|-----|
| Arena / compact / Moving densify / pin hooks | `test_arena_batch.cpp`, `test_gc_compact_batch.cpp`, pin suites | `core/` |
| FlatAST locks / atomics / SoA columns | `test_flatast_atomic_lock_batch.cpp`, `test_soa_batch.cpp` | `core/` / `compiler/` |
| Mutation boundary / guard / hold | `test_mutation_boundary_batch.cpp`, `test_mutation_guard_unit_batch.cpp` | `compiler/` |
| Occurrence / dirty-key / cone / TypeVar refined | `test_mutation_occurrence_dirty_batch.cpp` or existing occurrence suite | `compiler/` |
| Linear ownership / cross-closure escape | **`test_linear_cross_closure.cpp`**, `test_linear_ownership_batch.cpp` | `compiler/` |
| Dead coercion / CastOp density | `test_dead_coercion_batch.cpp`, castop suites | `compiler/` |
| AOT / SpecJIT / relower / stamp | `test_jit_aot_hot_update_unit_batch.cpp`, `test_incremental_relower_batch.cpp` | `compiler/` |
| Observability schema / metrics keys | `obs_schema_cases.hpp` + `test_obs_schema_matrix.cpp` | `compiler/` |
| Capability / sandbox / grant / Restricted | `test_capability_sandbox_batch.cpp`, security suites | `core/` / `compiler/` |
| Fiber / steal / reclaim / safepoint | fiber resume batch / serve suites | `serve/` / `compiler/` |
| Mailbox BP / hold / drain | existing mailbox suites | `serve/` / `orch/` |
| Orch / agent / parallel-intend | orch suites under `tests/orch/` | `orch/` |
| Hot-path matrix / multi-axis chaos | `test_hotpath_matrix_batch.cpp` | `core/` |

If unsure: `rg -n 'keyword' tests/compiler tests/core tests/serve tests/orch` and **extend the closest thematic file**.

## Consolidation waves (existing issue-suffixed files)

~400 historical `test_*_NNNN.cpp` remain. Migrate by **family**, not one-off renames:

| Wave | Family | Target home |
|------|--------|-------------|
| W0 (done) | linear cross-closure #2563/#2612/#2623 | `compiler/test_linear_cross_closure.cpp` |
| W1 | FlatAST / column locks atomics | `core/test_flatast_atomic_lock_batch.cpp` |
| W2 | security / capability / grant / Restricted | capability + security batches |
| W3 | densify / pin / Moving / envframe | arena + pin suites |
| W4 | AOT / SpecJIT / stamp / relower | jit/aot batches |
| W5 | mailbox / fiber / residual / chaos | serve + hotpath |
| W6 | occurrence / cone / coercion | occurrence + dead_coercion batches |
| W7 | leftover / uncategorized | case-by-case into nearest theme |

After each fold: drop old CMake targets, point coverage manifests at the thematic path, regen `test-registry.json` + inventory.

## Enforcement

- **pre-commit (hard):** rejects newly staged `test_issue_*.cpp` and `test_*_<NNNN>.cpp`
- **Scaffold:** `tests/scaffolds/module_test_scaffold.cpp` — no issue suffix in name
- **Policy authority:** this file + `tests/README.md`
