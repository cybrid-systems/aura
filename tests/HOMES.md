# Test homes — where new ACs go

**Agents: read this before creating any new `tests/**/*.cpp`.**

Issue numbers belong in comments (`// Issue #NNNN`) and coverage manifests —
**not** in filenames. Do not create `test_foo_2622.cpp` or `test_issue_2622.cpp`.

**Coverage (runtime vs manifest vs check_*.py):** [`COVERAGE.md`](COVERAGE.md).
Do not add a substring-only `check_*.py` per issue.

## Aura behavior suites (L1)

Append one named case under `tests/suite/`. The only API to teach is
`std/test`: `(test-file …)`, `(test-case name meta body…)`, `(test-finish)`.
`meta` is `(hash)` or `(hash "issue" N "tags" (list "…"))`. A failure prints
`file:name (#issue) FAILED` and the process exits non-zero after the other
cases. `std/edsl-test-harness` is a shim for old `it` / `summary` call sites.
Do not start a new per-file `*fails*` counter.

Ask before creating a file:

```
python3 tests/run.py home "<query>"
python3 tests/run.py cover --issue N
python3 tests/run.py cover --name <symbol>
```

`home` prints the one theme file, a `std/test` append snippet, and the rule
for a surface that has no home yet. A miss says it was not found and does
not guess a path. `cover` reads case metadata from the suites (file, case
name, issue). It does not compile the issue-farm and it is not a second
handwritten inventory.

## Aura L1 homes

Until a merge wave retargets a row, the home is the closest existing file.
Append there. Names `std/iter` re-exports are the `std/list` contract. Names
`std/algorithm` re-exports are the `std/sort` contract.

| Surface | Home now | Ask with |
|---|---|---|
| std/string | `tests/suite/string_join.aura` | `string-join`, `std/string` |
| std/list | `tests/suite/list_srfi_5133.aura` | `iota`, `std/list`, `std/iter` |
| std/vector | `tests/suite/vector_srfi_5133.aura` | `vector-ref`, `vector-map`, `std/vector` |
| std/sort | `tests/suite/sort_srfi_5133.aura` | `sorted?`, `list-sort`, `std/sort`, `std/algorithm` |
| std/json | `tests/suite/json_reg_split_5032.aura` | `json-parse`, `std/json` |
| std/extract and std/agent extractors | `tests/suite/export_contracts_5198.aura` | `extract-code`, `llm:extract-code` |
| lang/module | `tests/suite/module.aura` | `lang/module`, `overwrite warning` |
| lang/parser | `tests/suite/paren_semi_comment_5199.aura` | `unclosed list`, `(;` |
| runtime/fiber | `tests/suite/issue_5064_fiber_cancel_text.aura` | `fiber cancel` |
| runtime/orch | `tests/suite/orchestrator.aura` | `runtime/orch`, `orchestrator` |
| runtime/workspace | `tests/suite/workspace_source_ssot_2920.aura` | `runtime/workspace`, `workspace` |

These files may still use an older harness. New cases use `std/test`.
An unclosed program cannot sit inside `test-case`; put it in its own
`tests/suite/<module>_<feature>.aura` with `;; expect-err:`.

## Python process homes

These compare processes, faces, and grants. Behavior assertions do not go here.

| Surface | Home now | Ask with |
|---|---|---|
| CLI / help | `tests/python/test_issue_4935_help.py` | `--help` |
| Diagnostic location (`--load` vs file face vs oneshot) | `tests/python/test_issue_4864.py` | `--load`, `oneshot` |
| cross-mode | `tests/python/test_issue_4908_cross_mode.py` | `cross-mode` |
| sandbox / Restricted | `tests/python/test_issue_4870_production_sandbox.py` | `Restricted`, `AURA_SANDBOX` |

## C++ query homes

The theme batch table below stays the C++ map. `home` prints one file for
the words in this table. Schema-only rows still go in
`tests/compiler/obs_schema_cases.hpp`.

| Theme | Home now | Ask with |
|---|---|---|
| Arena | `tests/core/test_arena_batch.cpp` | `arena` |
| FlatAST | `tests/core/test_flatast_atomic_lock_batch.cpp` | `FlatAST` |
| Mutation boundary | `tests/compiler/test_mutation_boundary_batch.cpp` | `mutation boundary` |
| Occurrence | `tests/compiler/test_mutation_occurrence_dirty_batch.cpp` | `occurrence` |
| Linear ownership | `tests/compiler/test_linear_cross_closure.cpp` | `linear` |
| Dead coercion | `tests/compiler/test_dead_coercion_batch.cpp` | `dead coercion` |
| AOT / SpecJIT | `tests/compiler/test_jit_aot_hot_update_unit_batch.cpp` | `SpecJIT` |
| Observability schema | `tests/compiler/obs_schema_cases.hpp` | `obs schema` |
| Capability / sandbox | `tests/core/test_capability_sandbox_batch.cpp` | `capability` |
| Hot-path | `tests/core/test_hotpath_matrix_batch.cpp` | `hotpath` |

## Not a home

`lib/std/tests/` is not a home. Move a valid case into the suites above.
Do not create `test_issue_N.py`, `*_N.aura`, or `test_*_NNNN.cpp`.

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

**Detailed scan + merge plan:** [`CONSOLIDATION_PLAN.md`](CONSOLIDATION_PLAN.md).


**Status (2026-08-04):** multi-TU thematic batches done. Historical files keep
`_*NNNN` names as **member TUs** (`run_test_*` + `AURA_ISSUE_BATCH_MEMBER`);
**standalone `aura_add_issue_test` for issue-suffixed targets is removed**
(exception: special `test_issue_178` reflect dual-TU). CI / `all_test_issue_targets`
runs the batch drivers below.

| Wave | Family | Batch home | Status |
|------|--------|------------|--------|
| W0 | linear cross-closure #2563/#2612/#2623 | `compiler/test_linear_cross_closure.cpp` (single-TU fold) | done |
| W1 | FlatAST / column locks atomics | `core/test_flatast_atomic_lock_batch` | done |
| W2 | security / capability / grant / Restricted | `compiler/test_security_capability_batch` | done |
| W3 | densify / pin / Moving / envframe | `core/test_densify_pin_batch` | done |
| W4 | AOT / SpecJIT / stamp / relower | `compiler/test_aot_jit_stamp_batch` | done |
| W5 | mailbox / fiber / residual / chaos | `serve/test_mailbox_fiber_batch` | done |
| W6 | occurrence / cone / coercion | `compiler/test_occurrence_coercion_batch` | done |
| W7 | linear ownership residual | `compiler/test_linear_misc_batch` | done |
| W_orch | orch / agent | `orch/test_orch_agent_batch` | done |
| W_obs | obs / health leftovers | `compiler/test_obs_misc_batch` | done |
| W_other | leftovers | `compiler/test_misc_issue_fold_batch` | done |

**Tool:** `python3 scripts/tools/fold_issue_test_wave.py --wave W1 --apply`

**Next (optional):** strip `_NNNN` from member filenames when unique + rewrite
manifests (naming hygiene only — binary sprawl already fixed).

After further folds: regen `test-registry.json` + inventory.

## Enforcement

- **pre-commit (hard):** rejects newly staged `test_issue_*.cpp` and `test_*_<NNNN>.cpp`
- **Scaffold:** `tests/scaffolds/module_test_scaffold.cpp` — no issue suffix in name
- **Policy authority:** this file + `tests/README.md`
