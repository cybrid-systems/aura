// @category: unit
// @reason: Issue #2138 — evolvable RenderStrategy layer vs fixed native kernel.
//
//   AC1: strategy can be set/get; present path consults it
//   AC2: set-strategy bumps epoch (hot-replace without kernel edit)
//   AC3: Skip / Full / DirtyAABB change present behavior on same dirty
//   AC4: schema-2138 metrics (epoch, mode counts) on query:render-stats
//   AC5: source cites #2138; kernel present_batch still owns ANSI/ZC
//   AC6: mid-session swap → observable mode metric change

#include "test_harness.hpp"

#include "renderer/render_strategy.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::renderer::strategy::Mode;
using aura::renderer::strategy::reset_for_test;
using aura::renderer::strategy::set_strategy;
using aura::renderer::strategy::snapshot;
using aura::renderer::strategy::strategy_epoch;
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

// Present to /dev/null-style pipe; returns bytes (0 = skip).
static std::int64_t present_pipe(CompilerService& cs, std::int64_t bid) {
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    auto written = cs.eval(std::format("(terminal-present-batch {} {})", bid, pipefd[1]));
    ::close(pipefd[1]);
    char buf[4096];
    while (::read(pipefd[0], buf, sizeof(buf)) > 0) {
    }
    ::close(pipefd[0]);
    if (!written || !is_int(*written))
        return -1;
    return as_int(*written);
}

} // namespace

