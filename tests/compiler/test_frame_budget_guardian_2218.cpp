// @category: unit
// @reason: Issue #2218 — frame-budget guardian + automatic degrade on present.
//
//   AC1: Budget config (default 16000 µs; env AURA_FRAME_BUDGET_US; soft 80%)
//   AC2: Guardian on tui:present / tui:present-dirty → degrade + skip full
//   AC3: Forces agent-action hold/stop; query:render-stats reflects it
//   AC4: Counters + schema-2218 on query:render-stats
//   AC5: Over-budget → degrade; under-budget → no degrade; stress engages

#include "test_harness.hpp"

#include "compiler/frame_budget.hh"
#include "compiler/observability_metrics.h"
#include "renderer/render_frame_arena.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::frame_budget::check_present_guardian;
using aura::compiler::frame_budget::clear_forced_agent_action;
using aura::compiler::frame_budget::effective_agent_action;
using aura::compiler::frame_budget::kDefaultBudgetUs;
using aura::compiler::frame_budget::kPresentGuardianIssue;
using aura::compiler::frame_budget::kSoftMarginBp;
using aura::compiler::frame_budget::note_agent_closed_loop;
using aura::compiler::frame_budget::reset_for_test;
using aura::compiler::frame_budget::set_budget_us;
using aura::compiler::frame_budget::snapshot;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::renderer::record_render_frame_time_us;
using aura::renderer::reset_render_frame_metrics_for_test;
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

