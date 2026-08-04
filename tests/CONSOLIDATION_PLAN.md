# Issue-suffix test consolidation plan

**Scan date:** 2026-08-04  
**Scope:** `tests/**/test_*_<3–5 digit>.cpp` + `test_issue_*.cpp`  
**Authority:** [`HOMES.md`](HOMES.md) (where new ACs go) · this doc (how to finish historical renames/merges)

---

## 1. Scan snapshot

| Metric | Count |
|--------|------:|
| Issue-numbered test files | **404** |
| Already multi-TU batch members | **402** |
| Still `aura_add_issue_test` standalone | **0** |
| Not in any batch | **2** (`tests/reflect/test_issue_178*.cpp` — special dual-TU) |
| Batch-ready (`run_test_*` + `AURA_ISSUE_BATCH_MEMBER`) | **403** |
| Total size | ~4.3 MB |

### By directory

| Dir | Files |
|-----|------:|
| `tests/compiler/` | 277 |
| `tests/core/` | 69 |
| `tests/serve/` | 33 |
| `tests/orch/` | 19 |
| `tests/reflect/` | 6 |

### Same-base multi-issue clusters (true merge candidates)

| Base | Issues | Note |
|------|--------|------|
| `test_issue_*` | 178×2, 1990–1993 | Reflect dual-TU + serve leftovers |
| `test_lock_order_audit_*` | 2316, 2354 | Merge → `test_lock_order_audit.cpp` |
| `test_production_safety_*` | 1047, 1097 | Merge → `test_production_safety.cpp` |
| `test_reemit_production_default_defer_*` | 2205, 2208 | Merge → `test_reemit_production_default_defer.cpp` |

Almost every other file is **unique base + one issue** — consolidation is **rename hygiene + re-home from mega-misc**, not 1:N content merge.

---

## 2. Current state (what is already done)

Binary sprawl is largely fixed: issue TUs are **members** of thematic batches, not hundreds of link targets.

| Batch | Members | Role |
|-------|--------:|------|
| `test_flatast_atomic_lock_batch` | 36 | FlatAST / SoA / lock / atomic |
| `test_security_capability_batch` | 34 | Cap / grant / security / audit |
| `test_densify_pin_batch` | 21 | Densify / pin / Moving / envframe |
| `test_aot_jit_stamp_batch` | 28 | AOT / SpecJIT / stamp / relower |
| `test_mailbox_fiber_batch` | 24 | Mailbox / fiber / steal / residual |
| `test_occurrence_coercion_batch` | 41 | Occurrence / cone / coercion / type |
| `test_linear_misc_batch` | 11 | Linear residual (non cross-closure) |
| `test_orch_agent_batch` | 15 | Orch / agent |
| `test_obs_misc_batch` | 3 | Health / obs leftovers |
| **`test_misc_issue_fold_batch`** | **189** | **Catch-all — next split priority** |
| `test_linear_cross_closure` | (single file) | W0 true content merge |

CMake still has `# folded: aura_add_issue_test(...)` comment needles so coverage contracts stay green.

**Remaining problem:** filenames still look like `test_add_node_builder_contract_2445.cpp`, and **189 files dump into one misc mega-batch**, which is hard to run/debug and teaches the wrong habit.

---

## 3. Goal state

1. **No new** `test_*_<issue>.cpp` (already hard-blocked in pre-commit).
2. **Historical files** either:
   - renamed to `test_<module>_<feature>.cpp` (issue only in banner + manifest), **or**
   - content-merged when bases collide (table above).
3. **Misc mega-batch split** into ~8 medium thematic batches (≤30 members each).
4. **Optional later:** physical single-file amalgamations only where suites share fixtures (e.g. already done for linear cross-closure).

---

## 4. Work streams (ordered)

### Stream A — Split `test_misc_issue_fold_batch` (highest value)

**Why:** 189 members = slow, opaque, wrong home map.

