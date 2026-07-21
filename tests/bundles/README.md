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
| `../../scripts/gen_issue_bundles.py` | Regenerator (`--check` / default write) |

```bash
python3 scripts/gen_issue_bundles.py          # rewrite mains + cmake helper
python3 scripts/gen_issue_bundles.py --check  # CI freshness
```

## Profiles (link graphs differ — do not merge executables)

| Profile | Link helper | Notes |
|---------|-------------|-------|
| light / light_late | stub JIT + observability | light_late adds reflect TU for #178 |
| jit / jit_late1–5 | full LLVM JIT | late* split only for link-time size |
| jit_minimal / jit_contract / jit_tests | thinner JIT | |
| fiber | fiber stubs | |

Merging **profiles** into fewer binaries would re-create multi-GB link units
and mix incompatible stubs. Deduping the **12 identical main skeletons** into
one runner (done) is the right consolidation.

## Run

```bash
ninja -C build test_issues_fiber
./build/test_issues_fiber
```
