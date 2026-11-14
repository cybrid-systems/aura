// tests/scaffolds/legacy_test_redirect.cpp — LEGACY TEMPLATE redirect.
//
// @category: unit
// @reason: LEGACY TEMPLATE redirect — do NOT copy this into tests/issues/.
//
// This file is NOT a CMake target. Do not add_executable it.
//
// ═══════════════════════════════════════════════════════════════════════════
//  STOP — new work follows tests/HOMES.md. Do not add test_issue_*.
// ═══════════════════════════════════════════════════════════════════════════
//
// Preferred scaffold:
//   tests/scaffolds/module_test_scaffold.cpp
//     → copy to tests/<src-module>/test_<module>_<feature>.cpp
//       (no issue digits)
//     → register in CMakeLists.txt
//
// Policy: tests/HOMES.md
//   python3 tests/run.py home "<query>"
//   python3 build.py gate rejects a new issue-numbered file.
//
// Historical note: R1 (2026-07-21) abandoned the tests/domain/ pilot in favor
// of the src/-aligned layout. tests/legacy_test_inventory.md (#1957) tracks
// legacy migrations.
//
// If you are migrating a *legacy* issue file, follow the inventory roadmap.

#include "test_harness.hpp"

import std;
