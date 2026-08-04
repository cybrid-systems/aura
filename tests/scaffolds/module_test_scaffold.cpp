// tests/scaffolds/module_test_scaffold.cpp — TEMPLATE only.
//
// @category: integration
// @reason: TEMPLATE only — copy to tests/<src-aligned-subdir>/test_<module>_<feature>.cpp
//          and register via aura_add_issue_test() in CMakeLists.txt.
//
// This file is NOT a CMake target. Do not add_executable it.
//
// Policy: tests/README.md (R1 src/-aligned layout)
//
// ── BEFORE YOU CREATE A NEW FILE ──────────────────────────────────────────
// 1. Read tests/HOMES.md — prefer an existing thematic suite.
// 2. Schema-only? → obs_schema_cases.hpp row, STOP.
// 3. Same family already has a file? → extend it (add AC + run_all), STOP.
// 4. Only then copy this template to tests/<src-module>/test_<module>_<feature>.cpp
// 5. NEVER: test_issue_N.cpp or test_*_<issue_number>.cpp (pre-commit hard-fails).
//
// ── COPY-PASTE CHECKLIST ──────────────────────────────────────────────────
//  [ ] Name: tests/<src-module>/test_<module>_<feature>.cpp  (NO issue suffix)
//  [ ] Banner: // Issue #NNNN — …   (number in comment only)
//  [ ] CMakeLists.txt:
//        aura_add_issue_test(test_<module>_<feature>)
//        aura_issue_test_link_light(test_<module>_<feature>)   # default
//        add_dependencies(all_test_issue_targets test_<module>_<feature>)
//  [ ] Coverage: scripts/coverage/manifests/<N>.json if declarative AC
//  [ ] ninja -C build test_<module>_<feature> && ./build/test_<module>_<feature>
