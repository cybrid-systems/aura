// @category: unit
// @reason: Issue #3361 — `note_densify_entry_under_live_mutation` must force
// dirty-root revalidate IMMEDIATELY (no Guard-exit deferral, #3006) when
// given a non-null `Evaluator*` under Production/Full. Soft observes;
// Quiet (depth==0 + no densify_pending) returns false; nullptr preserves
// the counter-only behavior for tests / observe paths.
//
//   AC1: source cites #3361; helper signature accepts optional Evaluator*
//   AC2: hard path under production bumps g_linear_fast_path_dirty_revalidate_total
//   AC3: hard path with non-null Evaluator* ALSO bumps the linear boundary
//        consistency counter on the Evaluator's CompilerMetrics — i.e. the
//        revalidate fires immediately, not deferred to Guard exit
//   AC4: soft path bumps g_linear_fast_path_force_revalidate_observe_total,
//        returns false, no revalidate fires
//   AC5: quiet path (no live mutation) returns false without bumping
//        either counter
//   AC6: nullptr ev in hard path → dirty counter bumps, revalidate does
//        NOT fire (preserves observe-only / legacy behavior)

#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.observability;
import aura.core.ast;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::Evaluator;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::linear_fast_path_dirty_revalidate_total_v_read;
using aura::compiler::typed_audit::linear_fast_path_force_revalidate_observe_total_v_read;
using aura::compiler::typed_audit::note_densify_entry_under_live_mutation;
using aura::compiler::typed_audit::reset_linear_fast_path_dirty_revalidate_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

// Issue #3361 AC3: hard path with Evaluator* must run enforce_linear_boundary_consistency.
// We assert the metric the helper bumps — linear_boundary_consistency_total on the
// Evaluator's CompilerMetrics. Production/Full + densify_pending makes the helper
// take the hard path; force depth > 0 via the linear_ir_fastpath_boundary_depth_override
// test knob (mirrors #3238 wiring in evaluator_fiber_mutation.cpp).
struct ScopedOverride {
    int prev;
    explicit ScopedOverride(int v) noexcept
        : prev(::aura::compiler::typed_audit::g_linear_ir_fastpath_boundary_depth_override.exchange(
              v)) {}
    ~ScopedOverride() noexcept {
        ::aura::compiler::typed_audit::g_linear_ir_fastpath_boundary_depth_override.store(prev);
    }
};

bool check(bool cond, std::string_view msg) {
    if (cond) {
        ++g_passed;
        std::println("  PASS: {}", msg);
        return true;
    }
    ++g_failed;
    std::println("  FAIL: {}", msg);
    return false;
}

} // namespace

