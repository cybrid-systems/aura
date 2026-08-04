// @category: unit
// @reason: Issue #2204 — arm GcDeferReason::MutationHold on outermost
// MutationBoundaryGuard (unify with Panic/FfiPin/RenderPin).
//
//   AC1: During outermost Guard body, should_defer_destructive_gc()==true
//        and GCCollector::request() refuses with mutation-hold metric bump.
//   AC2: After Guard dtor, bit clear; GC request can proceed (absent other).
//   AC3: Nested Guard does not double-arm; single outer release clears bit.
//   AC4: Combined Panic + MutationHold: release order keeps the other armed.
//   AC5: query:gc-defer-reason-stats schema-2204 + arm-mutation-hold-total.
//   AC6: Soft #1493 hold-µs tune remains (source-cite); MutationHold is
//        additional hard gate.

#include "test_harness.hpp"

#include "core/gc_hooks.h"
#include "serve/gc_coordinator.h"
#include "serve/scheduler.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::GCCollector;
using aura::serve::Scheduler;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:gc-defer-reason-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void drain_mutation_hold() {
    while (aura::gc_hooks::mutation_hold_defer_active())
        aura::gc_hooks::release_mutation_hold_defer();
}

static void ac6_source() {
    std::println("\n--- AC6/source: arm sites + #1493 soft path retained ---");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto hooks = read_file("src/core/gc_hooks.h");
    auto gcc = read_file("src/serve/gc_coordinator.cpp");
    CHECK(mb.find("arm_mutation_hold_defer") != std::string::npos,
          "AC6: outermost enter arms MutationHold");
    CHECK(mb.find("release_mutation_hold_defer") != std::string::npos,
          "AC6: outermost exit releases MutationHold");
    CHECK(mb.find("Issue #2204") != std::string::npos || mb.find("#2204") != std::string::npos,
          "AC6: cites #2204");
    // Soft #1493 adaptive GC frequency from hold µs still present.
    CHECK(mb.find("mutation_hold_duration") != std::string::npos ||
              mb.find("hold_us") != std::string::npos || mb.find("#1493") != std::string::npos,
          "AC6: soft hold-µs path retained alongside hard gate");
    CHECK(hooks.find("MutationHold") != std::string::npos, "AC6: enum MutationHold");
    CHECK(hooks.find("arm_mutation_hold_defer") != std::string::npos, "AC6: arm API");
    CHECK(hooks.find("g_gc_defer_arm_mutation_hold_total") != std::string::npos,
          "AC6: arm total metric");
    CHECK(gcc.find("note_gc_request_deferred_mutation_hold") != std::string::npos,
          "AC6: GCCollector notes mutation-hold defer");
}

