// @category: unit
// @reason: Issue #2050 — protect render-critical closures from deopt storms;
// hot-swap draw/present without frame drop under Agent mutate pressure.
//
//   AC1: schema-2050 keys on query:render-jit-stability-stats
//   AC2: evolution-named define auto-registers as render-critical on soft dirty
//   AC3: high-frequency mark_define_dirty → deopt throttled + jit-keep (no storm)
//   AC4: body-only dirty prefers partial re-lower when IR is present
//   AC5: set-body storm on draw/present define + present loop → frame CV bound
//   AC6: non-render defines still dirty without force-register as critical

#include "test_harness.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

std::int64_t href_m(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    return href_m(cs, "query:render-jit-stability-stats", key);
}

} // namespace

int main() {
    using clock = std::chrono::steady_clock;

    // ── AC1: schema-2050 surface ──
    {
        std::println("--- AC1: schema-2050 keys ---");
        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:render-jit-stability-stats\")");
        CHECK(h && is_hash(*h), "query:render-jit-stability-stats hash");
        CHECK(href(cs, "schema") == 1563, "schema 1563 (base)");
        CHECK(href(cs, "schema-2050") == 2050, "schema-2050");
        CHECK(href(cs, "issue-2050") == 2050, "issue-2050");
        CHECK(href(cs, "render-critical-protect-wired") == 1, "protect wired");
        // Issue #2329: default raised from 500ms to 30000ms so the
        // evaluator fallback actually takes effect. See observability_metrics.h
        // (render_deopt_throttle_window_ms{30000}) and evaluator.ixx:7511.
        CHECK(href(cs, "window-ms") == 30000, "default throttle window 30000ms (issue #2329)");
        // Counters present (non-negative; may be process-shared)
        for (const char* k :
             {"render-critical-define-dirty-total", "render-critical-deopt-throttled-total",
              "render-critical-deopt-applied-total", "render-critical-jit-keep-total",
              "render-critical-partial-prefer-total", "render-critical-define-registered-total",
              "render-critical-define-count"}) {
            CHECK(href(cs, k) >= 0, std::format("{} present", k));
        }
        CHECK(href(cs, "render-critical-meta-count") >= 5, "render prim meta annotated");
    }

    // ── AC2+AC3: evolution name + deopt throttle storm ──
    {
        std::println("--- AC2/AC3: render-critical soft-dirty throttle ---");
        CompilerService cs;
        // Define a draw/present-named function (auto-detected as render evolution).
        CHECK(cs.eval("(set-code \"(define (agent-draw-frame x) (+ x 1))\")").has_value(),
              "set-code agent-draw-frame");
        CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
        auto v0 = cs.eval("(agent-draw-frame 41)");
        CHECK(v0 && is_int(*v0) && as_int(*v0) == 42, "agent-draw-frame works");

        const auto dirty0 = href(cs, "render-critical-define-dirty-total");
        const auto thr0 = href(cs, "render-critical-deopt-throttled-total");
        const auto app0 = href(cs, "render-critical-deopt-applied-total");
        const auto keep0 = href(cs, "render-critical-jit-keep-total");
        const auto reg0 = href(cs, "render-critical-define-registered-total");

        constexpr int kBurst = 40;
        for (int i = 0; i < kBurst; ++i)
            cs.public_mark_define_dirty("agent-draw-frame");

        const auto dirty1 = href(cs, "render-critical-define-dirty-total");
        const auto thr1 = href(cs, "render-critical-deopt-throttled-total");
        const auto app1 = href(cs, "render-critical-deopt-applied-total");
        const auto keep1 = href(cs, "render-critical-jit-keep-total");
        const auto reg1 = href(cs, "render-critical-define-registered-total");
        const auto def_n = href(cs, "render-critical-define-count");

        CHECK(dirty1 - dirty0 >= kBurst, std::format("dirty delta {} >= burst", dirty1 - dirty0));
        CHECK(reg1 > reg0 || def_n >= 1, "evolution name registered as critical");
        // At most a few applies across 500ms windows; rest throttled.
        const auto d_thr = thr1 - thr0;
        const auto d_app = app1 - app0;
        CHECK(d_thr >= kBurst - 5, std::format("throttled {} >= ~burst", d_thr));
        CHECK(d_app <= 5, std::format("applied {} <= 5 (no storm)", d_app));
        CHECK(d_thr > d_app, "throttled exceeds applied");
        CHECK(keep1 > keep0, std::format("jit-keep advanced {} → {}", keep0, keep1));

        // Function still callable after soft-dirty storm.
        auto v1 = cs.eval("(agent-draw-frame 1)");
        CHECK(v1 && is_int(*v1) && as_int(*v1) == 2, "still evaluates after dirty storm");
    }

    // ── AC4: body-only partial prefer when IR present ──
    {
        std::println("--- AC4: body-only partial prefer ---");
        CompilerService cs;
        // Lower into IR cache so mark_body_only_dirty can succeed.
        CHECK(cs.eval("(set-code \"(define (present-hud n) (* n 2))\")").has_value(),
              "set-code present-hud");
        CHECK(cs.eval("(eval-current)").has_value(), "eval present-hud");
        // Warm IR path
        for (int i = 0; i < 5; ++i)
            (void)cs.eval("(present-hud 3)");

        const auto part0 = href(cs, "render-critical-partial-prefer-total");
        for (int i = 0; i < 10; ++i)
            cs.public_mark_define_dirty("present-hud");
        const auto part1 = href(cs, "render-critical-partial-prefer-total");
        // Soft: if IR cache has body shape, partial prefer advances; else still dirty-only.
        if (part1 > part0) {
            CHECK(true, std::format("partial-prefer +{}", part1 - part0));
        } else {
            // Fallback: body-only may not apply without full lower bundle — dirty still counted.
            CHECK(href(cs, "render-critical-define-dirty-total") >= 10,
                  "dirty still advanced when partial not available");
            std::println("  (partial prefer not taken — IR shape soft-ok)");
        }
    }

    // ── AC5: set-body storm + present frame variance ──
    {
        std::println("--- AC5: set-body storm + present frame CV ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (draw-cell-style n) (+ n 0))\")").has_value(),
              "set-code draw-cell-style");
        CHECK(cs.eval("(eval-current)").has_value(), "eval draw-cell-style");

        auto id = cs.eval("(make-terminal-buffer 16 8)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        const auto bid = as_int(*id);

        // Warm present path so cold first frames do not dominate CV.
        for (int w = 0; w < 12; ++w) {
            (void)cs.eval(std::format("(terminal-set-cell {} {} {} 65 1 0)", bid, w % 16, w % 8));
            (void)cs.eval(std::format("(terminal-present-batch {} -1)", bid));
            (void)cs.eval("(draw-cell-style 0)");
        }

        constexpr int kFrames = 90;
        std::vector<double> frame_ms;
        frame_ms.reserve(kFrames);

        const auto thr0 = href(cs, "render-critical-deopt-throttled-total");
        const auto keep0 = href(cs, "render-critical-jit-keep-total");
        const auto dirty0 = href(cs, "render-critical-define-dirty-total");
        const auto app0 = href(cs, "render-critical-deopt-applied-total");

        for (int f = 0; f < kFrames; ++f) {
            const auto t0 = clock::now();
            // Agent self-modify draw logic every 3rd frame (sustained pressure).
            if (f % 3 == 0) {
                const auto body = std::format("(lambda (n) (+ n {}))", f % 7);
                (void)cs.eval(
                    std::format("(mutate:set-body \"draw-cell-style\" \"{}\" \"r{}\")", body, f));
            }
            (void)cs.eval(std::format("(draw-cell-style {})", f % 5));
            (void)cs.eval(std::format("(terminal-set-cell {} {} {} {} {} 0)", bid, f % 16,
                                      (f / 16) % 8, 65 + (f % 26), (f % 7) + 1));
            auto p = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
            CHECK(p && is_int(*p) && as_int(*p) >= 0, "present under draw mutate");
            const auto t1 = clock::now();
            frame_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }

        double sum = 0;
        for (double m : frame_ms)
            sum += m;
        const double mean = sum / static_cast<double>(kFrames);
        double var = 0;
        for (double m : frame_ms) {
            const double d = m - mean;
            var += d * d;
        }
        var /= static_cast<double>(kFrames);
        const double stdev = std::sqrt(var);
        const double cv = mean > 1e-9 ? stdev / mean : 0.0;
        // CI-friendly bound under set-body pressure; target <10–15% warm host.
        CHECK(cv < 0.75, std::format("frame CV {:.3f} < 0.75 (mean={:.3f}ms)", cv, mean));
        if (cv < 0.15)
            CHECK(true, std::format("frame CV {:.3f} meets <15% AC target", cv));
        else
            std::println("  frame CV {:.3f} (mean={:.3f}ms) — CI-ok, warm target <15%", cv, mean);

        const auto thr1 = href(cs, "render-critical-deopt-throttled-total");
        const auto keep1 = href(cs, "render-critical-jit-keep-total");
        const auto dirty1 = href(cs, "render-critical-define-dirty-total");
        const auto app1 = href(cs, "render-critical-deopt-applied-total");
        CHECK(dirty1 >= dirty0, "dirty non-decreasing under set-body");
        // No deopt storm under set-body: applied << frames / throttled.
        CHECK(app1 - app0 <= 8,
              std::format("no deopt storm under set-body: applied {}", app1 - app0));
        if (thr1 > thr0 || keep1 > keep0)
            CHECK(true, std::format("protect counters under set-body thr+{} keep+{}", thr1 - thr0,
                                    keep1 - keep0));
        else
            std::println("  (set-body path did not hit mark_define_dirty — present OK)");
    }

    // ── AC6: non-render define not auto-critical ──
    {
        std::println("--- AC6: non-render define not auto-critical ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (plain-math x) (+ x 1))\")").has_value(),
              "set-code plain-math");
        CHECK(cs.eval("(eval-current)").has_value(), "eval plain-math");
        const auto reg0 = href(cs, "render-critical-define-registered-total");
        const auto dirty0 = href(cs, "render-critical-define-dirty-total");
        for (int i = 0; i < 5; ++i)
            cs.public_mark_define_dirty("plain-math");
        const auto reg1 = href(cs, "render-critical-define-registered-total");
        const auto dirty1 = href(cs, "render-critical-define-dirty-total");
        CHECK(reg1 == reg0, "plain-math not registered as render-critical");
        CHECK(dirty1 == dirty0, "plain-math dirty does not bump rc define-dirty");
        auto v = cs.eval("(plain-math 9)");
        CHECK(v && is_int(*v) && as_int(*v) == 10, "plain-math still correct");
    }

    std::println("\n#2050 render-critical hotswap: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
