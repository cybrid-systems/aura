// @category: unit
// @reason: Issue #2137 — frame-budget cascade isolation: protect present
// from non-render dirty/relower/hot-update work while hotpath is held.
//
//   AC1: under FrameBudget / render hotpath, non-render cascade deferred
//   AC2: render-related cascade still proceeds; metrics move
//   AC3: deferred work drains after exit (eventual run / coalesce)
//   AC4: present hold histogram + p99 under cascade load
//   AC5: schema-2137 on query:render-stats; source cites #2137
//   AC6: concurrent non-render dirty + present loop → deferred counters

#include "test_harness.hpp"

#include "compiler/frame_budget.hh"
#include "compiler/observability_metrics.h"
#include "core/arena_auto_policy_stats.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::frame_budget::active;
using aura::compiler::frame_budget::defer_cascade;
using aura::compiler::frame_budget::deferred_pending;
using aura::compiler::frame_budget::drain_deferred;
using aura::compiler::frame_budget::enter;
using aura::compiler::frame_budget::exit;
using aura::compiler::frame_budget::FrameBudgetGuard;
using aura::compiler::frame_budget::is_render_related_name;
using aura::compiler::frame_budget::kDefaultBudgetUs;
using aura::compiler::frame_budget::kFrameBudgetIssue;
using aura::compiler::frame_budget::present_p99_us;
using aura::compiler::frame_budget::reset_for_test;
using aura::compiler::frame_budget::should_defer_cascade;
using aura::compiler::frame_budget::snapshot;
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
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:render-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_frame_budget_cascade_isolation_2137() {
    std::println("=== Issue #2137: frame-budget cascade isolation ===");
    CHECK(kFrameBudgetIssue == 2137, "issue stamp");
    CHECK(kDefaultBudgetUs == 16000, "default ~60fps budget us (#2218 AC1)");

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source ---");
        auto fb = read_file("src/compiler/frame_budget.hh");
        auto sd = read_file("src/compiler/service_dirty.cpp");
        auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        auto met = read_file("src/compiler/observability_metrics.h");
        CHECK(fb.find("#2137") != std::string::npos, "frame_budget.hh #2137");
        CHECK(fb.find("should_defer_cascade") != std::string::npos, "should_defer_cascade");
        CHECK(fb.find("deferred_cascade_total") != std::string::npos, "deferred metric");
        CHECK(sd.find("#2137") != std::string::npos, "service_dirty #2137");
        CHECK(sd.find("flush_frame_budget_deferred_") != std::string::npos, "flush");
        CHECK(emb.find("frame_budget::enter") != std::string::npos, "enter on hotpath");
        CHECK(met.find("frame_budget_deferred_cascade_total") != std::string::npos, "metrics");
        CHECK(met.find("present_p99_under_cascade_us") != std::string::npos, "p99 metric");
    }

    // ── AC1 / AC2: defer non-render; allow render ──
    {
        std::println("\n--- AC1/AC2: defer vs allow ---");
        reset_for_test();
        CHECK(!should_defer_cascade("helper"), "no defer outside budget");
        CHECK(is_render_related_name("terminal-present-batch"), "present is render");
        CHECK(is_render_related_name("tui:draw-batch"), "tui is render");
        CHECK(!is_render_related_name("helper-fn"), "helper not render");
        CHECK(!is_render_related_name("business-logic"), "business not render");

        {
            FrameBudgetGuard g;
            CHECK(active(), "budget active under guard");
            CHECK(should_defer_cascade("helper-fn"), "non-render deferred under budget");
            CHECK(!should_defer_cascade("draw-cell"), "draw allowed");
            CHECK(!should_defer_cascade("present-frame"), "present allowed");
            defer_cascade("helper-fn");
            defer_cascade("helper-fn"); // coalesce
            defer_cascade("other-logic");
            CHECK(deferred_pending() == 2, "coalesced pending=2");
        }
        CHECK(!active(), "budget inactive after guard");
        const auto snap = snapshot();
        CHECK(snap.deferred_cascade_total >= 2, "deferred_cascade_total");
        CHECK(snap.deferred_coalesce_hits >= 1, "coalesce hits");
        CHECK(snap.hold_samples >= 1, "hold sample recorded");
        auto drained = drain_deferred();
        CHECK(drained.size() == 2, "drain returns 2 names");
        CHECK(deferred_pending() == 0, "pending empty after drain");
        CHECK(snapshot().flush_total >= 1, "flush_total");
    }

    // ── AC3: mark_define_dirty defers under hotpath; drains after ──
    {
        std::println("\n--- AC3: service cascade defer + drain ---");
        reset_for_test();
        CompilerService cs;
        // Seed two defines via set-code so ir_cache entries exist.
        (void)cs.eval(R"((begin
            (define helper-fn (lambda () 1))
            (define draw-frame (lambda () 2))
            0))");
        // Enter render hotpath via evaluator (arms frame budget).
        cs.evaluator().enter_render_hotpath();
        CHECK(cs.evaluator().in_render_hotpath(), "in hotpath");
        const auto def_before = snapshot().deferred_cascade_total;
        cs.public_mark_define_dirty("helper-fn");
        // Non-render should defer — no crash, pending grows.
        CHECK(deferred_pending() >= 1 || snapshot().deferred_cascade_total > def_before,
              "helper deferred under hotpath");
        // Render-related should proceed (or at least not force-defer).
        cs.public_mark_define_dirty("draw-frame");
        cs.evaluator().exit_render_hotpath();
        // Drain deferred work.
        cs.public_flush_frame_budget_deferred();
        CHECK(deferred_pending() == 0, "flushed pending");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        if (m) {
            CHECK(m->frame_budget_wired.load() == 1, "wired");
        }
    }

    // ── AC4: present hold histogram under synthetic load ──
    {
        std::println("\n--- AC4: present p99 under cascade ---");
        reset_for_test();
        CompilerService cs;
        (void)cs.eval(R"((begin
            (define noise-a (lambda () 0))
            (define noise-b (lambda () 0))
            (define present-main (lambda () 0))
            0))");
        // Tight present loop with concurrent non-render dirty storms.
        for (int i = 0; i < 40; ++i) {
            cs.evaluator().enter_render_hotpath();
            // Storm non-render dirty while hotpath held.
            for (int j = 0; j < 8; ++j) {
                cs.public_mark_define_dirty("noise-a");
                cs.public_mark_define_dirty("noise-b");
            }
            // Small hold to simulate present work.
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            cs.evaluator().exit_render_hotpath();
            cs.public_flush_frame_budget_deferred();
        }
        const auto p99 = present_p99_us();
        const auto snap = snapshot();
        std::println("  deferred={} coalesce={} p99_us={} max_us={} hold_samples={}",
                     snap.deferred_cascade_total, snap.deferred_coalesce_hits, p99,
                     snap.present_max_us, snap.hold_samples);
        CHECK(snap.deferred_cascade_total > 0, "deferred under mixed load");
        CHECK(snap.hold_samples >= 40, "hold samples for each present");
        // p99 should be well under a 16.6ms frame in this synthetic harness.
        CHECK(p99 < 16000, "p99 under 16ms budget envelope");
        CHECK(href(cs, "schema-2137") == 2137, "schema-2137");
        CHECK(href(cs, "frame-budget-wired") == 1, "query wired");
        CHECK(href(cs, "frame-budget-deferred-cascade-total") >= 0, "query deferred");
    }

    // ── AC6: direct budget guard API ──
    {
        std::println("\n--- AC6: FrameBudgetGuard API ---");
        reset_for_test();
        {
            FrameBudgetGuard g(5000); // 5ms budget
            CHECK(active(), "active");
            for (int i = 0; i < 20; ++i)
                defer_cascade("batch-noise");
            CHECK(deferred_pending() == 1, "coalesce to one name");
        }
        CHECK(snapshot().deferred_coalesce_hits >= 19, "many coalesce hits");
        (void)drain_deferred();
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_frame_budget_cascade_isolation_2137();
}
#endif
