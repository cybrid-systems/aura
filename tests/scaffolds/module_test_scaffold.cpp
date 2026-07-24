// @category: integration
// @reason: TEMPLATE only — copy to tests/domain/test_domain_<theme>_<aspect>.cpp
//          and register in cmake/AuraDomainTests.cmake.
//
// This file is NOT a CMake target. Do not add_executable it.
//
// Policy: tests/README.md · tests/domain/README.md · issue #1958
// Inventory / migration: tests/legacy_test_inventory.md (#1957)
//
// ── BEFORE YOU CREATE A NEW FILE ──────────────────────────────────────────
// 1. Can this AC live in an existing suite?
//      test_domain_gates_batch (fiber / hygiene / typed-mutate)
//      test_obs_schema_matrix + cases/obs_schema_cases.hpp
// 2. Is it only a stats schema? → add a row to obs_schema_cases.hpp, STOP.
// 3. Only then copy this template to tests/domain/ and register CMake.
// 4. NEVER add tests/issues/test_issue_N.cpp for new work.
//
// ── COPY-PASTE CHECKLIST ──────────────────────────────────────────────────
//  [ ] Rename to tests/domain/test_domain_<theme>_<aspect>.cpp
//  [ ] Replace THEME / NNNN / query names / ACs below
//  [ ] cmake/AuraDomainTests.cmake:
//        aura_add_issue_test(test_domain_<theme>_<aspect>)
//        aura_issue_test_link_llvm_jit(test_domain_<theme>_<aspect>)  # if needed
//        add_dependencies(all_test_issue_targets test_domain_<theme>_<aspect>)
//  [ ] ninja -C build test_domain_<theme>_<aspect> && ./build/test_domain_<theme>_<aspect>
//  [ ] Commit message explains why a new suite (not an extension) was required

#include "test_harness.hpp" // unified harness only (#1960)

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void expect_hash_schema(CompilerService& cs, std::string_view q, std::int64_t schema) {
    auto h = cs.eval(aura::test::aura_call_expr(q));
    CHECK(h && is_hash(*h), std::format("{} returns hash", q));
    CHECK(href(cs, q, "schema") == schema, std::format("{} schema == {}", q, schema));
}

// Domain suites optimize for *invariants of a theme*, not one GitHub issue.
// Group related ACs; cite issue numbers in banners / CHECK messages only.
void run_suite(CompilerService& cs) {
    auto& ev = cs.evaluator();
    (void)ev;

    // ── Example: observability gate (replace or delete) ──
    std::println("\n=== THEME surface (#NNNN) ===");
    // expect_hash_schema(cs, "query:REPLACE-ME-stats", /*schema=*/NNNN);
    // ev.bump_REPLACE_ME();
    // CHECK(href(cs, "query:REPLACE-ME-stats", "total") >= 1, "total after bump");

    // ── Example: functional invariant ──
    std::println("\n=== THEME functional smoke ===");
    auto d = cs.eval("(define domain-tmpl-x 1)");
    CHECK(d.has_value(), "define domain-tmpl-x");
    auto r = cs.eval("domain-tmpl-x");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "domain-tmpl-x == 1");

    // ── Prefer broader contracts ──
    // Good: "after compact, StableNodeRef still resolves"
    // Bad:  "only checks that counter_added_in_pr_NNNN is non-zero"
}

} // namespace

// Bundle-friendly entry (name must match file theme; keep stable once registered).
int aura_issue_domain_THEME_run() {
    CompilerService cs;
    run_suite(cs);
    std::println("\n=== domain THEME: {} passed, {} failed ===", aura::test::g_passed,
                 aura::test::g_failed);
    return aura::test::g_failed == 0 ? 0 : 1;
}

int main() {
    return aura_issue_domain_THEME_run();
}
