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
// 1. Can this AC live in an existing suite?
//      test_arena_batch / test_hotpath_matrix_batch / test_soa_batch
//      test_obs_schema_matrix + tests/compiler/obs_schema_cases.hpp
// 2. Is it only a stats schema? → add a row to obs_schema_cases.hpp, STOP.
// 3. Only then copy this template to tests/<src-aligned-subdir>/ and register CMake.
// 4. NEVER add tests/issues/test_issue_N.cpp for new work.
//
// ── COPY-PASTE CHECKLIST ──────────────────────────────────────────────────
//  [ ] Rename to tests/<src-aligned-subdir>/test_<module>_<feature>[_<issue>].cpp
//  [ ] Replace THEME / NNNN / query names / ACs below
//  [ ] CMakeLists.txt:
//        aura_add_issue_test(test_<module>_<feature>[_<issue>])
//        aura_issue_test_link_llvm_jit(test_<module>_<feature>[_<issue>])  # if needed
//        add_dependencies(all_test_issue_targets test_<module>_<feature>[_<issue>])
//  [ ] ninja -C build test_<module>_<feature>[_<issue>] &&
//  ./build/test_<module>_<feature>[_<issue>]