int main() {
    std::println("=== Issue #2138: RenderStrategy evolvable layer ===");
    CHECK(aura::renderer::strategy::kRenderStrategyIssue == 2138, "issue stamp");

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source ---");
        auto rs = read_file("src/renderer/render_strategy.hh");
        auto io = read_file("src/compiler/evaluator_primitives_io.cpp");
        auto tui = read_file("src/compiler/evaluator_primitives_tui.cpp");
        auto tpl = read_file("src/compiler/render_prim_template.hh");
        CHECK(rs.find("#2138") != std::string::npos, "render_strategy.hh");
        CHECK(rs.find("present_batch_with_strategy") != std::string::npos, "kernel entry");
        CHECK(rs.find("RenderStrategyView") != std::string::npos, "view");
        CHECK(io.find("render:set-strategy") != std::string::npos, "set-strategy prim");
        CHECK(io.find("present_batch_with_strategy") != std::string::npos, "terminal path");
        CHECK(tui.find("present_batch_with_strategy") != std::string::npos, "tui path");
        CHECK(tpl.find("#2138") != std::string::npos, "template docs");
    }

    // ── AC1 / AC2: set / get / epoch ──
    {
        std::println("\n--- AC1/AC2: set-strategy + epoch ---");
        reset_for_test();
        CompilerService cs;
        auto e0 = cs.eval("(render:strategy-epoch)");
        CHECK(e0 && is_int(*e0) && as_int(*e0) >= 1, "epoch registered");
        const auto epoch0 = as_int(*e0);
        auto m0 = cs.eval("(render:get-strategy)");
        CHECK(m0 && is_int(*m0) && as_int(*m0) == 0, "default dirty-aabb");

        auto set1 = cs.eval("(render:set-strategy 2)"); // Skip
        CHECK(set1 && is_int(*set1) && as_int(*set1) > epoch0, "epoch bumps on set");
        auto m1 = cs.eval("(render:get-strategy)");
        CHECK(m1 && is_int(*m1) && as_int(*m1) == 2, "mode skip");

        auto set2 = cs.eval("(render:set-strategy \"full\")");
        CHECK(set2 && is_int(*set2) && as_int(*set2) > as_int(*set1), "string set bumps");
        auto m2 = cs.eval("(render:get-strategy)");
        CHECK(m2 && is_int(*m2) && as_int(*m2) == 1, "mode full");
    }

    // ── AC3: Skip / Full / DirtyAABB observable behavior ──
    {
        std::println("\n--- AC3: present behavior by mode ---");
        reset_for_test();
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 8 4)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "buffer");
        const auto bid = as_int(*id);
        // One dirty cell.
        (void)cs.eval(std::format("(terminal-set-cell {} 1 1 65 7 0)", bid));

        // DirtyAABB: should emit > 0.
        (void)cs.eval("(render:set-strategy 0)");
        const auto n_aabb = present_pipe(cs, bid);
        CHECK(n_aabb > 0, "dirty-aabb emits");

        // Dirty again, then Skip: should return 0 without emitting.
        (void)cs.eval(std::format("(terminal-set-cell {} 2 2 66 7 0)", bid));
        (void)cs.eval("(render:set-strategy 2)");
        const auto skip_before = snapshot().mode_skip_total;
        const auto n_skip = present_pipe(cs, bid);
        CHECK(n_skip == 0, "skip returns 0");
        CHECK(snapshot().mode_skip_total > skip_before, "skip metric");

        // Still dirty: DirtyAABB again should emit (dirty preserved through Skip).
        (void)cs.eval("(render:set-strategy 0)");
        const auto n_after = present_pipe(cs, bid);
        CHECK(n_after > 0, "dirty preserved after skip → emit");

        // Full mode: force full frame.
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 67 7 0)", bid));
        (void)cs.eval("(render:set-strategy 1)");
        const auto full_before = snapshot().mode_full_total;
        const auto n_full = present_pipe(cs, bid);
        CHECK(n_full > 0, "full emits");
        CHECK(snapshot().mode_full_total > full_before, "full metric");
    }

    // ── AC4: schema-2138 ──
    {
        std::println("\n--- AC4: schema-2138 ---");
        reset_for_test();
        CompilerService cs;
        (void)cs.eval("(render:set-strategy 3 5000)"); // auto + 50% threshold
        CHECK(href(cs, "schema-2138") == 2138, "schema");
        CHECK(href(cs, "strategy-wired") == 1, "wired");
        CHECK(href(cs, "strategy-epoch") >= 1, "epoch key");
        CHECK(href(cs, "strategy-mode") == 3, "auto mode");
        CHECK(href(cs, "strategy-full-dirty-ratio-bp") == 5000, "threshold");
        CHECK(href(cs, "strategy-set-total") >= 1, "set total");
    }

    // ── AC6: mid-session swap ──
    {
        std::println("\n--- AC6: mid-session strategy swap ---");
        reset_for_test();
        CompilerService cs;
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id && is_int(*id), "buf");
        const auto bid = as_int(*id);
        for (int i = 0; i < 6; ++i) {
            (void)cs.eval(std::format("(terminal-set-cell {} 0 0 {} 7 0)", bid, 65 + i));
            if (i % 2 == 0)
                (void)cs.eval("(render:set-strategy 0)");
            else
                (void)cs.eval("(render:set-strategy 2)");
            (void)present_pipe(cs, bid);
        }
        const auto snap = snapshot();
        std::println("  resolve={} dirty={} skip={} sets={} epoch={}", snap.resolve_total,
                     snap.mode_dirty_aabb_total, snap.mode_skip_total, snap.set_strategy_total,
                     snap.epoch);
        CHECK(snap.resolve_total >= 6, "resolved each present");
        CHECK(snap.mode_skip_total >= 2, "skip path used");
        CHECK(snap.mode_dirty_aabb_total >= 2, "aabb path used");
        CHECK(snap.set_strategy_total >= 6, "hot-replaced strategy");
        CHECK(snap.epoch >= 6, "epoch advanced");
    }

    // Direct kernel API (no Evaluator) still works via present_batch_with_strategy.
    {
        std::println("\n--- kernel-only path ---");
        reset_for_test();
        set_strategy(Mode::Skip);
        aura::renderer::FramebufferOwned fb;
        fb.resize(2, 2);
        fb.dirty.mark_all_dirty(2, 2);
        auto v = fb.view();
        const auto n = aura::renderer::strategy::present_batch_with_strategy(v, fb.dirty, -1);
        CHECK(n == 0, "kernel skip");
        set_strategy(Mode::DirtyAABB);
        const auto n2 = aura::renderer::strategy::present_batch_with_strategy(v, fb.dirty, -1);
        CHECK(n2 > 0, "kernel dirty-aabb after skip preserves dirty");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
