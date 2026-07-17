// @category: unit
// @reason: TEMPLATE only — copy to tests/test_<feature>.cpp and register via
//          aura_add_issue_test(...) in cmake/AuraDomainTests.cmake (or CMakeLists).
//          See docs/test_harness_pattern.md (Issue #1570).
//
// This file is NOT a CMake target. Do not add_executable it.

/*
================================================================================
COPY-PASTE CHECKLIST
  1. Rename to tests/test_<name>.cpp (or tests/domain/<name>.cpp)
  2. Replace ISSUE_N / query name / ACs
  3. cmake/AuraDomainTests.cmake:
       aura_add_issue_test(test_<name>)
       aura_issue_test_link_llvm_jit(test_<name>)   # if CompilerService/JIT needed
       add_dependencies(all_test_issue_targets test_<name>)
  4. Build: cmake --build build --target test_<name> -j
  5. Run:   ./build/test_<name>
  6. If you touch evaluator_primitives*.cpp / evaluator.ixx: gate needs this
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