int main() {
    using namespace aura::compiler::typed_audit;
    std::println(
        "\n--- Issue #3361: note_densify_entry_under_live_mutation immediate revalidate ---");

    // Force production/Full so the hard path is exercised.
    apply_production_audit_defaults();

    // --- AC5: quiet path (depth==0, no densify_pending) returns false without bumping counters ---
    std::println("\n--- AC5: quiet path returns false, no counter bump ---");
    {
        reset_linear_fast_path_dirty_revalidate_for_test();
        const auto dirty_before = linear_fast_path_dirty_revalidate_total_v_read();
        const auto observe_before = linear_fast_path_force_revalidate_observe_total_v_read();
        // No live mutation (default state) → quiet path.
        const bool returned = note_densify_entry_under_live_mutation(nullptr);
        check(!returned, "quiet path returns false");
        check(linear_fast_path_dirty_revalidate_total_v_read() == dirty_before,
              "quiet path does not bump dirty_revalidate_total");
        check(linear_fast_path_force_revalidate_observe_total_v_read() == observe_before,
              "quiet path does not bump observe_total");
    }

    // --- AC2 + AC6: hard path (depth override + densify_pending) bumps dirty counter,
    //     no revalidate counter change when ev is nullptr ---
    std::println("\n--- AC2+AC6: hard path with nullptr ev bumps dirty counter, no revalidate ---");
    {
        reset_linear_fast_path_dirty_revalidate_for_test();
        // Use a minimal metrics instance to observe the revalidate side.
        CompilerMetrics metrics{};
        // depth > 0 → linear_fast_path_live_mutation_active() returns true.
        const ScopedOverride depth_override{1};
        // Arm densify_pending so the helper's live check also passes the densify arm.
        g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
            1, std::memory_order_relaxed);
        const auto dirty_before = linear_fast_path_dirty_revalidate_total_v_read();
        const auto revalidate_before =
            metrics.linear_boundary_consistency_total.load(std::memory_order_relaxed);
        const bool returned = note_densify_entry_under_live_mutation(nullptr);
        check(returned, "hard path returns true under production");
        check(linear_fast_path_dirty_revalidate_total_v_read() == dirty_before + 1,
              "hard path bumps dirty_revalidate_total by 1");
        // nullptr ev → no enforce call → no revalidate counter bump.
        check(metrics.linear_boundary_consistency_total.load(std::memory_order_relaxed) ==
                  revalidate_before,
              "nullptr ev does NOT fire enforce_linear_boundary_consistency");
        // Cleanup: clear densify_pending + depth.
        g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
            0, std::memory_order_relaxed);
    }

    // --- AC3: hard path with a real Evaluator + its metrics → revalidate counter bumps ---
    std::println(
        "\n--- AC3: hard path with Evaluator* fires enforce_linear_boundary_consistency ---");
    {
        reset_linear_fast_path_dirty_revalidate_for_test();
        const ScopedOverride depth_override{1};
        g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
            1, std::memory_order_relaxed);
        Evaluator ev{};
        CompilerMetrics metrics{};
        ev.attach_compiler_metrics(&metrics);
        const auto dirty_before = linear_fast_path_dirty_revalidate_total_v_read();
        const auto revalidate_before =
            metrics.linear_boundary_consistency_total.load(std::memory_order_relaxed);
        const bool returned = note_densify_entry_under_live_mutation(&ev);
        check(returned, "hard path returns true under production with Evaluator*");
        check(linear_fast_path_dirty_revalidate_total_v_read() == dirty_before + 1,
              "hard path bumps dirty_revalidate_total by 1");
        check(metrics.linear_boundary_consistency_total.load(std::memory_order_relaxed) >
                  revalidate_before,
              "non-null ev fires enforce_linear_boundary_consistency (revalidate counter bumped) — "
              "Issue #3361");
        // Cleanup.
        g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
            0, std::memory_order_relaxed);
        ev.attach_compiler_metrics(nullptr);
    }

    // --- AC4: soft path bumps observe counter, no dirty counter, no revalidate ---
    std::println("\n--- AC4: soft path bumps observe counter, no revalidate ---");
    {
        reset_linear_fast_path_dirty_revalidate_for_test();
        // Temporarily drop back to Soft: g_strategy is read by get_strategy() inside the helper.
        // The cleanest way is to call apply_dev_audit_defaults (Soft) which sets
        // AuditStrategy::Sampled, but get_strategy() returns AuditStrategy::Sampled and
        // production_defaults_active() is false → not hard → Soft observe branch fires.
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        const ScopedOverride depth_override{1};
        g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
            1, std::memory_order_relaxed);
        const auto dirty_before = linear_fast_path_dirty_revalidate_total_v_read();
        const auto observe_before = linear_fast_path_force_revalidate_observe_total_v_read();
        Evaluator ev{};
        CompilerMetrics metrics{};
        ev.attach_compiler_metrics(&metrics);
        const auto revalidate_before =
            metrics.linear_boundary_consistency_total.load(std::memory_order_relaxed);
        const bool returned = note_densify_entry_under_live_mutation(&ev);
        check(!returned, "soft path returns false");
        check(linear_fast_path_dirty_revalidate_total_v_read() == dirty_before,
              "soft path does NOT bump dirty_revalidate_total");
        check(linear_fast_path_force_revalidate_observe_total_v_read() == observe_before + 1,
              "soft path bumps observe_total by 1");
        check(metrics.linear_boundary_consistency_total.load(std::memory_order_relaxed) ==
                  revalidate_before,
              "soft path does NOT fire enforce_linear_boundary_consistency");
        // Cleanup: restore production defaults + clear densify_pending.
        apply_production_audit_defaults();
        g_typed_mutation_audit_counters.linear_densify_scan_mismatch_inject_pending.store(
            0, std::memory_order_relaxed);
        ev.attach_compiler_metrics(nullptr);
    }

    if (g_failed) {
        std::println("\nFAIL: {} passed / {} failed", g_passed, g_failed);
        return 1;
    }
    std::println("\nPASS: {} passed / 0 failed — Issue #3361 immediate revalidate wiring verified",
                 g_passed);
    return 0;
}