| Phase | New batch | Approx size | Source keywords / examples |
|-------|-----------|------------:|----------------------------|
| A1 | `test_mutation_hold_boundary_batch` | ~22 | `mutation_*`, `hold_`, `boundary`, `txn`, `guard_exit`, steal+mutation |
| A2 | `test_shape_soa_storm_batch` | ~14 | `shape_*`, `soa_*`, `storm`, columnar hot |
| A3 | `test_ir_closure_jit_misc_batch` | ~22 | `ir_*`, `closure_*`, `jit_*`, `dce_*`, `emit_object`, deopt |
| A4 | `test_module_query_batch` | ~13 | `module_*`, `query_*`, rebind, partition |
| A5 | `test_json_io_cap_batch` | ~9 | `json_*`, `load_cap`, `command_line`, `regex`, channel |
| A6 | `test_stable_ref_validate_batch` | ~8 | `stable_ref_*`, `validate_*`, fillup, endian |
| A7 | `test_production_hardening_batch` | ~8 | `production_*`, readiness, safety, chaos PR gate |
| A8 | `test_pcv_workspace_batch` | ~7 | `pcv_*`, workspace isolation/mtx |
| A9 | `test_arena_compact_hooks_batch` | ~12 | arena compact hooks, dtor, adaptive compact |
| A10 | residual → shrink `misc` to **≤40** | ~40–80 | true leftovers; reclassify next wave |

**Per phase checklist:**

1. `python3 scripts/tools/fold_issue_test_wave.py` (extend) or hand-move sources in CMake.
2. Move member paths from misc → new `add_executable(...)`.
3. Regenerate driver `run_*` list.
4. Refresh `# folded:` alias block if needed.
5. `cmake -S . -B build` + `ninja -C build <batch>` smoke.
6. `./build.py gate` (coverage needles).

### Stream B — Rename hygiene (filename without issue digits)

Do **after** Stream A homes are stable so renames land in the right batch.

**Rules:**

- `test_add_node_builder_contract_2445.cpp` → `test_add_node_builder_contract.cpp`
- Keep `_NNNN` only on **collision** (same base, multiple issues — Stream C).
- Update in the same PR:
  - CMake source list
  - `run_test_<new_stem>()` symbol (or keep old `run_` + alias — prefer rename both)
  - `scripts/coverage/manifests/*.json` paths
  - any `read_file("tests/.../test_*_NNNN.cpp")` in other tests
  - `# folded:` comments
  - `docs/generated/test-registry.json` (regen)

**Suggested rename waves** (by current thematic batch — lower risk first):

| Order | Batch | ~Files | Notes |
|------:|-------|-------:|-------|
| B1 | `flatast_atomic_lock` | 36 | Small, coherent, already correct home |
| B2 | `security_capability` | 34 | |
| B3 | `densify_pin` | 21 | |
| B4 | `linear_misc` + linear_cross_closure | 11+ | Cross-closure already clean name |
| B5 | `orch_agent` | 15 | |
| B6 | `mailbox_fiber` | 24 | |
| B7 | `occurrence_coercion` | 41 | |
| B8 | `aot_jit_stamp` | 28 | Watch llvm vs light assumptions |
| B9 | A1–A10 new batches | varies | Rename as you split |
| B10 | residual misc | ≤40 | Last |

**Automation sketch** (add to `fold_issue_test_wave.py` or new `rename_issue_suffix_tests.py`):

```text
for each member in batch:
  new = strip _NNNN if unique under tests/
  git mv + rewrite run_ symbol + rewrite path strings
```

### Stream C — True content merges (small, do anytime)

| Action | Files | Target |
|--------|-------|--------|
| Merge | `test_lock_order_audit_2316` + `_2354` | `test_lock_order_audit.cpp` |
| Merge | `test_production_safety_1047` + `_1097` | `test_production_safety.cpp` |
| Merge | `test_reemit_production_default_defer_2205` + `_2208` | `test_reemit_production_default_defer.cpp` |
| Keep special | `test_issue_178` + `_reflect` | Reflect dual-TU executable (do **not** dump into misc) |
| Absorb or delete | `test_issue_1990`–`1993` | Prefer fold into serve mailbox/fiber suite with banner issues |

Pattern: same as W0 `test_linear_cross_closure` — namespaces + one `main` / `run_all`.

### Stream D — Contract modernization (parallel, low risk)

Coverage / gate scripts still string-search:

```text
aura_add_issue_test(test_foo_NNNN)
aura_issue_test_link_light(...)
```

Today satisfied by `# folded:` comments. Prefer migrating contracts to:

- path exists: `tests/.../test_foo.cpp`
- **or** listed as source of a known batch target

Then delete the comment alias block (smaller `CMakeLists.txt`).

---

## 5. Theme inventory (all 404 — planning view)

Approximate keyword buckets (a file may fit multiple; primary used for routing):