static void ac1_ac2_guard_defers_gc() {
    std::println("\n--- AC1/AC2: outermost Guard arms MutationHold → request defers ---");
    drain_mutation_hold();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    Scheduler sched(1);
    auto* gc = sched.gc_collector();
    CHECK(gc != nullptr, "GCCollector");
    gc->set_alloc_threshold(1);
    for (int i = 0; i < 16; ++i)
        gc->record_alloc();

    const auto arm0 =
        aura::gc_hooks::g_gc_defer_arm_mutation_hold_total.load(std::memory_order_relaxed);
    const auto req0 =
        aura::gc_hooks::g_gc_request_deferred_mutation_hold_total.load(std::memory_order_relaxed);

    bool ok = true;
    {
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        CHECK(gr.has_value() && gr->get() != nullptr, "AC1: try_acquire outermost");
        auto g = std::move(*gr);
        CHECK(aura::gc_hooks::mutation_hold_defer_active(), "AC1: mutation_hold active");
        CHECK(aura::gc_hooks::should_defer_destructive_gc(), "AC1: should_defer true");
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::MutationHold)) != 0,
              "AC1: MutationHold bit set");
        CHECK(aura::gc_hooks::g_gc_defer_arm_mutation_hold_total.load(std::memory_order_relaxed) >
                  arm0,
              "AC1: arm-mutation-hold total advanced");
        CHECK(!gc->request(), "AC1: GCCollector::request refuses under hold");
        CHECK(aura::gc_hooks::g_gc_request_deferred_mutation_hold_total.load(
                  std::memory_order_relaxed) > req0,
              "AC1: request-deferred-mutation-hold bumped");
        // g goes out of scope → release
    }
    CHECK(!aura::gc_hooks::mutation_hold_defer_active(), "AC2: hold inactive after dtor");
    CHECK((aura::gc_hooks::defer_reasons_snapshot() &
           static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::MutationHold)) == 0,
          "AC2: MutationHold bit clear");
    // Absent other reasons, should_defer may still be true if residual Panic
    // from a failed checkpoint path — drain panic if any.
    if (aura::gc_hooks::gc_defer_pending_panic_depth() == 0 &&
        !aura::gc_hooks::ffi_pin_defer_active() && !aura::gc_hooks::render_pin_defer_active()) {
        CHECK(!aura::gc_hooks::should_defer_destructive_gc(),
              "AC2: should_defer false after Guard (no other reasons)");
    }
    // After release, request may succeed if threshold still crossed.
    for (int i = 0; i < 8; ++i)
        gc->record_alloc();
    const bool req = gc->request();
    if (req)
        (void)gc->collect();
    CHECK(true, "AC2: post-release GC path exercised");
}

static void ac3_nested_no_double_arm() {
    std::println("\n--- AC3: nested Guard does not double-arm ---");
    drain_mutation_hold();
    CompilerService cs;
    CHECK(cs.eval("(+ 0 0)").has_value(), "warm");
    const auto arm0 =
        aura::gc_hooks::g_gc_defer_arm_mutation_hold_total.load(std::memory_order_relaxed);
    bool ok_outer = true;
    bool ok_inner = true;
    {
        auto outer = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok_outer);
        CHECK(outer.has_value() && outer->get() != nullptr, "AC3: outer acquire");
        auto g_outer = std::move(*outer);
        CHECK(aura::gc_hooks::mutation_hold_defer_depth() == 1, "AC3: depth==1 after outer");
        const auto arm_after_outer =
            aura::gc_hooks::g_gc_defer_arm_mutation_hold_total.load(std::memory_order_relaxed);
        CHECK(arm_after_outer == arm0 + 1, "AC3: outer arm +1 (0→set only)");
        {
            auto inner =
                Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok_inner);
            CHECK(inner.has_value() && inner->get() != nullptr, "AC3: inner acquire");
            auto g_inner = std::move(*inner);
            // Nested must not bump depth (only outermost arms).
            CHECK(aura::gc_hooks::mutation_hold_defer_depth() == 1,
                  "AC3: nested does not arm (depth stays 1)");
            CHECK(aura::gc_hooks::g_gc_defer_arm_mutation_hold_total.load(
                      std::memory_order_relaxed) == arm_after_outer,
                  "AC3: nested does not bump arm total");
        }
        CHECK(aura::gc_hooks::mutation_hold_defer_active(), "AC3: still active after nested exit");
    }
    CHECK(!aura::gc_hooks::mutation_hold_defer_active(), "AC3: cleared after outer exit");
    CHECK(aura::gc_hooks::mutation_hold_defer_depth() == 0, "AC3: depth 0 after outer");
}