int main() {
    std::println("=== Issue #2218: frame-budget present guardian ===");
    CHECK(kPresentGuardianIssue == 2218, "issue stamp");
    CHECK(kDefaultBudgetUs == 16000, "AC1: default 16000 µs");
    CHECK(kSoftMarginBp == 8000, "AC1: soft margin 80%");

    // ── AC source ──
    {
        std::println("\n--- AC source ---");
        auto fb = read_file("src/compiler/frame_budget.hh");
        auto tui = read_file("src/compiler/evaluator_primitives_tui.cpp");
        auto tpl = read_file("src/compiler/render_prim_template.hh");
        auto met = read_file("src/compiler/observability_metrics.h");
        auto io = read_file("src/compiler/evaluator_primitives_io.cpp");
        CHECK(fb.find("#2218") != std::string::npos, "frame_budget.hh #2218");
        CHECK(fb.find("check_present_guardian") != std::string::npos, "guardian API");
        CHECK(fb.find("AURA_FRAME_BUDGET_US") != std::string::npos, "env budget");
        CHECK(fb.find("frame_budget_degrade_total") != std::string::npos, "degrade counter");
        CHECK(tui.find("check_present_guardian") != std::string::npos, "TUI wires guardian");
        CHECK(tui.find("tui:present") != std::string::npos, "present retained");
        CHECK(tui.find("tui:present-dirty") != std::string::npos, "present-dirty retained");
        CHECK(tpl.find("#2218") != std::string::npos, "template docs");
        CHECK(met.find("frame_budget_degrade_total") != std::string::npos, "CompilerMetrics");
        CHECK(io.find("schema-2218") != std::string::npos, "query schema-2218");
        CHECK(io.find("note_agent_closed_loop") != std::string::npos, "AC3 stash health");
    }

    reset_for_test();
    reset_render_frame_metrics_for_test();
    set_budget_us(16000);
    note_agent_closed_loop(100, /*optimize-ok*/ 1);
    clear_forced_agent_action();

    // ── AC1 / AC5 under-budget: no degrade ──
    {
        std::println("\n--- AC5: under-budget no degrade ---");
        reset_for_test();
        set_budget_us(16000);
        note_agent_closed_loop(100, 1);
        clear_forced_agent_action();
        // Inject light samples
        for (int i = 0; i < 16; ++i)
            record_render_frame_time_us(2000);
        const auto d = check_present_guardian(2000, /*dirty*/ 0, /*total*/ 1000);
        CHECK(!d.degrade, "under budget: no degrade");
        CHECK(!d.skip_full_rebuild || !d.degrade, "no skip without degrade");
        CHECK(snapshot().frame_budget_check_total >= 1, "check_total bumped");
        CHECK(snapshot().frame_budget_degrade_total == 0, "degrade_total stays 0");
    }

    // ── AC2 / AC5: over-budget → degrade + skip full when clean/tiny ──
    {
        std::println("\n--- AC2/AC5: over-budget degrade ---");
        reset_for_test();
        set_budget_us(16000);
        note_agent_closed_loop(90, 1);
        clear_forced_agent_action();
        for (int i = 0; i < 32; ++i)
            record_render_frame_time_us(25000); // p99 > 16ms
        const auto p99 = aura::renderer::render_frame_time_p99_us();
        CHECK(p99 > 16000, "synthetic p99 over budget");
        const auto d = check_present_guardian(p99, /*dirty*/ 0, /*total*/ 100);
        CHECK(d.degrade, "over budget: degrade");
        CHECK(d.skip_full_rebuild, "clean dirty → skip full rebuild");
        CHECK(snapshot().frame_budget_degrade_total >= 1, "degrade_total +1");
        CHECK(snapshot().frame_budget_last_p99_us == p99, "last_p99 recorded");
        CHECK(snapshot().forced_agent_action == 4 || snapshot().forced_agent_action == 0,
              "forced hold or stop");
        // Tiny dirty still skips full
        const auto d2 = check_present_guardian(p99, /*dirty*/ 4, /*total*/ 1000);
        CHECK(d2.degrade && d2.skip_full_rebuild, "tiny dirty → skip full");
        // Large dirty under degrade: still degrade but may not skip full
        const auto d3 = check_present_guardian(p99, /*dirty*/ 500, /*total*/ 1000);
        CHECK(d3.degrade, "large dirty still degrades");
        CHECK(!d3.skip_full_rebuild, "large dirty: allow present of content");
    }

    // ── AC2: low health / hold action triggers degrade without high p99 ──
    {
        std::println("\n--- AC2: health / action trigger ---");
        reset_for_test();
        set_budget_us(16000);
        note_agent_closed_loop(30, 1); // low health
        clear_forced_agent_action();
        const auto d = check_present_guardian(/*p99*/ 1000, 0, 100);
        CHECK(d.degrade, "low health degrades");
        note_agent_closed_loop(90, 0); // hold
        clear_forced_agent_action();
        const auto d2 = check_present_guardian(1000, 0, 100);
        CHECK(d2.degrade, "hold action degrades");
    }

    // ── Soft warn at 80% ──
    {
        std::println("\n--- AC1: soft margin ---");
        reset_for_test();
        set_budget_us(16000);
        note_agent_closed_loop(100, 1);
        clear_forced_agent_action();
        // 80% of 16000 = 12800; 13000 soft but not over
        const auto d = check_present_guardian(13000, 0, 100);
        CHECK(!d.degrade, "soft only: not degrade");
        CHECK(d.soft_warn, "soft_warn at ≥80%");
        CHECK(snapshot().frame_budget_soft_warn_total >= 1, "soft_warn counter");
    }

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: query:render-stats schema-2218 ---");
        CHECK(href(cs, "schema-2218") == 2218, "schema-2218");
        CHECK(href(cs, "issue-2218") == 2218, "issue-2218");
        CHECK(href(cs, "frame-budget-guardian-wired") == 1, "wired");
        CHECK(href(cs, "frame-budget-us") == 16000 || href(cs, "frame-budget-us") > 0,
              "budget-us exposed");
        CHECK(href(cs, "frame-budget-check-total") >= 0, "check-total key");
        CHECK(href(cs, "frame-budget-degrade-total") >= 0, "degrade-total key");
        CHECK(href(cs, "frame-budget-last-p99-us") >= 0, "last-p99 key");
    }

    // ── AC2/AC3: present path under synthetic over-budget ──
    {
        std::println("\n--- AC2/AC3: tui present under over-budget ---");
        reset_for_test();
        reset_render_frame_metrics_for_test();
        set_budget_us(16000);
        note_agent_closed_loop(100, 1);
        clear_forced_agent_action();
        for (int i = 0; i < 40; ++i)
            record_render_frame_time_us(30000);

        const auto deg0 = snapshot().frame_budget_degrade_total;
        const auto chk0 = snapshot().frame_budget_check_total;
        auto p = cs.eval("(tui:present)");
        CHECK(p.has_value(), "tui:present callable under pressure");
        auto d = cs.eval("(tui:present-dirty)");
        CHECK(d.has_value(), "tui:present-dirty callable under pressure");
        CHECK(snapshot().frame_budget_check_total > chk0, "present bumped check_total");
        CHECK(snapshot().frame_budget_degrade_total > deg0, "present took degrade path");

        // AC3: agent-action forced via query
        const auto action = href(cs, "agent-action");
        CHECK(action == 0 || action == 4, "AC3: agent-action hold/stop after degrade");
        CHECK(href(cs, "frame-budget-forced-action") == 0 ||
                  href(cs, "frame-budget-forced-action") == 4,
              "forced-action exposed");
        CHECK(effective_agent_action(1) == 0 || effective_agent_action(1) == 4,
              "effective_agent_action forces");
        CHECK(href(cs, "safe-to-mutate") == 0, "AC3: safe-to-mutate 0 under forced hold/stop");
    }

    // ── AC5: under-budget present after recover ──
    {
        std::println("\n--- AC5: recover under budget ---");
        reset_for_test();
        reset_render_frame_metrics_for_test();
        set_budget_us(16000);
        note_agent_closed_loop(100, 1);
        clear_forced_agent_action();
        for (int i = 0; i < 16; ++i)
            record_render_frame_time_us(1000);
        const auto deg0 = snapshot().frame_budget_degrade_total;
        (void)cs.eval("(tui:present)");
        (void)cs.eval("(tui:present-dirty)");
        // May still have checks but degrade should not rise from under-budget alone
        // (prior forced cleared; health ok)
        CHECK(snapshot().frame_budget_degrade_total == deg0 ||
                  snapshot().frame_budget_degrade_total == deg0 + 0,
              "under-budget present: no new degrade");
        CHECK(href(cs, "schema-2218") == 2218, "schema stable");
    }

    // ── Stress: continuous over-budget presents keep degrade engaged ──
    {
        std::println("\n--- AC5: stress degrade engages ---");
        reset_for_test();
        reset_render_frame_metrics_for_test();
        set_budget_us(5000); // tight budget
        note_agent_closed_loop(100, 1);
        clear_forced_agent_action();
        for (int i = 0; i < 20; ++i)
            record_render_frame_time_us(12000);
        for (int i = 0; i < 8; ++i)
            (void)cs.eval("(tui:present-dirty)");
        CHECK(snapshot().frame_budget_degrade_total > 0, "stress: degrade rate > 0");
        CHECK(snapshot().frame_budget_check_total >= 8, "stress: checks ≥ presents");
    }

    std::println("\n=== #2218 done: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
