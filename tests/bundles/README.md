# tests/bundles — issue-test link profiles

These are **not** hand-written test suites. Each profile is one fat executable
that links many `tests/issues/test_issue_*.cpp` (and a few domain pilots)
together so CI does not link 200+ × ~200MB standalones.

## Layout

| Path | Role |
|------|------|
| `issue_bundle_runner.{hh,cpp}` | **Shared** fork-isolated member runner (hand-written) |
| `test_issues_<profile>_main.cpp` | **Generated** member table + `main()` (slim) |
| `../fixtures/issue_link_profiles.json` | Profile → member list |
| `../../cmake/AuraIssueBundles.cmake` | Generated CMake helper |
| `../../scripts/tools/gen_issue_bundles.py` | Regenerator (`--check` / default write) |

```bash
python3 scripts/tools/gen_issue_bundles.py          # rewrite mains + cmake helper
python3 scripts/tools/gen_issue_bundles.py --check  # CI freshness
```

## Profiles (link graphs differ — do not merge executables)

| Profile | Link helper | Notes |
|---------|-------------|-------|
| light / light_late | `aura_issue_test_link_light` (shared `aura_jit_light_test_objects`, no LLVM) | light_late adds reflect TU for #178 |
| jit / jit_late1–5 | `aura_issue_test_link_llvm_jit` (full LLVM) | late* split only for link-time size |
| jit_minimal / jit_contract / jit_tests | thinner JIT | |
| fiber | fiber stubs | |

Standalone issue tests should prefer **light** by default (see `tests/README.md` · link profiles).
Full LLVM is opt-in; the allow-list is `cmake/issue_tests_need_full_llvm.txt`.

Merging **profiles** into fewer binaries would re-create multi-GB link units
and mix incompatible stubs. Deduping the **12 identical main skeletons** into
one runner (done) is the right consolidation.

## Run

```bash
ninja -C build test_issues_fiber
./build/test_issues_fiber
```
