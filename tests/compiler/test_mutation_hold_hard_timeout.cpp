// @category: unit
// @reason: Issue #2199 — hard timeout / force-fail for long outermost
// MutationBoundary holds under strict mode.
//
//   AC1: Strict on + synthetic long mutate → outermost exit fails,
//        abort metric ≥1 (success=false before exit_mutation_boundary)
//   AC2: Strict off → no force-fail; too_long / histogram still bump
//   AC3: Nested (non-outermost) guards never force-fail independently
//   AC4: Scheduler hook still fires on too_long
//   AC5: Force-fail does not unlock early (ordering retained in source)
//   AC6: query:mutation-boundary-hold-stats schema-2199 keys

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/aura_jit_bridge.h"

#include <chrono>
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

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-boundary-hold-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void spin_us(std::int64_t min_us) {
    auto t0 = std::chrono::steady_clock::now();
    while (
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count() < min_us) {
    }
}

static std::atomic<std::uint64_t> g_hook_calls{0};
static void test_scheduler_hook(std::uint64_t /*fiber_id*/, std::uint64_t /*duration_us*/) {
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
}

// ── AC1: Strict force-fail ──────────────────────────────────
static void ac1_strict_force_fail() {
    std::println("\n--- AC1: Strict + long hold → force-fail / abort metric ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    // Lower ceilings so spin is cheap: threshold 500µs, hard 2ms.
    metrics.long_mutation_threshold_us.store(500, std::memory_order_relaxed);
    metrics.max_extreme_mutation_us.store(2'000, std::memory_order_relaxed);
    metrics.hard_timeout_us.store(2'000, std::memory_order_relaxed);
    metrics.long_mutation_strict_mode.store(1, std::memory_order_relaxed);

    const auto abort0 = metrics.long_mutation_forced_abort_total.load();
    const auto extreme0 = metrics.long_mutation_extreme_total.load();
    const auto too_long0 = metrics.mutation_too_long_total.load();
    const auto rollback0 = metrics.mutation_boundary_rollbacks_total.load();

    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(5'000); // > hard_timeout 2ms
    }
    CHECK(!ok, "AC1: flag forced false on outermost exit");
    CHECK(metrics.long_mutation_forced_abort_total.load() > abort0, "forced_abort_total ≥1");
    CHECK(metrics.long_mutation_extreme_total.load() > extreme0, "extreme total bumped");
    CHECK(metrics.mutation_too_long_total.load() > too_long0, "too_long bumped");
    // Rollback path: bump_mutation_boundary_rollback on outermost !success
    CHECK(metrics.mutation_boundary_rollbacks_total.load() > rollback0,
          "rollback counter bumped (success=false path)");

    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC2: Soft no force-fail ─────────────────────────────────
static void ac2_soft_no_force_fail() {
    std::println("\n--- AC2: Strict off → no force-fail; metrics still bump ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.long_mutation_threshold_us.store(500, std::memory_order_relaxed);
    metrics.max_extreme_mutation_us.store(2'000, std::memory_order_relaxed);
    metrics.hard_timeout_us.store(2'000, std::memory_order_relaxed);
    metrics.long_mutation_strict_mode.store(0, std::memory_order_relaxed); // Soft

    const auto abort0 = metrics.long_mutation_forced_abort_total.load();
    const auto too_long0 = metrics.mutation_too_long_total.load();
    const auto holds0 = metrics.mutation_boundary_holds_total.load();

    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(5'000);
    }
    CHECK(ok, "AC2: Soft does not force-fail (flag stays true)");
    CHECK(metrics.long_mutation_forced_abort_total.load() == abort0, "no forced abort");
    CHECK(metrics.mutation_too_long_total.load() > too_long0, "too_long still bumps");
    CHECK(metrics.mutation_boundary_holds_total.load() > holds0, "hold sample recorded");
    // Histogram sum matches holds
    std::uint64_t hist = 0;
    for (std::size_t i = 0; i < CompilerMetrics::kMutationBoundaryHoldHistBuckets; ++i)
        hist += metrics.mutation_boundary_hold_histogram[i].load();
    CHECK(hist == metrics.mutation_boundary_holds_total.load(), "histogram integrity");

    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC3: nested never independent force-fail ────────────────
static void ac3_nested_no_independent_fail() {
    std::println("\n--- AC3: nested Guard does not force-fail alone ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.long_mutation_threshold_us.store(500, std::memory_order_relaxed);
    metrics.hard_timeout_us.store(2'000, std::memory_order_relaxed);
    metrics.max_extreme_mutation_us.store(2'000, std::memory_order_relaxed);
    metrics.long_mutation_strict_mode.store(1, std::memory_order_relaxed);

    const auto abort0 = metrics.long_mutation_forced_abort_total.load();
    bool outer_ok = true;
    bool inner_ok = true;
    {
        Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &outer_ok);
        {
            Evaluator::MutationBoundaryGuard inner(cs.evaluator(), &inner_ok);
            spin_us(5'000); // long hold while nested
        }
        // After inner dtor: only outermost samples hold; inner must not
        // independently force-fail (inner has no enter_ts_ sample path).
        CHECK(inner_ok, "nested flag not independently force-failed");
    }
    // Outermost may force-fail if total hold exceeded hard timeout.
    CHECK(metrics.mutation_boundary_holds_total.load() == 1,
          "one outermost sample for nested pair");
    // If outer force-failed, abort bumped once (not twice).
    const auto abort_delta = metrics.long_mutation_forced_abort_total.load() - abort0;
    CHECK(abort_delta <= 1, "at most one forced abort (outermost only)");

    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC4: scheduler hook on too_long ─────────────────────────
static void ac4_scheduler_hook() {
    std::println("\n--- AC4: scheduler hook fires on too_long ---");
    g_hook_calls.store(0);
    aura_set_long_mutation_scheduler_hook(&test_scheduler_hook);

    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.long_mutation_threshold_us.store(500, std::memory_order_relaxed);
    metrics.long_mutation_strict_mode.store(0, std::memory_order_relaxed);
    metrics.max_extreme_mutation_us.store(30'000'000, std::memory_order_relaxed);

    {
        bool ok = true;
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(2'000); // > 500µs threshold
    }
    CHECK(g_hook_calls.load() >= 1, "AC4: on_long_mutation / hook fired");
    CHECK(metrics.mutation_too_long_total.load() >= 1, "too_long for hook path");

    aura_set_long_mutation_scheduler_hook(nullptr);
    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC5: no early unlock (source order) ─────────────────────
static void ac5_ordering() {
    std::println("\n--- AC5: force-fail before exit_mutation_boundary, unlock last ---");
    auto src = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(src.find("long_mutation_forced_abort_total") != std::string::npos, "abort metric");
    CHECK(src.find("success = false") != std::string::npos ||
              src.find("success=false") != std::string::npos,
          "re-syncs success before exit");
    CHECK(src.find("Issue #2199") != std::string::npos, "cites #2199");
    // force_fail block appears before exit_mutation_boundary call
    const auto ff = src.find("long_mutation_forced_abort_total");
    const auto exit = src.find("exit_mutation_boundary(success)");
    CHECK(ff != std::string::npos && exit != std::string::npos && ff < exit,
          "force-fail before exit_mutation_boundary");
    // Unlock remains last in outermost pipeline comments
    CHECK(src.find("unlock (LAST)") != std::string::npos ||
              src.find("unlock LAST") != std::string::npos ||
              src.find("phase5_unlock") != std::string::npos,
          "unlock-last ordering retained");
}

// ── AC6: query schema ───────────────────────────────────────
static void ac6_query_schema() {
    std::println("\n--- AC6: query schema-2199 ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.long_mutation_strict_mode.store(1, std::memory_order_relaxed);
    metrics.long_mutation_threshold_us.store(500, std::memory_order_relaxed);
    metrics.hard_timeout_us.store(1'000, std::memory_order_relaxed);
    {
        bool ok = true;
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        spin_us(3'000);
    }
    CHECK(href(cs, "schema-2199") == 2199, "schema-2199");
    CHECK(href(cs, "issue-2199") == 2199, "issue-2199");
    CHECK(href(cs, "hard-timeout-force-fail-wired") == 1, "wired");
    CHECK(href(cs, "long-mutation-forced-abort-total") >= 1, "abort total queryable");
    CHECK(href(cs, "long-mutation-strict-mode") == 1, "strict mode key");
    CHECK(href(cs, "hard-timeout-us") == 1000, "hard-timeout-us key");
    CHECK(href(cs, "mutation-too-long-total") >= 1, "too-long key");

    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(fields.find("long_mutation_forced_abort_total") != std::string::npos, "fields abort");
    CHECK(fields.find("hard_timeout_us") != std::string::npos, "fields hard_timeout");

    cs.evaluator().set_compiler_metrics(nullptr);
}

} // namespace

int run_test_mutation_hold_hard_timeout() {
    std::println("=== Issue #2199: MutationBoundary hard timeout / force-fail ===");
    ac1_strict_force_fail();
    ac2_soft_no_force_fail();
    ac3_nested_no_independent_fail();
    ac4_scheduler_hook();
    ac5_ordering();
    ac6_query_schema();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutation_hold_hard_timeout();
}
#endif