| Theme | ~Files | Preferred long-term home |
|-------|-------:|--------------------------|
| flatast / lock / atomic | 52 | `flatast_atomic_lock_batch` (+ rename B1) |
| security / cap / audit | 47 | `security_capability_batch` |
| occurrence / type / coercion | 46 | `occurrence_coercion_batch` |
| aot / jit / stamp | 36 | `aot_jit_stamp_batch` |
| mailbox / fiber / steal | 31 | `mailbox_fiber_batch` |
| densify / pin / moving | 22 | `densify_pin_batch` |
| orch / agent | 16 | `orch_agent_batch` |
| arena / gc / compact | 12 | **new** A9 or existing arena batches |
| linear | 12 | `linear_misc` + `linear_cross_closure` |
| module / prim / json / ir mix | 56 | **split** A3–A5 |
| uncategorized residual | 56 | **split** A1, A2, A6–A10 |
| obs / health | 6 | `obs_misc` or schema matrix |
| lock_order / hot_pass | 6 | A2 / Stream C |
| legacy `test_issue_*` | 6 | Stream C |

---

## 6. Execution cadence (recommended)

| Sprint | Deliverable | Exit criteria |
|--------|-------------|----------------|
| **S0** (done) | Multi-TU fold + no standalone issue targets | gate green |
| **S1 / A1** (done) | `test_mutation_hold_boundary_batch` (21 members) | misc 189→168; batch registered |
| **S1 / A2** (done) | `test_shape_soa_storm_batch` (14 members) | misc 168→154; `shape_profiler_concurrency_2141` kept special exe |
| **S1 / A3** (done) | `test_ir_closure_jit_misc_batch` (23 members) | misc 154→131; IR/closure/JIT/deopt/DCE/PrimCall |
| **S1 / A4** (done) | `test_module_query_batch` (12 members) | misc 131→119; module/rebind + query hygiene/index |
| **S1** (rest) | Stream A5 (misc split continues) | misc ≤110; more new batches green |
| **S2** | Stream A6–A10 + Stream C merges | misc ≤40; 3 content merges |
| **S3** | Stream B1–B5 renames | those batches have **zero** `_NNNN` filenames |
| **S4** | Stream B6–B10 renames | issue-suffix file count → **&lt; 30** specials only |
| **S5** | Stream D contract cleanup | remove `# folded:` block |

Do **not** rename 400 files in one PR — batch-sized PRs (~20–40 files) keep pre-push/gate reviewable.

---

## 7. Definition of done (whole program)

- [ ] `rg '_\\d{3,5}\\.cpp$' tests --glob 'test_*.cpp'` → only allowlisted exceptions (e.g. none, or `test_issue_178*`)
- [ ] No `test_misc_issue_fold_batch` over 40 members (or batch deleted)
- [ ] Every theme in `HOMES.md` maps to a real suite path without issue digits
- [ ] Coverage manifests point at thematic paths
- [ ] `./build.py gate` green; sample `ninja -C build test_*_batch && ./build/test_*_batch`

---

## 8. Commands cheat sheet

```bash
# inventory
python3 - <<'PY'
from pathlib import Path
import re
n=list(Path('tests').rglob('test_*.cpp'))
iss=[p for p in n if re.search(r'_\d{3,5}\.cpp$|test_issue_\d+', p.name)]
print(len(iss), 'issue-suffixed')
PY

# existing fold tool
python3 scripts/tools/fold_issue_test_wave.py --list
python3 scripts/tools/fold_issue_test_wave.py --wave W1 --dry-run

# after each wave
cmake -S . -B build
ninja -C build test_flatast_atomic_lock_batch   # example
./build.py gate
python3 scripts/tools/gen_test_registry.py
```

---

## 9. Explicit non-goals (for now)

- Rewriting AC bodies or weakening coverage contracts
- Moving files out of `tests/<src-module>/` layout
- Reintroducing `tests/issues/` or `tests/domain/`
- One-shot amalgamating all 404 into a handful of 10k-line files (unreviewable)

---

## 10. Next action (when resuming)

Start **S1 / A1**: carve `test_mutation_hold_boundary_batch` out of `test_misc_issue_fold_batch` (~22 files), smoke-run, gate, then continue A2–A5.

Agent prompt one-liner:

> Extend existing thematic batch (or HOMES.md home); never add `test_*_<issue>.cpp`. Historical renames follow `tests/CONSOLIDATION_PLAN.md` Stream B after misc split.