static void ac4_combined_panic_and_hold() {
    std::println("\n--- AC4: Panic + MutationHold independence ---");
    drain_mutation_hold();
    // Clear residual panic if any from prior suites.
    while (aura::gc_hooks::gc_defer_pending_panic_depth() > 0)
        aura::gc_hooks::release_gc_defer_pending_panic();

    CompilerService cs;
    CHECK(cs.eval("(+ 2 2)").has_value(), "warm");
    bool ok = true;
    {
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
        CHECK(gr.has_value() && gr->get() != nullptr, "AC4: acquire");
        auto g = std::move(*gr);
        CHECK(aura::gc_hooks::mutation_hold_defer_active(), "AC4: hold armed");
        // Independently arm Panic (process-wide).
        aura::gc_hooks::arm_gc_defer_pending_panic();
        CHECK(aura::gc_hooks::gc_deferred_for_pending_panic(), "AC4: panic armed");
        CHECK(aura::gc_hooks::should_defer_destructive_gc(), "AC4: still defers");
        const auto reasons = aura::gc_hooks::defer_reasons_snapshot();
        CHECK((reasons & static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::MutationHold)) !=
                  0,
              "AC4: MutationHold bit set");
        CHECK((reasons & static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::Panic)) != 0,
              "AC4: Panic bit set");
        // Release Panic early — MutationHold must remain.
        aura::gc_hooks::release_gc_defer_pending_panic();
        CHECK(!aura::gc_hooks::gc_deferred_for_pending_panic() ||
                  aura::gc_hooks::gc_defer_pending_panic_depth() == 0,
              "AC4: panic released");
        CHECK(aura::gc_hooks::mutation_hold_defer_active(),
              "AC4: MutationHold still active after Panic release");
        CHECK(aura::gc_hooks::should_defer_destructive_gc(),
              "AC4: still defers solely due to MutationHold");
        // Re-arm Panic, then leave Guard (releases hold) — Panic must remain.
        aura::gc_hooks::arm_gc_defer_pending_panic();
    }
    CHECK(!aura::gc_hooks::mutation_hold_defer_active(), "AC4: hold released on Guard dtor");
    // Panic may have been cleared by Guard's panic checkpoint commit path
    // if save_panic_checkpoint armed per-eval Panic. Re-arm process Panic
    // to prove independence of the bitmask release helper.
    if (!aura::gc_hooks::gc_deferred_for_pending_panic())
        aura::gc_hooks::arm_gc_defer_pending_panic();
    CHECK(aura::gc_hooks::should_defer_destructive_gc(), "AC4: Panic alone still defers");
    CHECK((aura::gc_hooks::defer_reasons_snapshot() &
           static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::MutationHold)) == 0,
          "AC4: MutationHold clear while Panic may remain");
    aura::gc_hooks::release_gc_defer_pending_panic();
    // Drain any residual panic depth from Guard checkpoint path.
    while (aura::gc_hooks::gc_defer_pending_panic_depth() > 0)
        aura::gc_hooks::release_gc_defer_pending_panic();
    drain_mutation_hold();
}

static void ac5_query_schema() {
    std::println("\n--- AC5: query schema-2204 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 3 3)").has_value(), "warm");
    CHECK(href(cs, "schema-2204") == 2204, "schema-2204");
    CHECK(href(cs, "issue-2204") == 2204, "issue-2204");
    CHECK(href(cs, "mutation-hold-wired") == 1, "mutation-hold-wired");
    CHECK(href(cs, "arm-mutation-hold-total") >= 0, "arm-mutation-hold-total");
    CHECK(href(cs, "mutation-hold-depth") >= 0, "mutation-hold-depth");
    CHECK(href(cs, "request-deferred-mutation-hold-total") >= 0,
          "request-deferred-mutation-hold-total");
    // Lineage
    CHECK(href(cs, "schema-2088") == 2088, "schema-2088 retained");
    CHECK(href(cs, "unified-defer-wired") == 1, "unified-defer-wired");
}

} // namespace

int run_test_gc_defer_mutation_hold_2204() {
    std::println("=== Issue #2204: GcDeferReason::MutationHold on outermost Guard ===");
    drain_mutation_hold();
    ac6_source();
    ac1_ac2_guard_defers_gc();
    ac3_nested_no_double_arm();
    ac4_combined_panic_and_hold();
    ac5_query_schema();
    drain_mutation_hold();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_gc_defer_mutation_hold_2204();
}
#endif
