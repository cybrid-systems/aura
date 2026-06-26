// @category: integration
// @reason: Issue #426 — Fine-grained dirty tracking and
//          minimal re-lower for AI mutate:rebind/set-body
//          (P0). Validates:
//            - ir_cache_pure pure functions
//              (count_dirty_blocks /
//              estimate_relower_blocks /
//              summarize_block_dirty) are callable
//              and return sensible counts
//            - CompilerService::total_dirty_block_count()
//              is callable and returns >= 0
//            - (query:compiler-cache-stats) primitive
//              is wired and returns an integer
//            - empty mask → 0 dirty blocks
//            - fully-dirty mask (all bits set) →
//              full_relower_candidates > 0
//            - (regression) prior #456/#460/#448
//              primitives still work


#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.core.ast;

namespace aura_issue_426_detail {

// ── AC1: query:compiler-cache-stats returns an integer ──
bool test_query_compiler_cache_stats() {
    std::println("\n--- AC1: query:compiler-cache-stats returns a value ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:compiler-cache-stats)");
    if (!r) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::is_int(*r),
          "query:compiler-cache-stats returns an integer");
    return true;
}

// ── AC2: count_dirty_blocks pure function ──
bool test_count_dirty_blocks() {
    std::println("\n--- AC2: count_dirty_blocks pure function ---");
    // empty mask → 0
    {
        std::vector<std::uint8_t> mask;
        const auto n = aura::compiler::count_dirty_blocks(mask);
        CHECK(n == 0, "empty mask returns 0 dirty blocks");
    }
    // all-bits-set in 1 byte → 8
    {
        std::vector<std::uint8_t> mask = {0xFF};
        const auto n = aura::compiler::count_dirty_blocks(mask);
        CHECK(n == 8, "mask {0xFF} returns 8 dirty blocks");
    }
    // mixed mask
    {
        std::vector<std::uint8_t> mask = {0x01, 0x03, 0x00};
        const auto n = aura::compiler::count_dirty_blocks(mask);
        CHECK(n == 3, "mask {0x01, 0x03, 0x00} returns 3 dirty blocks (1+2+0)");
    }
    return true;
}

// ── AC3: estimate_relower_blocks heuristic ──
bool test_estimate_relower_blocks() {
    std::println("\n--- AC3: estimate_relower_blocks heuristic ---");
    CHECK(aura::compiler::estimate_relower_blocks(0) == 0,
          "0 dirty → 0 (skip)");
    CHECK(aura::compiler::estimate_relower_blocks(1) == 1,
          "1 dirty → 1 (incremental)");
    CHECK(aura::compiler::estimate_relower_blocks(7) == 7,
          "7 dirty → 7 (incremental)");
    CHECK(aura::compiler::estimate_relower_blocks(8) ==
              static_cast<std::size_t>(-1),
          "8+ dirty → -1 (full re-lower sentinel)");
    CHECK(aura::compiler::estimate_relower_blocks(100) ==
              static_cast<std::size_t>(-1),
          "100 dirty → -1 (full re-lower sentinel)");
    return true;
}

// ── AC4: summarize_block_dirty aggregates ──
bool test_summarize_block_dirty() {
    std::println("\n--- AC4: summarize_block_dirty aggregates ---");
    // 3 functions: f0 clean, f1 partial (3 dirty), f2 full (8 dirty)
    std::vector<std::vector<std::uint8_t>> per_func = {
        {0x00},                                // 0 dirty
        {0x07},                                // 3 dirty
        {0xFF},                                // 8 dirty (full)
    };
    const auto s = aura::compiler::summarize_block_dirty(per_func);
    CHECK(s.functions_total == 3,
          "summarize: 3 functions total");
    CHECK(s.total_dirty_blocks == 11,
          "summarize: 0 + 3 + 8 = 11 total dirty blocks");
    CHECK(s.functions_with_dirty == 2,
          "summarize: 2 functions have at least 1 dirty block");
    CHECK(s.incremental_candidates == 1,
          "summarize: 1 function (f1) is incremental candidate (1..7 dirty)");
    CHECK(s.full_relower_candidates == 1,
          "summarize: 1 function (f2) is full-relower candidate (8+ dirty)");
    return true;
}

// ── AC5: CompilerService::total_dirty_block_count is
//         callable and returns >= 0 ──
bool test_service_total_dirty_block_count() {
    std::println("\n--- AC5: CompilerService::total_dirty_block_count ---");
    aura::compiler::CompilerService cs;
    const auto n = cs.total_dirty_block_count();
    CHECK(n >= 0,
          "CompilerService::total_dirty_block_count() returns >= 0");
    return true;
}

// ── AC6: query:compiler-cache-stats after mutate is
//         observable ──
bool test_query_compiler_cache_stats_after_mutate() {
    std::println("\n--- AC6: query:compiler-cache-stats observable ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(set-code \"(define c 1) (define d 2)\")")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(query:compiler-cache-stats)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    const auto count =
        static_cast<std::int64_t>(aura::compiler::types::as_int(*r));
    CHECK(count >= 0,
          "query:compiler-cache-stats count >= 0");
    return true;
}

// ── AC7: regression — prior #456/#460/#448 primitives
//         still work ──
bool test_regression_prior_primitives() {
    std::println("\n--- AC7: regression — prior primitives still work ---");
    aura::compiler::CompilerService cs;
    auto r1 = cs.eval("(query:compiler-incremental-stats)");
    CHECK(r1.has_value() && aura::compiler::types::is_int(*r1),
          "query:compiler-incremental-stats (regression for #460)");
    auto r2 = cs.eval("(query:mutation-coordination-stats)");
    CHECK(r2.has_value() && aura::compiler::types::is_int(*r2),
          "query:mutation-coordination-stats (regression for #448)");
    auto r3 = cs.eval("(query:mutation-impact)");
    CHECK(r3.has_value() && aura::compiler::types::is_int(*r3),
          "query:mutation-impact (regression for #456)");
    return true;
}

// ── AC8: define + eval smoke (regression) ──
bool test_define_eval_regression() {
    std::println("\n--- AC8: define + eval smoke ---");
    aura::compiler::CompilerService cs;
    if (!cs.eval("(define smoke-426-a 19)")) {
        ++g_failed;
        return false;
    }
    if (!cs.eval("(define smoke-426-b 23)")) {
        ++g_failed;
        return false;
    }
    auto r = cs.eval("(+ smoke-426-a smoke-426-b)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        ++g_failed;
        return false;
    }
    CHECK(aura::compiler::types::as_int(*r) == 42,
          "smoke: (+ 19 23) == 42 (regression)");
    return true;
}

int run_tests() {
    std::println("Issue #426 (Fine-grained dirty tracking + minimal re-lower)\n");
    test_query_compiler_cache_stats();
    test_count_dirty_blocks();
    test_estimate_relower_blocks();
    test_summarize_block_dirty();
    test_service_total_dirty_block_count();
    test_query_compiler_cache_stats_after_mutate();
    test_regression_prior_primitives();
    test_define_eval_regression();
    std::println("\nResults: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_issue_426_detail

int aura_issue_426_run() { return aura_issue_426_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_426_run(); }
#endif