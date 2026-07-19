// @category: integration
// @reason: Issue #1488 — dead string_heap_ push pollution cleanup
//
// AC1: arena:adaptive-stats returns int pair (no dead heap push) — #1072
// AC2: long poll loop does not grow string_heap_ by 2×calls (dead-push invariant)
// AC3: pair shape still (trigger-count . skip-count) of ints
// AC4: production-hardening flag still reports arena-adaptive-no-dead-push

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac1_shape() {
    std::println("\n--- AC1: arena:adaptive-stats pair-of-ints shape ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"arena:adaptive-stats\")");
    CHECK(r.has_value(), "stats:get arena:adaptive-stats returns a value");
    if (!r)
        return;
    CHECK(is_pair(*r), "result is a pair");
    if (!is_pair(*r))
        return;
    const auto& pairs = cs.evaluator().pairs();
    const auto pidx = as_pair_idx(*r);
    CHECK(pidx < pairs.size(), "pair index in range");
    if (pidx >= pairs.size())
        return;
    const auto& pr = pairs[pidx];
    CHECK(is_int(pr.car) && is_int(pr.cdr), "car/cdr are ints (trigger . skip)");
    CHECK(as_int(pr.car) >= 0 && as_int(pr.cdr) >= 0, "counters non-negative");
}

static void ac2_no_heap_growth() {
    std::println("\n--- AC2: long poll does not pollute string_heap_ ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    constexpr int kIters = 1000;

    // Warm so one-time setup is out of the measurement window.
    for (int i = 0; i < 3; ++i) {
        auto w = cs.eval("(stats:get \"arena:adaptive-stats\")");
        CHECK(w.has_value() && is_pair(*w), "warm stats:get returns pair");
    }

    const auto heap_before = ev.string_heap().size();
    const auto pairs_before = ev.pairs().size();
    std::size_t ok = 0;
    for (int i = 0; i < kIters; ++i) {
        auto r = cs.eval("(stats:get \"arena:adaptive-stats\")");
        if (r && is_pair(*r))
            ++ok;
    }
    const auto heap_delta = ev.string_heap().size() - heap_before;
    const auto pairs_delta = ev.pairs().size() - pairs_before;
    std::println("  string_heap delta={} pairs delta={} ok={}", heap_delta, pairs_delta, ok);

    CHECK(ok == static_cast<std::size_t>(kIters), "all 1000 polls returned pairs");

    // Accounting (per cs.eval of the same source):
    //   +1 string_heap  — re-intern of the \"arena:adaptive-stats\" literal
    //   +1 pairs        — live (trigger . skip) return value
    // Pre-#1072 dead push added +2 more string_heap entries (to_string of
    // trigger/skip, immediately discarded) → heap growth would be ~3N.
    // Fixed (#1072/#1488): heap growth is ~1N from re-parse only.
    CHECK(heap_delta < static_cast<std::size_t>(2 * kIters),
          "string_heap growth < 2N (no dead counter push_back)");
    CHECK(heap_delta <= static_cast<std::size_t>(kIters) + 8,
          "string_heap growth ~1/call from re-parse, not 2 dead ints");
    // Primitive-attributable pollution = heap_delta - parse_cost.
    // With parse_cost ≈ pairs_delta (1:1), residual must be 0 (not +2N).
    const auto residual = heap_delta > pairs_delta ? heap_delta - pairs_delta : 0;
    std::println("  residual (heap - pairs) = {} (must be ~0, was +2N pre-fix)", residual);
    CHECK(residual <= 8, "no residual string_heap pollution beyond re-parse/pair accounting");
    CHECK(pairs_delta <= static_cast<std::size_t>(kIters) + 4,
          "pairs growth ~1 per poll (live return values)");
}

static void ac3_direct_primitive() {
    std::println("\n--- AC3: direct (stats:get) still pair-of-ints ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"arena:adaptive-stats\")");
    CHECK(r && is_pair(*r), "direct stats:get returns pair");
    if (!r || !is_pair(*r))
        return;
    const auto pidx = as_pair_idx(*r);
    const auto& pairs = cs.evaluator().pairs();
    if (pidx >= pairs.size()) {
        CHECK(false, "pair index in range");
        return;
    }
    CHECK(is_int(pairs[pidx].car) && is_int(pairs[pidx].cdr), "ints after multi-call path");
}

static void ac4_hardening_flag() {
    std::println("\n--- AC4: production-hardening arena-adaptive-no-dead-push ---");
    CompilerService cs;
    auto r = cs.eval("(hash-ref (engine:metrics \"query:production-hardening-1072-1096-stats\") "
                     "\"arena-adaptive-no-dead-push\")");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "arena-adaptive-no-dead-push == 1");
}

} // namespace

int main() {
    std::println("test_issue_1488: dead string_heap_ push pollution (#1488)");
    ac1_shape();
    ac2_no_heap_growth();
    ac3_direct_primitive();
    ac4_hardening_flag();
    std::println("\n#1488: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
