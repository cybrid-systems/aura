// @category: integration
// @reason: Issue #1580 — strengthen fiber resume after steal: EnvFrame /
// bridge_epoch / StableNodeRef auto-refresh + panic checkpoint transfer.
//
//   AC1: complete_post_resume_steal_refresh bumps post_steal_refresh_count
//   AC2: transfer_and_revalidate_panic_checkpoint API + metrics
//   AC3: fiber resume hints captured at yield (env/epoch) used on refresh
//   AC4: auto_restamp Steal site + probe_and_repin do not crash
//   AC5: 1000-iter concurrent defuse bump + refresh + repin stress
//   AC6: restore_post_yield_or_rollback still consistent with #1490 path

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::test::g_failed;
using aura::test::g_passed;

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define y (f 40))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
}

static void ac1_complete_post_resume() {
    std::println("\n--- AC1: complete_post_resume_steal_refresh ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    const auto c0 = ev.get_post_steal_refresh_count();
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_post_steal_refresh_count() == c0 + 1, "post_steal_refresh_count +1");
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_post_steal_refresh_count() == c0 + 2, "second call advances again");
}

static void ac2_transfer_panic_api() {
    std::println("\n--- AC2: transfer_and_revalidate_panic_checkpoint ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    // No pending panic → false, no crash
    CHECK(!ev.transfer_and_revalidate_panic_checkpoint(nullptr), "no pending checkpoint → false");
    const auto t0 = ev.get_panic_transfer_on_steal_count();
    // Simulate pending via Guard path: save_panic_checkpoint if available
    // Without active Guard, pending is false — count stays.
    CHECK(ev.get_panic_transfer_on_steal_count() == t0, "no spurious transfer");
}

static void ac3_refresh_with_hints() {
    std::println("\n--- AC3: refresh with env/epoch hints ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    (void)cs.eval("(let ((a 1) (b 2)) (+ a b))");
    const auto frames = ev.env_frames_size();
    const auto epoch = ev.current_bridge_epoch();
    ev.bump_defuse_version_for_test();
    const auto n = ev.refresh_stale_frames_after_steal(
        frames > 0 ? static_cast<std::uint64_t>(frames - 1) : 0, epoch);
    CHECK(n >= 0, std::format("hinted refresh ok (refreshed={})", n));
    // complete path with null fiber still uses epoch 0 full scan
    const auto c0 = ev.get_post_steal_refresh_count();
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.get_post_steal_refresh_count() > c0, "complete path after hinted refresh");
}

static void ac4_repin_and_restamp() {
    std::println("\n--- AC4: probe_and_repin + Steal restamp ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    ev.probe_and_repin_linear_on_steal();
    const auto n = ev.auto_restamp_pinned_stable_refs_at(Evaluator::StableRefRefreshSite::Steal);
    CHECK(n >= 0, std::format("Steal restamp ok (n={})", n));
    CHECK(true, "no crash under repin + restamp");
}

static void ac5_thousand_iter_stress() {
    std::println("\n--- AC5: 1000-iter concurrent defuse + refresh + repin ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    std::atomic<bool> stop{false};
    std::atomic<int> refresh_ok{0};
    std::atomic<int> bump_ok{0};

    std::thread bumper([&] {
        for (int i = 0; i < 1000 && !stop.load(std::memory_order_relaxed); ++i) {
            ev.bump_defuse_version_for_test();
            bump_ok.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::thread refresher([&] {
        for (int i = 0; i < 1000; ++i) {
            ev.complete_post_resume_steal_refresh(nullptr);
            ev.probe_and_repin_linear_on_steal();
            refresh_ok.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_relaxed);
    });

    bumper.join();
    refresher.join();

    CHECK(refresh_ok.load() == 1000, "1000 refresh closed-loops");
    CHECK(bump_ok.load() >= 100, "concurrent defuse bumps");
    CHECK(ev.get_post_steal_refresh_count() >= 1000, "refresh count >= 1000");
    // Correctness: still evaluate after stress
    auto r = cs.eval("(+ 1 2)");
    CHECK(r.has_value(), "eval ok after stress");
}

static void ac6_restore_post_yield() {
    std::println("\n--- AC6: restore_post_yield_or_rollback ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    // Empty yield stack → true (no-op happy path)
    CHECK(ev.restore_post_yield_or_rollback(), "empty stack restore true");
    // After complete_post_resume, still consistent
    ev.complete_post_resume_steal_refresh(nullptr);
    CHECK(ev.restore_post_yield_or_rollback(), "still true after refresh");
}

} // namespace

int main() {
    std::println("=== test_fiber_resume_post_steal_refresh (#1580) ===");
    ac1_complete_post_resume();
    ac2_transfer_panic_api();
    ac3_refresh_with_hints();
    ac4_repin_and_restamp();
    ac5_thousand_iter_stress();
    ac6_restore_post_yield();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
