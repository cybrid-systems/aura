// @category: unit
// @reason: TEMPLATE only — copy to tests/test_<feature>.cpp and register via
//          aura_add_issue_test(...) in cmake/AuraDomainTests.cmake (or CMakeLists).
//
// This file is NOT a CMake target. Do not add_executable it.

// ⚠️  BEFORE creating a new tests/test_issue_NNN.cpp OR tests/test_<name>.cpp  ⚠️
//
// Aura convention (Anqi 2026-07-20 directive, "ship in order"):
//   1. Look at the 5 domain files in tests/ first:
//        - tests/test_fiber.cpp        (fiber / GC safepoint / steal / resume)
//        - tests/test_ir.cpp           (parser / IR / lowering / opt / type checker)
//        - tests/test_observability.cpp (observability / stats / metrics / counters)
//        - tests/test_mutation.cpp     (mutation / dirty_propagation / post_invalidate)
//        - tests/test_persist.cpp      (persist / save / load / stdlib)
//   2. If your issue fits one of these domains, add a test case THERE
//      (not a new file). Use the AURA_ISSUE_TEST macro (defined in
//      tests/test_harness.hpp, Phase 4 2026-07-20) — the new pattern:
//
//           AURA_ISSUE_TEST(NNN, "short description", {
//               // test body — uses CHECK(...) for assertions
//               ...
//           });
//
//      The macro expands to `extern "C" int aura_issue_NNN_run()`
//      that the bundle driver (tests/bundles/test_issues_*_main.cpp)
//      declares and calls. The domain file is the natural home for
//      domain-related tests; the per-issue file pattern is the
//      LEGACY pattern (tests/issues/test_issue_NNN.cpp) that we're
//      moving away from. See test_harness.hpp for full macro doc.
//   3. Only create a NEW file if the issue is truly a NEW DOMAIN
//      (no existing domain file fits). Justify in commit message
//      "why this needs a new domain file".
//   4. The pre-commit hook will WARN (not block) on new test_issue_*.cpp
//      files added without a justification comment in the commit message.
//
// The 5 domain files are PLACEHOLDERS today (Phase 2 of consolidation
// 2026-07-20). Future issues should add test cases there. See
// tests/domain_classification.md for the current mapping of which
// existing tests/issues/test_issue_NNN.cpp files belong to which
// domain (Phase 4+ will physically migrate them).

/*
================================================================================
COPY-PASTE CHECKLIST

  DEFAULT PATH (for most new issues):
    1. Add a test case to one of the 5 domain files in tests/ root
       (test_fiber.cpp / test_ir.cpp / test_observability.cpp /
        test_mutation.cpp / test_persist.cpp).
       Use the AURA_ISSUE_TEST macro (defined in tests/test_harness.hpp,
       Phase 4 2026-07-20):

         AURA_ISSUE_TEST(NNN, "short description", {
             // test body — uses CHECK(...) for assertions
             ...
         });

       The macro expands to `extern "C" int aura_issue_NNN_run()` that
       the bundle driver (tests/bundles/test_issues_*_main.cpp)
       declares and calls. See test_harness.hpp for full doc.

  EXCEPTION PATH (only if no existing domain file fits):
    2. NEW FILE: rename to tests/test_<name>.cpp (or tests/domain/<name>.cpp)
    3. Replace ISSUE_N / query name / ACs
    4. cmake/AuraDomainTests.cmake:
         aura_add_issue_test(test_<name>)
         aura_issue_test_link_llvm_jit(test_<name>)   # if CompilerService/JIT needed
         add_dependencies(all_test_issue_targets test_<name>)
    5. Build: cmake --build build --target test_<name> -j
    6. Run:   ./build/test_<name>
    7. If you touch evaluator_primitives*.cpp / evaluator.ixx: gate needs this
       tests/ file staged with the prod change (#1453 binding).
================================================================================
*/

#include "test_harness.hpp"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

// Optional: hash-ref helper for engine:metrics dashboards.
std::int64_t href_m(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    // ── AC0: stats surface shape (schema == issue number) ──
    {
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:REPLACE-ME-stats"))");
        CHECK(h && is_hash(*h), "stats is hash");
        // CHECK(href_m(cs, "query:REPLACE-ME-stats", "schema") == ISSUE_N, "schema");
        // CHECK(href_m(cs, "query:REPLACE-ME-stats", "active") == 1, "active");
        (void)href_m;
        ++g_passed; // placeholder when template not filled
        std::println("  SKIP: fill ACs before registering this target");
    }

    // ── AC1: functional happy path ──
    // {
    //     CompilerService cs;
    //     auto r = cs.eval("...");
    //     CHECK(r.has_value(), "eval ok");
    // }

    // ── AC2: denial / error path ──
    // ── AC3: metrics bump ──
    // ── AC4: stress (threads / 1000-iter) ──

    std::println("\n=== template: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
