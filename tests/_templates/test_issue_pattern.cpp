// @category: unit
// @reason: LEGACY TEMPLATE redirect — do NOT copy this into tests/issues/.
//
// This file is NOT a CMake target. Do not add_executable it.
//
// ═══════════════════════════════════════════════════════════════════════════
//  STOP — new work goes to tests/domain/, not test_issue_*.cpp
// ═══════════════════════════════════════════════════════════════════════════
//
// Preferred scaffold:
//   tests/templates/test_domain_pattern.cpp
//     → copy to tests/domain/test_domain_<theme>_<aspect>.cpp
//     → register in cmake/AuraDomainTests.cmake
//
// Policy:
//   tests/README.md          (#1958 guidelines)
//   tests/domain/README.md
//   tests/legacy_test_inventory.md  (#1957 inventory + migration)
//   docs/contributing.md
//
// Decision tree (short):
//   1. New query:*-stats schema?  → domain/cases/obs_schema_cases.hpp
//   2. Fits fiber / hygiene / typed_mutate / obs matrix?
//        → extend that suite
//   3. Family already batched (compact, soa, linear, …)?
//        → extend tests/test_*_batch.cpp
//   4. Else new tests/domain/test_domain_*.cpp from test_domain_pattern.cpp
//   5. NEVER new tests/issues/test_issue_N.cpp for routine work
//
// Historical note: earlier Phase-2 notes mentioned five placeholder domain
// files at tests/ root (test_fiber.cpp, …). The live preferred location is
// tests/domain/test_domain_*.cpp + case tables. See cmake/AuraDomainTests.cmake.
//
// If you are migrating a *legacy* issue file, follow the inventory roadmap
// (wave order in legacy_test_inventory.md), not this stub.

#include "test_harness.hpp"

import std;

// Intentionally empty of runnable ACs — open test_domain_pattern.cpp instead.
int main() {
    std::println("template redirect only; use tests/templates/test_domain_pattern.cpp");
    return 0;
}
