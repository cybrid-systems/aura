// tests/scaffolds/legacy_test_redirect.cpp — LEGACY TEMPLATE redirect.
//
// @category: unit
// @reason: LEGACY TEMPLATE redirect — do NOT copy this into tests/issues/.
//
// This file is NOT a CMake target. Do not add_executable it.
//
// ═══════════════════════════════════════════════════════════════════════════
//  STOP — new work goes to tests/<src-aligned-subdir>/, not test_issue_*.cpp
// ═══════════════════════════════════════════════════════════════════════════
//
// Preferred scaffold:
//   tests/scaffolds/module_test_scaffold.cpp
//     → copy to tests/<src-aligned-subdir>/test_<module>_<feature>[_<issue>].cpp
//     → register in CMakeLists.txt
//
// Policy:
//   tests/README.md          (R1 src/-aligned guidelines)
//
// Decision tree (short):
//   1. New query:*-stats schema?  → tests/compiler/obs_schema_cases.hpp
//   2. Fits fiber / hygiene / typed_mutate / obs matrix?
//        → extend that suite
//   3. Family already batched (compact, soa, linear, …)?
//        → extend tests/test_*_batch.cpp
//   4. Else new tests/<src-aligned-subdir>/test_<module>_<feature>.cpp
//   5. NEVER new tests/issues/test_issue_N.cpp for routine work
//
// Historical note: R1 (2026-07-21) abandoned the tests/domain/ pilot in favor
// of the src/-aligned layout. tests/legacy_test_inventory.md (#1957) tracks
// legacy migrations.
//
// If you are migrating a *legacy* issue file, follow the inventory roadmap.

#include "test_harness.hpp"

import std;
