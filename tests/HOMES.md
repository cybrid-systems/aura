# Test homes — where new cases go

**Agents: read this before creating a test.** This file is the one decision
tree and the genealogy. It covers Aura, Python, and C++.

`python3 build.py gate` rejects a newly added issue-numbered file. The
optional pre-commit hook runs the same check on staged adds. A missing
`core.hooksPath` does not skip the gate. Modifying or deleting a historical
numbered file is allowed.

Issue numbers belong in case metadata, comments, and coverage manifests —
not in a new filename.

**Manifest policy:** [`COVERAGE.md`](COVERAGE.md). Do not add a substring-only
`check_*.py`, and do not put an issue number in a new check filename.
**Hot-path matrix only:** [`STRATEGY.md`](STRATEGY.md).

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

## Decision tree

```
1. Ask. Do not guess a file.

     python3 tests/run.py home "<query>"
     python3 tests/run.py cover --issue N
     python3 tests/run.py cover --name <symbol>

   A hit prints `home: <path>` and an append snippet. A miss starts with
   `not found:` and does not name an existing file. A tie lists both
   paths and says not to guess.

2. Append in the language that hit prints.

   Aura
     Append one (test-case ...) to that file. Keep a single (test-file ...)
     and a single (test-finish). If the file still uses an older harness,
     still append the std/test shape. Do not add another failure counter.
     An unclosed program cannot sit inside test-case. Give it its own
     tests/suite/<module>_<feature>.aura with ;; expect-status: and
     ;; expect-err:.

   Python
     Process checks (CLI / help, diagnostic location, cross-mode,
     sandbox / Restricted) append a unittest method on that home.
     Behavior assertions do not go in those files.

   C++
     Add an AC function and call it from main / run_all.
     Schema-only rows (query:*-stats / engine:* key) go in
     tests/compiler/obs_schema_cases.hpp
     (+ production_sweep_cases.hpp when the production flag is set).
     Stop. No new .cpp for a schema row.

3. No home yet? Create one theme file. No issue digits in the name.

   Aura:  tests/suite/<module>_<feature>.aura
   C++:   tests/<src-module>/test_<module>_<feature>.cpp
          register once:
            aura_add_issue_test(test_<module>_<feature>)
            aura_issue_test_link_light(...)
            add_dependencies(all_test_issue_targets ...)
   Python behavior does not get a new test_issue_*.py.
   The issue number goes in the case metadata, a comment, or
   scripts/coverage/manifests/<N>.json.

4. Never create these (the gate fails the add; editing an existing
   file is allowed):

   - tests/**/test_issue_*.py
   - tests/**/test_*_<3–5 digits>.py
   - tests/suite/*_<3–5 digits>.aura
   - tests/suite/issue_<3–5 digits>_*.aura
   - tests/suite/issue_<3–5 digits>.aura
   - scripts/coverage/checks/check_*_<3–5 digits>*.py
   - scripts/coverage/checks/check_<3–5 digits>_*.py
   - scripts/coverage/checks/check_<3–5 digits>.py
   - tests/issues/test_issue_N.cpp
   - tests/**/test_issue_*.cpp
   - tests/**/test_*_<3–5 digits>.cpp
   - new tests/test_*.cpp at the repo root

   lib/std/tests/ is not a home. A new check script, when one is truly
   required, has no issue digits. This refusal does not ask for a new
   check script.
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

**Historical scan (not the how-to):** [`CONSOLIDATION_PLAN.md`](CONSOLIDATION_PLAN.md).


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

- **gate (hard):** `python3 build.py gate`, including `--changed` and the CI
  gate job, runs `scripts/tools/check_new_test_files.py`. It fails paths
  added versus `origin/main`, staged adds, and untracked files. It does not
  read `core.hooksPath`.
- **pre-commit:** the same script, staged adds only (`--cached`).
- A modified or deleted historical numbered file passes. A rename that shows
  up as delete + add fails when the new name is still on the never-create list.
- **Scaffold:** `tests/scaffolds/module_test_scaffold.cpp` — no issue suffix in the name.
- **Policy authority:** this file. Other docs point here.
