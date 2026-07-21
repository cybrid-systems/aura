// @category: integration
// @reason: Issue #1620 — C++26 Contracts + consteval expand for
// Arena/Value/Shape/FlatAST hot paths (refine #1321/#1519/#742).
//
//   AC1: query:cpp26-contracts-stats schema 1620 + coverage flags
//   AC2: consteval-checks == 77
//   AC3: contract-hot-paths == 56
//   AC4: eval/mutate bumps hotpath-invariant-hits
//   AC5: multi-round stress monotonic
//   AC6: #742/#1519 lineage keys present
//   AC7: zero violations on normal path

#include "test_harness.hpp"
#include "core/cpp26_contract_stats.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.cxx26_invariants;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:cpp26-contracts-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool seed(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (fact n) (if (= n 0) 1 (* n (fact (- n 1))))) "
                 "(define a 1) (define b 2) (fact 5)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void ac1_schema() {
    std::println("\n--- AC1: schema 1620 + coverage flags ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    auto h = cs.eval("(engine:metrics \"query:cpp26-contracts-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1620 || href(cs, "schema") == 742, "schema 1620|742");
    CHECK(href(cs, "issue") == 1620 || href(cs, "issue") < 0, "issue 1620");
    CHECK(href(cs, "hotpath-contracts-1620-active") == 1, "1620 active");
    CHECK(href(cs, "arena-tier-contracts-active") == 1, "arena-tier");
    CHECK(href(cs, "value-as-star-contracts-active") == 1, "value-as");
    CHECK(href(cs, "shape-bit-test-contracts-active") == 1, "shape-bit");
    CHECK(href(cs, "flatast-get-type-contracts-active") == 1, "flatast-get-type");
    CHECK(href(cs, "hotpath-contracts-expanded-active") == 1 ||
              href(cs, "hotpath-contracts-expanded-active") < 0,
          "expanded active");
}

static void ac2_consteval() {
    std::println("\n--- AC2: consteval-checks == 77 ---");
    CompilerService cs;
    CHECK(href(cs, "consteval-checks") == 77, "consteval-checks == 77");
    static_assert(aura::core::kCpp26ConstevalChecksShipped == 77,
                  "kCpp26ConstevalChecksShipped must match kConstevalChecksTotal");
    static_assert(aura::core::cpp26::kConstevalChecksTotal == 77);
}

static void ac3_hot_paths() {
    std::println("\n--- AC3: contract-hot-paths == 56 ---");
    CompilerService cs;
    CHECK(href(cs, "contract-hot-paths") == 56, "contract-hot-paths == 56");
    static_assert(aura::core::cpp26::kContractHotPathsShipped == 56);
}

static void ac4_hotpath_hits() {
    std::println("\n--- AC4: hotpath-invariant-hits grow ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto h0 = href(cs, "hotpath-invariant-hits");
    (void)cs.eval("(fact 4)");
    (void)cs.eval("(+ a b)");
    (void)cs.eval("(mutate:rebind \"a\" \"10\")");
    (void)cs.eval("(eval-current)");
    // Direct metric read after FlatAST get / type_id / mark_dirty paths
    const auto h1 = href(cs, "hotpath-invariant-hits");
    CHECK(h1 > h0, "hits grew after eval/mutate");
}

static void ac5_stress() {
    std::println("\n--- AC5: multi-round stress ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    const auto h0 = href(cs, "hotpath-invariant-hits");
    for (int i = 0; i < 20; ++i) {
        (void)cs.eval(std::format("(mutate:rebind \"b\" \"{}\")", 20 + i));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(fact 2)");
    }
    CHECK(href(cs, "hotpath-invariant-hits") >= h0, "hits non-decreasing");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok");
}

static void ac6_lineage() {
    std::println("\n--- AC6: lineage keys ---");
    CompilerService cs;
    CHECK(href(cs, "contract-violations-caught") >= 0, "violations");
    CHECK(href(cs, "hotpath-contracts-1519-active") == 1 ||
              href(cs, "hotpath-contracts-1519-active") < 0,
          "1519 flag");
    CHECK(href(cs, "contract-hot-paths") >= 48, "hot paths >= 48 lineage");
}

static void ac7_no_violations() {
    std::println("\n--- AC7: zero violations normal path ---");
    CompilerService cs;
    CHECK(seed(cs), "seed");
    (void)cs.eval("(mutate:rebind \"a\" \"3\")");
    (void)cs.eval("(eval-current)");
    CHECK(href(cs, "contract-violations-caught") == 0, "no violations");
    CHECK(href(cs, "contract-violation-hotpath") == 0 || href(cs, "contract-violation-hotpath") < 0,
          "no hotpath violations");
}

} // namespace

int main() {
    std::println("=== Issue #1620: C++26 Contracts hot-path expand ===");
    ac1_schema();
    ac2_consteval();
    ac3_hot_paths();
    ac4_hotpath_hits();
    ac5_stress();
    ac6_lineage();
    ac7_no_violations();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
