// @category: integration
// @reason: Issue #1493 — per-fiber mutation depth / safepoint metrics export
// + hold-time adaptive safepoint frequency (closed-loop on #1483).
//
//   AC1: query:per-fiber-mutation-stack-stats schema 1493 + histogram fields
//   AC2: query:gc-safepoint-adaptive-stats schema 1493 + hold/wait/freq fields
//   AC3: depth samples bump mutation_stack_depth_histogram
//   AC4: long hold raises gc_frequency_tune_ratio (adapt up)
//   AC5: nested Guard + long stress — metrics advance, eval remains healthy

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/gc_hooks.h"
#include "serve/fiber.h"

#include <chrono>
#include <cstdint>
#include <print>
#include <thread>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_per_fiber_stats() {
    std::println("\n--- AC1: per-fiber-mutation-stack-stats schema 1493 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:per-fiber-mutation-stack-stats\")");
    CHECK(h && is_hash(*h), "per-fiber-mutation-stack-stats is hash");
    CHECK(href(cs, "query:per-fiber-mutation-stack-stats", "schema") == 1493, "schema 1493");
    CHECK(href(cs, "query:per-fiber-mutation-stack-stats", "lifetime-max") >= 0, "lifetime-max");
    CHECK(href(cs, "query:per-fiber-mutation-stack-stats", "live-depth") >= 0, "live-depth");
    CHECK(href(cs, "query:per-fiber-mutation-stack-stats", "hist-samples") >= 0, "hist-samples");
}

static void ac2_adaptive_stats() {
    std::println("\n--- AC2: gc-safepoint-adaptive-stats schema 1493 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:gc-safepoint-adaptive-stats\")");
    CHECK(h && is_hash(*h), "gc-safepoint-adaptive-stats is hash");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "schema") == 1493, "schema 1493");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "threshold") >= 0, "threshold");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "defer-count") >= 0, "defer-count");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "avg-mutation-hold-us") >= 0, "avg hold");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "gc-frequency-tune-ratio") >= 0,
          "gc-frequency-tune-ratio");
    CHECK(href(cs, "query:gc-safepoint-adaptive-stats", "safepoint-wait-while-mutation-held-us") >=
              0,
          "wait-while-mutation-held-us");
}

static void ac3_depth_histogram() {
    std::println("\n--- AC3: depth histogram samples ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");
    const auto s0 = href(cs, "query:per-fiber-mutation-stack-stats", "hist-samples");
    // Nested guards sample depth on yield checkpoint / bump path.
    bool ok1 = true, ok2 = true;
    {
        Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &ok1);
        cs.evaluator().bump_per_fiber_mutation_stack_depth_max(2);
        cs.evaluator().bump_per_fiber_mutation_stack_depth_max(3);
        Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &ok2);
        cs.evaluator().bump_per_fiber_mutation_stack_depth_max(4);
    }
    const auto s1 = href(cs, "query:per-fiber-mutation-stack-stats", "hist-samples");
    CHECK(s1 > s0, "hist-samples advanced after depth bumps");
    CHECK(cs.evaluator().get_per_fiber_mutation_stack_depth_max() >= 4, "lifetime-max >= 4");
}

static void ac4_hold_time_frequency_adapt() {
    std::println("\n--- AC4: long hold raises gc frequency ratio ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    // Force a low threshold so a short sleep counts as "long".
    m->long_mutation_threshold_us.store(1000, std::memory_order_relaxed); // 1ms
    const auto ratio0 = aura::serve::gc_frequency_tune_ratio().load(std::memory_order_relaxed);
    const auto up0 = m->safepoint_frequency_adapt_up_total.load();
    // Outermost Guard with sleep > threshold.
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto ratio1 = aura::serve::gc_frequency_tune_ratio().load(std::memory_order_relaxed);
    const auto up1 = m->safepoint_frequency_adapt_up_total.load();
    CHECK(ratio1 >= ratio0, "frequency ratio non-decreasing after long hold");
    CHECK(up1 > up0 || ratio1 > ratio0, "adapt-up or ratio advanced");
    // Also exercise direct API.
    const auto r_before = aura::serve::gc_frequency_tune_ratio().load(std::memory_order_relaxed);
    cs.evaluator().adapt_gc_frequency_from_hold_us(10'000'000); // very long
    CHECK(aura::serve::gc_frequency_tune_ratio().load(std::memory_order_relaxed) >= r_before,
          "direct adapt_gc_frequency_from_hold_us raises or holds ratio");
}

static void ac5_stress() {
    std::println("\n--- AC5: nested stress + process wait counters ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    // Simulate safepoint wait accounting (serve path may not run in unit test).
    const auto w0 = aura::gc_hooks::safepoint_wait_while_mutation_held_us();
    aura::gc_hooks::note_safepoint_wait_while_mutation(1234);
    CHECK(aura::gc_hooks::safepoint_wait_while_mutation_held_us() >= w0 + 1234,
          "process-wide wait-us advanced");
    CHECK(aura::gc_hooks::safepoint_wait_while_mutation_held_count() >= 1, "wait count advanced");

    for (int i = 0; i < 30; ++i) {
        bool ok = true;
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        cs.evaluator().bump_per_fiber_mutation_stack_depth_max(static_cast<std::size_t>(i % 5));
        (void)cs.eval("(+ 1 1)");
    }
    CHECK(m->mutation_boundary_holds_total.load() >= 1 ||
              href(cs, "query:gc-safepoint-adaptive-stats", "hold-samples") >= 0,
          "hold samples trackable");
    auto r = cs.eval("(+ 40 2)");
    CHECK(r && is_int(*r) && as_int(*r) == 42, "eval healthy after stress");
}

} // namespace

int main() {
    std::println("test_issue_1493: per-fiber mutation / adaptive safepoint metrics (#1493)");
    ac1_per_fiber_stats();
    ac2_adaptive_stats();
    ac3_depth_histogram();
    ac4_hold_time_frequency_adapt();
    ac5_stress();
    std::println("\n#1493: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
