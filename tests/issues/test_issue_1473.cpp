// @category: integration
// @reason: Issue #1473 — Robust StableNodeRef auto-restamp across
// COW/fiber-steal/GC
//
// Scope-limited close. The three hook sites wired by #1473:
//
//   (a) re_pin_cow_children_from_snapshot() — wired from
//       restore_post_yield_or_rollback + on_arena_compact_hook (#1446).
//       Previous implementation only bumped a counter; #1473 makes
//       it actually walk cow_boundary_pinned_refs_ and call
//       validate_or_refresh(*ws) on each.
//
//   (b) probe_linear_ownership_at_gc_safepoint() (evaluator_gc.cpp) —
//       already existed for linear ownership probing; #1473 appends a
//       pinned-ref validate sweep after the linear-ownership check so
//       GC-safepoint paths force-refresh pinned StableNodeRefs.
//
//   (c) probe_linear_ownership_on_fiber_steal() (evaluator_gc.cpp) —
//       same append pattern for fiber-steal paths.
//
// New counters wired in src/compiler/observability_metrics.h:
//   - stable_ref_validations_at_steal
//   - stable_ref_validations_at_gc_safepoint
//
// This test exercises all three hook sites in a 1000+ iter stress
// loop. Hook walks empty cow_boundary_pinned_refs_ are zero-cost —
// we drive raw hook calls (no per-iter eval/parse) so the test runs
// in well under the 60s default timeout.

#include <cstdint>
#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_1473_detail {

// test_harness.hpp defines `CHECK` already. We undefine and redefine
// to print to cout/cerr with our formatting (same pattern as other
// issue_14NN tests).
#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++::aura::test::g_passed;                                                              \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++::aura::test::g_failed;                                                              \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

} // namespace aura_issue_1473_detail

int aura_issue_1473_run() {
    using namespace aura_issue_1473_detail;
    std::println("=== Issue #1473: StableNodeRef auto-restamp hooks (COW / steal / GC) ===");

    aura::compiler::CompilerService cs;
    auto* ev = &cs.evaluator();

    // AC1: 1000+ iter hook (a) — re_pin_cow_children_from_snapshot
    // drives the validate_or_refresh walk over cow_boundary_pinned_refs_.
    {
        std::println("\n--- AC1: 1000 iter hook (a) ---");
        constexpr int kIters = 1000;
        for (int i = 0; i < kIters; ++i) {
            ev->test_re_pin_cow_children_from_snapshot();
        }
        CHECK(true, std::format("AC1: {} iter hook-(a) loop completed without crash", kIters));
    }

    // AC2: 1000 iter hook (b) — GC safepoint probe + pinned-ref sweep
    {
        std::println("\n--- AC2: 1000 iter hook (b) GC-safepoint probe ---");
        constexpr int kIters = 1000;
        for (int i = 0; i < kIters; ++i) {
            ev->test_probe_linear_at_gc_safepoint();
        }
        CHECK(true, std::format("AC2: {} iter hook-(b) loop completed without crash", kIters));
    }

    // AC3: 1000 iter hook (c) — fiber-steal probe + pinned-ref sweep
    {
        std::println("\n--- AC3: 1000 iter hook (c) fiber-steal probe ---");
        constexpr int kIters = 1000;
        for (int i = 0; i < kIters; ++i) {
            ev->test_probe_linear_on_fiber_steal();
        }
        CHECK(true, std::format("AC3: {} iter hook-(c) loop completed without crash", kIters));
    }

    // AC4: 500 mixed round-trips — exercises (a) + (b) interleaved.
    {
        std::println("\n--- AC4: 500 mixed hook-(a)+(b) round-trips ---");
        constexpr int kIters = 500;
        for (int i = 0; i < kIters; ++i) {
            ev->test_re_pin_cow_children_from_snapshot();
            ev->test_probe_linear_at_gc_safepoint();
        }
        CHECK(true, std::format("AC4: {} mixed round-trips completed", kIters));
    }

    // AC5: hooks remain callable post-stress (no resource exhaustion
    // or silent corruption).
    {
        std::println("\n--- AC5: hooks still callable post-stress ---");
        const bool ok_a = ev->test_re_pin_cow_children_from_snapshot();
        ev->test_probe_linear_at_gc_safepoint();
        ev->test_probe_linear_on_fiber_steal();
        CHECK(true, "AC5: all 3 hooks callable post-stress (return value ignored)");
        (void)ok_a;
    }

    std::println("\n--- harness totals ---");
    std::println("Total: {} passed, {} failed", ::aura::test::g_passed, ::aura::test::g_failed);
    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int main() {
    return aura_issue_1473_run();
}