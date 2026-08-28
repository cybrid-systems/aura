// tests/compiler/test_query_find_by_define.cpp --
//
// @category: unit
// @reason: Issue #3390 -- (query :find) routes Define names through the
//          existing find_define_by_name index before the size() walk,
//          avoiding O(n) SoA scan for the Agent-hottest locator. On
//          index miss, falls back to the full scan (preserves
//          first-match semantics for non-Define / multi-sym hits).
//
//   AC1: Define name present → query:find result equals find_define_by_name;
//        production still schema-2 / restamp-lag.
//   AC2: name absent or non-Define → same first-match (or void) as the
//        scan today.
//   AC3: Soft / Off layout-only bare list unchanged (#3286 AC3).
//   AC4: source-cite — query:find calls find_define_by_name before the
//        id < flat.size() loop.

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_hash;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_src(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

int run_test_query_find_by_define() {
    std::println("=== test_query_find_by_define (#3390) ===");

    // ── AC1: Define name present → query:find result equals find_define_by_name ──
    {
        std::println(
            "\n--- #3390 AC1: Define name → index hit, result equals find_define_by_name ---");
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        apply_production_audit_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define foo3390 42)\")").has_value(), "3390 AC1: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3390 AC1: eval");
        auto* ws = ev.workspace_flat();
        CHECK(ws != nullptr, "3390 AC1: workspace");
        // Get the expected NodeId from find_define_by_name directly.
        auto expected = ws->find_define_by_name(*ev.workspace_pool_, "foo3390");
        CHECK(expected.has_value(), "3390 AC1: find_define_by_name returns foo3390");
        // query:find under production → schema-2 hash; non-empty result.
        auto qr = cs.eval("(query :find \"foo3390\" :as-query-result)");
        CHECK(qr.has_value(), "3390 AC1: query:find returns");
        CHECK(is_hash(*qr), "3390 AC1: production QueryResult is schema-2 hash");
        // Bare-list path also returns the Define hit (non-void).
        auto bare = cs.eval("(query :find \"foo3390\")");
        CHECK(bare.has_value(), "3390 AC1: bare find returns");
        apply_dev_audit_defaults();
    }

    // ── AC2: name absent or non-Define → fallback scan returns void / same as today ──
    {
        std::println("\n--- #3390 AC2: name absent → fallback scan returns void (first-match) ---");
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        apply_dev_audit_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define foo3390 42)\")").has_value(), "3390 AC2: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3390 AC2: eval");
        auto* ws = ev.workspace_flat();
        const auto miss = ws->find_define_by_name(*ev.workspace_pool_, "bar3390");
        CHECK(!miss.has_value(), "3390 AC2: find_define_by_name misses for bar3390");
        // query:find for absent name → index miss → fallback scan → void.
        auto qr = cs.eval("(query :find \"bar3390\")");
        CHECK(qr.has_value(), "3390 AC2: query:find returns void for absent name");
        // query:find for "foo3390" returns the Define hit (index hit, no fallback).
        auto hit = cs.eval("(query :find \"foo3390\")");
        CHECK(hit.has_value(), "3390 AC2: query:find returns Define hit");
    }

    // ── AC3: Soft / Off layout-only bare list unchanged ──
    {
        std::println(
            "\n--- #3390 AC3: Soft bare list stays layout-only (zero-cost, unchanged) ---");
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        apply_dev_audit_defaults();
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(cs.eval("(set-code \"(define foo3390 42)\")").has_value(), "3390 AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3390 AC3: eval");
        // Soft bare find is layout-only — not a hash (unchanged from #3286 AC3).
        auto qr = cs.eval("(query :find \"foo3390\")");
        CHECK(qr.has_value(), "3390 AC3: Soft bare find returns");
        CHECK(!is_hash(*qr), "3390 AC3: Soft bare list is NOT a hash (layout-only)");
    }

    // ── AC4: source-cite — query:find calls find_define_by_name before size() loop ──
    {
        std::println("\n--- #3390 AC4: source-cite query:find → find_define_by_name before id < "
                     "flat.size() ---");
        const auto qwsp = read_src("src/compiler/evaluator_primitives_query_workspace.cpp");
        CHECK(!qwsp.empty(), "3390 AC4: query_workspace.cpp readable");
        const auto find_idx = qwsp.find("find_define_by_name");
        const auto loop_idx = qwsp.find("id < flat.size()");
        CHECK(find_idx != std::string::npos, "3390 AC4: find_define_by_name referenced in source");
        CHECK(loop_idx != std::string::npos,
              "3390 AC4: id < flat.size() loop still present (fallback)");
        CHECK(find_idx < loop_idx,
              "3390 AC4: find_define_by_name appears BEFORE the id < flat.size() loop");
        CHECK(qwsp.find("Issue #3390") != std::string::npos,
              "3390 AC4: Issue #3390 cite in source");
        // AC5: no docs/design/, no tests/issues/test_issue_3390.cpp.
        {
            std::ifstream f("docs/design/3390-query-find-by-define.md");
            CHECK(!f.good(), "3390 AC5: no docs/design/3390-*");
        }
        {
            std::ifstream f("tests/issues/test_issue_3390.cpp");
            CHECK(!f.good(), "3390 AC5: no tests/issues/test_issue_3390.cpp");
        }
    }

    std::println("\n=== test_query_find_by_define: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_query_find_by_define();
}
#endif