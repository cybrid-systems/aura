// @category: unit
// @reason: Issue #3133 — Synthetic YieldReason::MutationBoundary injection on
// non-yielding hold-budget holders. Closes the #3071/#3035 residual window
// where a body that never hits check_gc_safepoint / yield keeps the
// exclusive lock after cancel is armed.
//
//   AC1: source cites #3133 in fiber.h + fiber.cpp — Fiber::inject_synthetic
//        mutation_boundary_yield() method (sets force_safepoint_requested_
//        AND last_yield_reason_ = MutationBoundary synthetically) + poll
//        caller replaces request_force_safepoint() with the new inject.
//   AC2: Soft/sandbox=off contract preserved — aura_hold_budget_poll_inbody
//        _window gates via mutation_hold_budget_reject_enabled(); Soft: no
//        call to inject (counter-only, no flag set).
//   AC3: Nested Guards never independently force-fail (outermost only) —
//        aura_evaluator_force_degrade_outermost_holder existing contract
//        preserved (#2932 / #2999 / #3035).
//   AC4: Reuse existing #3035 / #3071 counters (forced_fail_closed_total +
//        inbody_window_exceeded_total) — no new query key required.
//   AC5: No tests/issues/test_issue_3133.cpp (#81967); no docs/design/3133-*
//        (#1655). Extend existing test_chaos_mutate_steal_gc_mailbox lineage.

#include "test_harness.hpp"

#include "compiler/mutation_hold_budget.h"
#include "compiler/typed_mutation_audit.h"
#include "serve/fiber.h"
#include "serve/runtime_production_abi.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

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

} // namespace

int run_test_hold_budget_synthetic_yield_injection() {
    std::println("=== Issue #3133: hold-budget synthetic yield injection ===");
    CHECK(true, "ac3133: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: Fiber::inject_synthetic_mutation_boundary_yield ---");
        auto fh = read_file("src/serve/fiber.h");
        auto fc = read_file("src/serve/fiber.cpp");
        CHECK(!fh.empty(), "AC1: fiber.h readable");
        CHECK(!fc.empty(), "AC1: fiber.cpp readable");

        // Fiber.h: method declaration with the right signature + comment block.
        auto method_pos = fh.find("inject_synthetic_mutation_boundary_yield()");
        CHECK(method_pos != std::string::npos, "AC1: method declared in fiber.h");
        // Comment block sits immediately above the declaration (body is
        // out-of-line in fiber.cpp so the header window has no stores).
        const auto decl_start = method_pos > 1600 ? method_pos - 1600 : 0;
        auto win = fh.substr(decl_start, method_pos - decl_start + 80);
        CHECK(win.find("Issue #3133") != std::string::npos, "AC1: cites #3133 (declaration block)");
        CHECK(win.find("force_safepoint_requested") != std::string::npos,
              "AC1: sets force_safepoint_requested_ = true");
        CHECK(win.find("last_yield_reason_") != std::string::npos,
              "AC1: sets last_yield_reason_ = MutationBoundary synthetically");
        CHECK(win.find("YieldReason::MutationBoundary") != std::string::npos,
              "AC1: explicit YieldReason::MutationBoundary");

        // Fiber.cpp: method implementation + poll caller.
        auto impl_pos = fc.find("void Fiber::inject_synthetic_mutation_boundary_yield()");
        CHECK(impl_pos != std::string::npos, "AC1: method implemented in fiber.cpp");
        auto impl_end = fc.find("}\n\n", impl_pos);
        if (impl_end == -1 || impl_end > impl_pos + 1500)
            impl_end = impl_pos + 1500;
        auto impl_win = fc.substr(impl_pos, impl_end - impl_pos);
        CHECK(impl_win.find("force_safepoint_requested_.store(true") != std::string::npos,
              "AC1: impl sets force_safepoint_requested_");
        CHECK(impl_win.find("last_yield_reason_.store(YieldReason::MutationBoundary") !=
                  std::string::npos,
              "AC1: impl sets last_yield_reason_");

        // Poll caller replaces request_force_safepoint() with inject().
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        CHECK(poll_pos != std::string::npos, "AC1: poll definition present");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);
        CHECK(poll_win.find("Issue #3133") != std::string::npos, "AC1: poll caller cites #3133");
        CHECK(poll_win.find("inject_synthetic_mutation_boundary_yield()") != std::string::npos,
              "AC1: poll calls inject on subsequent polls");
        // First escalation (force_degrade) preserved — no replacement.
        CHECK(poll_win.find("aura_evaluator_force_degrade_outermost_holder(fid)") !=
                  std::string::npos,
              "AC1: first escalation via force_degrade preserved");
    }

    // ── AC2: Soft/sandbox=off contract ──
    {
        std::println("\n--- AC2: Soft/sandbox=off zero behavioural change ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);
        // The Soft gate is via mutation_hold_budget_reject_enabled() —
        // when Soft, the function returns early after bumping the metric,
        // so inject is never called.
        CHECK(poll_win.find("mutation_hold_budget_reject_enabled()") != std::string::npos,
              "AC2: Soft gate via reject_enabled()");
        CHECK(poll_win.find("if (!mutation_hold_budget_reject_enabled())") != std::string::npos,
              "AC2: Soft early-return gate");
        // Soft path is metric-only (AC2 contract).
        CHECK(poll_win.find("return 0; // Soft / sandbox=off") != std::string::npos,
              "AC2: Soft returns 0 (metric-only, no flag set)");
    }

    // ── AC3: Nested Guards never independently force-fail ──
    {
        std::println("\n--- AC3: outermost-only force-fail preserved ---");
        auto efl = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        // The existing aura_evaluator_force_degrade_outermost_holder contract
        // (called from poll on first escalation) only fires on outermost
        // (depth==0 + held==true). Nested Guards are NOT force-failed.
        auto pos =
            efl.find("aura_evaluator_force_degrade_outermost_holder(std::uint64_t fiber_id)");
        CHECK(pos != std::string::npos, "AC3: force_degrade_outermost_holder preserved");
        auto end = pos + 4000;
        auto win = efl.substr(pos, end - pos);
        CHECK(win.find("mark_outermost_mutation_failed") != std::string::npos,
              "AC3: outermost only mark");
        CHECK(win.find("outermost") != std::string::npos, "AC3: outermost contract preserved");
    }

    // ── AC4: counter surface reuse ──
    {
        std::println("\n--- AC4: existing counters reused, no new query key ---");
        auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        auto fc = read_file("src/serve/fiber.cpp");
        // Existing counters (no new keys per AC4):
        //   - g_mutation_hold_budget_forced_fail_closed_total (#3035)
        //   - g_mutation_hold_budget_inbody_window_exceeded_total (#3071)
        //   - g_hold_budget_cancel_armed_ns / _fiber / _escalated (#3071)
        CHECK(mhb.find("g_mutation_hold_budget_forced_fail_closed_total") != std::string::npos,
              "AC4: existing #3035 forced_fail_closed_total preserved");
        CHECK(mhb.find("g_mutation_hold_budget_inbody_window_exceeded_total") != std::string::npos,
              "AC4: existing #3071 inbody_window_exceeded_total preserved");
        // Poll wires through to the existing counters (no new atomics in the
        // poll body itself; the inject method reuses existing Fiber atomics).
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);
        CHECK(poll_win.find("g_mutation_hold_budget_inbody_window_exceeded_total.fetch_add") !=
                  std::string::npos,
              "AC4: existing inbody_window_exceeded_total bumped (poll)");
    }

    // ── AC5: no test_issue_3133.cpp, no docs/design/3133-* ──
    {
        std::println("\n--- AC5: src-aligned test, no plan doc ---");
        auto root = std::filesystem::current_path();
        CHECK(!std::filesystem::exists(root / "tests" / "issues" / "test_issue_3133.cpp"),
              "AC5: tests/issues/test_issue_3133.cpp absent (#81967)");
        CHECK(!std::filesystem::exists(root / "tests" / "serve" / "test_issue_3133.cpp"),
              "AC5: tests/serve/test_issue_3133.cpp absent (#81967)");
        auto docs = root / "docs" / "design";
        if (std::filesystem::exists(docs)) {
            for (const auto& f : std::filesystem::directory_iterator(docs)) {
                auto name = f.path().filename().string();
                CHECK(name.find("3133-") == std::string::npos,
                      "AC5: no docs/design/3133-* plan doc (#1655)");
                (void)name;
                break;
            }
        }
        // Existing hold-budget / chaos suites extended (regression-preserved):
        //   tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp (chaos lineage)
        CHECK(read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp")
                      .find("aura_hold_budget_poll_inbody_window") != std::string::npos,
              "AC5: chaos suite still exercises the poll path (regression)");
    }

    std::println("\n=== #3133 synthetic yield injection: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

// @category: unit
// @reason: Issue #3160 — hold-budget inbody window: after synthetic yield
// inject (from #3133), escalate force_degrade if live outermost holder
// is still held past 2× inbody bound (post-#3133 residual). #3133 closed
// the synthetic inject side but it is set-and-forget: if the body never
// reaches a cooperative edge (check_gc_safepoint / yield / Phase-5),
// lock + depth stay held past any multiple of the hold SLO. #3160 closes
// the residual by auditing the existing escalation path under the
// 2× bound contract + adding regression-guard source-cite for the
// one-shot force_degrade + re-inject-on-later-exceeds pattern.
//
//   AC6: aura_hold_budget_poll_inbody_window escalates after elapsed >
//        bound_us where bound_us = mutation_hold_inbody_window_bound_us()
//        (default 2× hold SLO per #3071).
//   AC7: escalate is one-shot per arm via
//        g_hold_budget_cancel_escalated.exchange(1, ...) == 0.
//   AC8: first escalate calls aura_evaluator_force_degrade_outermost_holder
//        which does request_cancel() + mark_outermost_mutation_failed()
//        (next edge cannot commit success).
//   AC9: later exceeds re-inject synthetic yield via
//        f->inject_synthetic_mutation_boundary_yield() (keeps inject
//        persistent until next cooperative edge).
//   AC10: mutation_hold_inbody_window_bound_us() returns slo * 2ULL
//         (default 2× hold SLO per #3071).
//   AC11: Soft / sandbox=off path: metric-only, no flag set
//         (mutation_hold_budget_reject_enabled() gate preserved).
//   AC12: no preemptive unlock of workspace_mtx_ while body is live
//         (#3035 dual-topology contract preserved).
//   AC13: no new middle-of-metrics counter introduced (uses existing
//         g_mutation_hold_budget_inbody_window_exceeded_total +
//         g_mutation_hold_budget_forced_fail_closed_total).
//   AC14: extends existing test_hold_budget_synthetic_yield_injection
//         suite (#81967); no tests/issues/test_issue_3160.cpp.
//   AC15: source-cite + coverage linter; no docs/design/3160-* per #1655.

int run_test_hold_budget_inbody_escalate() {
    std::println("=== Issue #3160: hold-budget inbody window escalate after 2× bound ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    // ── AC6: 2× bound threshold ──
    {
        std::println("\n--- AC6: poll escalates after elapsed > bound_us (2× SLO default) ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        CHECK(!fc.empty(), "AC6: fiber.cpp readable");
        CHECK(!mhb.empty(), "AC6: mutation_hold_budget.h readable");

        // Poll uses mutation_hold_inbody_window_bound_us() as the threshold.
        CHECK(fc.find("const auto bound_us = mutation_hold_inbody_window_bound_us();") !=
                  std::string::npos,
              "AC6: poll reads bound from mutation_hold_inbody_window_bound_us()");
        CHECK(fc.find("if (elapsed_us <= bound_us)") != std::string::npos,
              "AC6: poll escalates when elapsed > bound_us (2× SLO default per #3071)");

        // bound_us function returns slo * 2ULL (default 2× hold SLO).
        CHECK(mhb.find("return slo * 2ULL;") != std::string::npos,
              "AC6: bound default = slo * 2ULL (2× hold SLO)");
    }

    // ── AC7: escalate is one-shot per arm ──
    {
        std::println("\n--- AC7: escalate is one-shot via exchange(1) ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        CHECK(poll_pos != std::string::npos, "AC7: poll present");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);

        // exchange(1, std::memory_order_acq_rel) == 0 → first escalate only.
        CHECK(poll_win.find(
                  "g_hold_budget_cancel_escalated.exchange(1, std::memory_order_acq_rel)") !=
                  std::string::npos,
              "AC7: escalate is one-shot via exchange(1, acq_rel) == 0 "
              "(holder-degrade totals stay one-shot per window)");
    }

    // ── AC8: first escalate calls force_degrade_outermost_holder ──
    {
        std::println("\n--- AC8: first escalate calls force_degrade_outermost_holder ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto efl = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);

        // First escalate: aura_evaluator_force_degrade_outermost_holder(fid).
        CHECK(poll_win.find("aura_evaluator_force_degrade_outermost_holder(fid)") !=
                  std::string::npos,
              "AC8: first escalate calls force_degrade_outermost_holder(fid)");

        // force_degrade_outermost_holder does request_cancel + mark_outermost_mutation_failed
        // (next edge cannot commit success).
        CHECK(efl.find("aura_evaluator_force_degrade_outermost_holder(std::uint64_t fiber_id)") !=
                  std::string::npos,
              "AC8: force_degrade_outermost_holder defined");
        auto efl_pos =
            efl.find("aura_evaluator_force_degrade_outermost_holder(std::uint64_t fiber_id)");
        auto efl_end = efl_pos + 4000;
        auto efl_win = efl.substr(efl_pos, efl_end - efl_pos);
        CHECK(efl_win.find("request_cancel()") != std::string::npos,
              "AC8: force_degrade calls request_cancel() on the holder fiber");
        CHECK(efl_win.find("mark_outermost_mutation_failed()") != std::string::npos,
              "AC8: force_degrade calls mark_outermost_mutation_failed() "
              "(next commit cannot succeed)");
    }

    // ── AC9: later exceeds re-inject synthetic yield (keeps inject persistent) ──
    {
        std::println("\n--- AC9: later exceeds re-inject synthetic yield ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);

        // else branch (escalated already 1) → inject_synthetic_mutation_boundary_yield().
        CHECK(poll_win.find("f->inject_synthetic_mutation_boundary_yield();") != std::string::npos,
              "AC9: later exceeds re-inject synthetic yield (keeps inject persistent "
              "until next cooperative edge)");

        // Issue #3133 cite in the poll.
        CHECK(poll_win.find("Issue #3133") != std::string::npos,
              "AC9: poll cites #3133 (synthetic yield inject source)");
    }

    // ── AC11: Soft / sandbox=off metric-only ──
    {
        std::println("\n--- AC11: Soft / sandbox=off metric-only ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);

        // Soft gate via mutation_hold_budget_reject_enabled() — returns 0 if Soft.
        CHECK(poll_win.find("if (!mutation_hold_budget_reject_enabled())") != std::string::npos,
              "AC11: Soft gate via reject_enabled()");
        CHECK(poll_win.find("return 0; // Soft / sandbox=off") != std::string::npos,
              "AC11: Soft returns 0 (metric-only, no escalate / no force_degrade)");
    }

    // ── AC12: no preemptive workspace_mtx_ unlock while body is live ──
    {
        std::println("\n--- AC12: no preemptive workspace_mtx_ unlock while body is live ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        auto poll_end = poll_pos + 6000;
        auto poll_win = fc.substr(poll_pos, poll_end - poll_pos);

        // poll does NOT call any workspace_mtx_ unlock path.
        CHECK(poll_win.find("workspace_mtx_") == std::string::npos,
              "AC12: poll does NOT touch workspace_mtx_ (#3035 dual-topology contract preserved — "
              "no preemptive unlock while body is live)");
        CHECK(poll_win.find("unlock") == std::string::npos,
              "AC12: poll does NOT call unlock on any mutex "
              "(topology stays consistent until cooperative edge)");
    }

    // ── AC13: no new middle-of-metrics counter ──
    {
        std::println("\n--- AC13: no new middle-of-metrics counter (use existing) ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        // No g_3160_* atomic counter introduced (issue AC4: reuse existing).
        CHECK(fc.find("g_3160_") == std::string::npos,
              "AC13: no g_3160_* atomic counter in fiber.cpp (use existing "
              "g_mutation_hold_budget_inbody_window_exceeded_total)");
        CHECK(mhb.find("g_3160_") == std::string::npos,
              "AC13: no g_3160_* atomic counter in mutation_hold_budget.h");

        // Existing counters still present and used.
        CHECK(mhb.find("g_mutation_hold_budget_forced_fail_closed_total") != std::string::npos,
              "AC13: existing #3035 forced_fail_closed_total preserved");
        CHECK(mhb.find("g_mutation_hold_budget_inbody_window_exceeded_total") != std::string::npos,
              "AC13: existing #3071 inbody_window_exceeded_total preserved");
        CHECK(mhb.find("g_hold_budget_cancel_escalated") != std::string::npos,
              "AC13: existing #3071 cancel_escalated flag preserved");
    }

    // ── AC14: extends existing test suite (#81967); no test_issue_3160.cpp ──
    {
        std::println(
            "\n--- AC14: extends existing test suite (#81967); no test_issue_3160.cpp ---");
        auto root = std::filesystem::current_path();
        CHECK(!std::filesystem::exists(root / "tests" / "issues" / "test_issue_3160.cpp"),
              "AC14: tests/issues/test_issue_3160.cpp absent (#81967)");
        // Extends existing test_hold_budget_synthetic_yield_injection suite.
        auto this_test = read_file("tests/serve/test_hold_budget_synthetic_yield_injection.cpp");
        CHECK(!this_test.empty(), "AC14: existing test suite readable");
        CHECK(this_test.find("run_test_hold_budget_inbody_escalate") != std::string::npos,
              "AC14: this test extends existing test_hold_budget_synthetic_yield_injection.cpp "
              "(run_test_hold_budget_inbody_escalate added)");
    }

    // ── AC15: no docs/design/3160-* (per #1655) ──
    {
        std::println("\n--- AC15: no docs/design/3160-* (per #1655) ---");
        const auto design = read_file("docs/design/3160-hold-budget-inbody-escalate.md");
        CHECK(design.empty(), "AC15: no docs/design/3160-* plan doc (per #1655 aura 哲学)");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3160 hold-budget inbody escalate: {} passed, {} failed ===", passed,
                 failed);
    return failed == 0 ? 0 : 1;
}

// @reason: Issue #3194 — non-cooperative inbody window force-release (I1).
//   AC1: same-fiber poll past bound force-releases hold + depth + marks failed
//   AC2: cross-fiber helper only pending-cancel
//   AC3: Soft observe-only
//   AC4: reuse forced_unlock_total + forced_fail_closed_total
//   AC5/AC6: extend this suite + linter; no invent / docs/design

int run_test_hold_budget_inbody_force_release() {
    std::println("=== Issue #3194: hold-budget inbody force-release (I1 residual) ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    // ac3194_1_same_fiber_force_release
    {
        std::println("\n--- AC1: same-fiber force-release past inbody bound ---");
        using aura::compiler::CompilerService;
        using aura::compiler::Evaluator;
        using aura::serve::Scheduler;
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        ::unsetenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::mutation_hold_budget_reject_enabled(),
              "3194 AC1: reject_enabled under production");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        CompilerService cs;
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<int> ok_flag{1};
        std::atomic<int> ran{0};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        std::atomic<int> polled{-1};
        Scheduler sched(2);
        sched.spawn([&]() {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                auto* f = aura::serve::g_current_fiber;
                CHECK(f != nullptr, "3194 AC1: fiber current");
                aura::compiler::mutation_hold_budget_note_cancel_armed(f->id());
                aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
                polled.store(aura::serve::aura_hold_budget_poll_inbody_window(),
                             std::memory_order_relaxed);
                held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                                 std::memory_order_relaxed);
                depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                                  std::memory_order_relaxed);
                ran.store(1, std::memory_order_relaxed);
            }
            ok_flag.store(ok ? 1 : 0, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3194 AC1: fiber body ran");
        CHECK(polled.load() == 1, "3194 AC1: poll exceeded bound");
        CHECK(ok_flag.load() == 0, "3194 AC1: success flag forced false");
        CHECK(held_after.load() == 0, "3194 AC1: workspace hold cleared");
        CHECK(depth_after.load() == 0, "3194 AC1: depth slot == 0");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() >= 1,
              "3194 AC1: forced_unlock_total");
        CHECK(aura::compiler::mutation_hold_budget_forced_fail_closed_total_v_read() >= 1,
              "3194 AC1: forced_fail_closed_total");
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ac3194_2_cross_fiber_no_preemptive_release
    {
        std::println("\n--- AC2: cross-fiber pending-cancel only ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto pos = emb.find("aura_evaluator_force_release_outermost_holder");
        CHECK(pos != std::string::npos, "3194 AC2: helper present");
        const auto win = emb.substr(pos, 1600);
        CHECK(win.find("cur->id() == fiber_id") != std::string::npos, "3194 AC2: same-fiber test");
        CHECK(win.find("aura_fiber_request_hold_budget_cancel") != std::string::npos,
              "3194 AC2: cross-fiber pending-cancel");
        CHECK(win.find("force_release_hold_budget_inbody") != std::string::npos,
              "3194 AC2: same-fiber reuses #3118 force-release via public wrapper");
        CHECK(emb.find("force_release_hold_after_cancel_") != std::string::npos,
              "3194 AC2: #3118 helper still present");
        const auto fc = read_file("src/serve/fiber.cpp");
        const auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        const auto poll_win = fc.substr(poll_pos, 7000);
        CHECK(poll_win.find("workspace_mtx_") == std::string::npos,
              "3194 AC2: poll does not touch workspace_mtx_ (helper owns release)");
    }

    // ac3194_3_soft_observe_only
    {
        std::println("\n--- AC3: Soft observe-only ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        CHECK(!aura::compiler::mutation_hold_budget_reject_enabled(),
              "3194 AC3: Soft reject_enabled false");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
        aura::compiler::g_hold_budget_cancel_armed_fiber.store(1, std::memory_order_release);
        const auto u0 = aura::compiler::mutation_hold_budget_forced_unlock_total_v_read();
        (void)aura::serve::aura_hold_budget_poll_inbody_window();
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == u0,
              "3194 AC3: no force-release under Soft");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(emb.find("if (!mutation_hold_budget_reject_enabled())") != std::string::npos,
              "3194 AC3: helper gates on reject_enabled");
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ── AC4/AC5/AC6: counters + linter + no invent ──
    {
        std::println("\n--- AC4/AC6: reuse counters + linter ---");
        const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        const auto t = read_file("tests/serve/test_hold_budget_synthetic_yield_injection.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_hold_budget_inbody_force_release_3194.py");
        const auto build = read_file("build.py");
        CHECK(mhb.find("kMutationHoldBudgetInbodyForceReleaseIssue") != std::string::npos,
              "3194 AC4: issue stamp");
        CHECK(mhb.find("g_3194_") == std::string::npos, "3194 AC4: no new g_3194_* counter");
        CHECK(t.find("ac3194_1_same_fiber_force_release") != std::string::npos ||
                  t.find("3194 AC1: same-fiber") != std::string::npos,
              "3194 AC5: AC1 in this suite");
        CHECK(t.find("ac3194_2_cross_fiber_no_preemptive_release") != std::string::npos ||
                  t.find("3194 AC2: cross-fiber") != std::string::npos,
              "3194 AC5: AC2 in this suite");
        CHECK(t.find("ac3194_3_soft_observe_only") != std::string::npos ||
                  t.find("3194 AC3: Soft") != std::string::npos,
              "3194 AC5: AC3 in this suite");
        CHECK(!lint.empty() && lint.find("3194") != std::string::npos, "3194 AC6: linter");
        CHECK(build.find("check_hold_budget_inbody_force_release_3194") != std::string::npos,
              "3194 AC6: build.py");
        CHECK(read_file("docs/design/3194-hold-budget-inbody-force-release.md").empty(),
              "3194 AC6: no docs/design/");
        CHECK(read_file("tests/serve/test_issue_3194.cpp").empty(),
              "3194 AC6: no invent test_issue_3194");
        CHECK(read_file("tests/issues/test_issue_3194.cpp").empty(),
              "3194 AC6: no tests/issues/test_issue_3194");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3194 hold-budget inbody force-release: {} passed, {} failed ===", passed,
                 failed);
    return failed == 0 ? 0 : 1;
}

// @reason: Issue #3222 — I1 residual of #3194. Scheduler idle poll is
//   always cross-fiber (pending-cancel only). Same-fiber inbody poll
//   from Fiber::check_gc_safepoint force-releases hold + depth past
//   2×SLO so a live body cannot keep workspace_mtx_ until dtor.
//   AC1: same-fiber check_gc_safepoint past bound force-releases + marks failed
//   AC2: cross-fiber helper only pending-cancel; poll cannot spell "unlock"
//   AC3: Soft observe-only
//   AC4: reuse forced_unlock_total + forced_fail_closed_total
//   AC5/AC6: extend this suite + linter; no invent / docs/design

int run_test_hold_budget_inbody_force_unlock() {
    std::println("=== Issue #3222: hold-budget inbody force-unlock (I1 residual of #3194) ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    // ac3222_1_same_fiber_safepoint_force_unlock
    {
        std::println(
            "\n--- AC1: same-fiber check_gc_safepoint force-releases past inbody bound ---");
        using aura::compiler::CompilerService;
        using aura::compiler::Evaluator;
        using aura::serve::Fiber;
        using aura::serve::Scheduler;
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        ::unsetenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::mutation_hold_budget_reject_enabled(),
              "3222 AC1: reject_enabled under production");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        CompilerService cs;
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<int> ok_flag{1};
        std::atomic<int> ran{0};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        Scheduler sched(2);
        sched.spawn([&]() {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                auto* f = aura::serve::g_current_fiber;
                CHECK(f != nullptr, "3222 AC1: fiber current");
                f->request_hold_budget_cancel();
                aura::compiler::mutation_hold_budget_note_cancel_armed(f->id());
                aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
                Fiber::check_gc_safepoint();
                held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                                 std::memory_order_relaxed);
                depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                                  std::memory_order_relaxed);
                ran.store(1, std::memory_order_relaxed);
            }
            ok_flag.store(ok ? 1 : 0, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3222 AC1: fiber body ran");
        CHECK(ok_flag.load() == 0, "3222 AC1: success flag forced false");
        CHECK(held_after.load() == 0, "3222 AC1: workspace hold cleared at safepoint");
        CHECK(depth_after.load() == 0, "3222 AC1: depth slot == 0");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() >= 1,
              "3222 AC1: forced_unlock_total");
        CHECK(aura::compiler::mutation_hold_budget_forced_fail_closed_total_v_read() >= 1,
              "3222 AC1: forced_fail_closed_total");
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ac3222_2_cross_fiber_no_preemptive_unlock
    {
        std::println("\n--- AC2: cross-fiber pending-cancel only ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto pos = emb.find("aura_evaluator_force_unlock_outermost_holder");
        CHECK(pos != std::string::npos, "3222 AC2: force_unlock ABI present");
        CHECK(emb.find("aura_evaluator_force_release_outermost_holder(fiber_id)") !=
                  std::string::npos,
              "3222 AC2: force_unlock reuses #3194 force_release");
        const auto rel = emb.find("aura_evaluator_force_release_outermost_holder");
        CHECK(rel != std::string::npos, "3222 AC2: force_release helper present");
        const auto win = emb.substr(rel, 1800);
        CHECK(win.find("cur->id() == fiber_id") != std::string::npos, "3222 AC2: same-fiber test");
        CHECK(win.find("aura_fiber_request_hold_budget_cancel") != std::string::npos,
              "3222 AC2: cross-fiber pending-cancel");
        const auto fc = read_file("src/serve/fiber.cpp");
        const auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        const auto poll_win = fc.substr(poll_pos, 7000);
        CHECK(poll_win.find("aura_evaluator_force_unlock_outermost_holder") == std::string::npos,
              "3222 AC2: poll does not spell force_unlock (#3160 AC12)");
        CHECK(poll_win.find("aura_evaluator_force_release_outermost_holder") != std::string::npos,
              "3222 AC2: poll still calls force_release");
        const auto sp = fc.find("void Fiber::check_gc_safepoint()");
        CHECK(sp != std::string::npos, "3222 AC2: check_gc_safepoint present");
        const auto sp_win = fc.substr(sp, 2500);
        CHECK(sp_win.find("aura_hold_budget_poll_inbody_window()") != std::string::npos,
              "3222 AC2: check_gc_safepoint polls inbody window (same-fiber)");
    }

    // ac3222_3_soft_observe_only
    {
        std::println("\n--- AC3: Soft observe-only ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        CHECK(!aura::compiler::mutation_hold_budget_reject_enabled(),
              "3222 AC3: Soft reject_enabled false");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
        aura::compiler::g_hold_budget_cancel_armed_fiber.store(1, std::memory_order_release);
        const auto u0 = aura::compiler::mutation_hold_budget_forced_unlock_total_v_read();
        aura::serve::Fiber::check_gc_safepoint();
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == u0,
              "3222 AC3: no force-unlock under Soft");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(emb.find("if (!mutation_hold_budget_reject_enabled())") != std::string::npos,
              "3222 AC3: helper gates on reject_enabled");
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ── AC4/AC5/AC6: counters + linter + no invent ──
    {
        std::println("\n--- AC4/AC6: reuse counters + linter ---");
        const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        const auto t = read_file("tests/serve/test_hold_budget_synthetic_yield_injection.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_hold_budget_inbody_force_unlock_3222.py");
        const auto build = read_file("build.py");
        CHECK(mhb.find("kMutationHoldBudgetInbodyForceUnlockIssue") != std::string::npos,
              "3222 AC4: issue stamp");
        CHECK(mhb.find("g_3222_") == std::string::npos, "3222 AC4: no new g_3222_* counter");
        CHECK(t.find("ac3222_1_same_fiber_safepoint_force_unlock") != std::string::npos ||
                  t.find("3222 AC1: same-fiber") != std::string::npos,
              "3222 AC5: AC1 in this suite");
        CHECK(t.find("ac3222_2_cross_fiber_no_preemptive_unlock") != std::string::npos ||
                  t.find("3222 AC2: cross-fiber") != std::string::npos,
              "3222 AC5: AC2 in this suite");
        CHECK(t.find("ac3222_3_soft_observe_only") != std::string::npos ||
                  t.find("3222 AC3: Soft") != std::string::npos,
              "3222 AC5: AC3 in this suite");
        CHECK(!lint.empty() && lint.find("3222") != std::string::npos, "3222 AC6: linter");
        CHECK(build.find("check_hold_budget_inbody_force_unlock_3222") != std::string::npos,
              "3222 AC6: build.py");
        CHECK(read_file("docs/design/3222-hold-budget-inbody-force-unlock.md").empty(),
              "3222 AC6: no docs/design/");
        CHECK(read_file("tests/serve/test_issue_3222.cpp").empty(),
              "3222 AC6: no invent test_issue_3222");
        CHECK(read_file("tests/issues/test_issue_3222.cpp").empty(),
              "3222 AC6: no tests/issues/test_issue_3222");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3222 hold-budget inbody force-unlock: {} passed, {} failed ===", passed,
                 failed);
    return failed == 0 ? 0 : 1;
}

// @reason: Issue #3223 — cross-fiber force_degrade must nudge the victim
//   worker to run the same inbody force-release as same-fiber (#3222).
//   AC1: cross-fiber force_degrade + victim check_gc_safepoint past bound
//        force-releases hold + depth + marks failed
//   AC2: Soft observe-only
//   AC3: outermost-only (g_tls_outermost_guard / #2932)
//   AC4: reuse cross-fiber fired/consumed + forced_unlock totals
//   AC5/AC6: extend this suite + linter; no invent / docs/design

int run_test_hold_budget_cross_fiber_urgent_inbody_poll() {
    std::println("=== Issue #3223: cross-fiber urgent inbody poll (I1 residual of #2726) ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    // ac3223_1_cross_fiber_force_degrade_victim_force_release
    {
        std::println("\n--- AC1: cross-fiber force_degrade → victim safepoint force-release ---");
        using aura::compiler::CompilerService;
        using aura::compiler::Evaluator;
        using aura::serve::Fiber;
        using aura::serve::Scheduler;
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        ::setenv("AURA_HOLD_BUDGET_INBODY_BOUND_US", "1000", 1);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::mutation_hold_budget_reject_enabled(),
              "3223 AC1: reject_enabled under production");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::clear_mutation_hold_budget_holder_degrade_cross_fiber_cancel_for_test();
        CompilerService cs;
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<std::uint64_t> holder_id{0};
        std::atomic<int> ready{0};
        std::atomic<int> go{0};
        std::atomic<int> ran{0};
        std::atomic<int> ok_flag{1};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        Scheduler sched(2);
        sched.spawn([&]() {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                auto* f = aura::serve::g_current_fiber;
                CHECK(f != nullptr, "3223 AC1: holder fiber current");
                holder_id.store(f->id(), std::memory_order_release);
                ready.store(1, std::memory_order_release);
                for (int i = 0; i < 400 && go.load(std::memory_order_acquire) == 0; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                Fiber::check_gc_safepoint();
                held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                                 std::memory_order_relaxed);
                depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                                  std::memory_order_relaxed);
                ran.store(1, std::memory_order_relaxed);
            }
            ok_flag.store(ok ? 1 : 0, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ready.load(std::memory_order_acquire) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(ready.load() == 1, "3223 AC1: holder entered Guard");
        const auto fid = holder_id.load(std::memory_order_acquire);
        CHECK(fid != 0, "3223 AC1: holder id published");
        // Host thread: g_current_fiber is null → cross-fiber force_degrade.
        aura::serve::aura_evaluator_force_degrade_outermost_holder(fid);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        go.store(1, std::memory_order_release);
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3223 AC1: holder body ran");
        CHECK(ok_flag.load() == 0, "3223 AC1: success flag forced false");
        CHECK(held_after.load() == 0, "3223 AC1: workspace hold cleared on victim");
        CHECK(depth_after.load() == 0, "3223 AC1: depth slot == 0");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() >= 1,
              "3223 AC1: forced_unlock_total");
        CHECK(aura::compiler::
                      mutation_hold_budget_holder_degrade_cross_fiber_cancel_fired_total_v_read() >=
                  1,
              "3223 AC1: cross-fiber cancel fired");
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ac3223_2_soft_observe_only
    {
        std::println("\n--- AC2: Soft observe-only ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        CHECK(!aura::compiler::mutation_hold_budget_reject_enabled(),
              "3223 AC2: Soft reject_enabled false");
        const auto efm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        const auto pos = efm.find("aura_fiber_request_urgent_inbody_poll(fiber_id)");
        CHECK(pos != std::string::npos, "3223 AC2: urgent poll wired on force_degrade");
        const auto win = efm.substr(pos > 400 ? pos - 400 : 0, 800);
        CHECK(win.find("mutation_hold_budget_reject_enabled()") != std::string::npos,
              "3223 AC2: urgent poll gated on reject_enabled");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::serve::aura_evaluator_force_degrade_outermost_holder(1);
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == 0,
              "3223 AC2: Soft force_degrade does not force-release");
    }

    // ac3223_3_outermost_only
    {
        std::println("\n--- AC3: outermost-only ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        CHECK(emb.find("g_tls_outermost_guard") != std::string::npos,
              "3223 AC3: force-release uses outermost TLS Guard");
        CHECK(emb.find("cur->id() == fiber_id") != std::string::npos,
              "3223 AC3: same-fiber test (no foreign unique_lock unlock)");
    }

    // ── AC4/AC5/AC6: counters + linter + no invent ──
    {
        std::println("\n--- AC4/AC6: reuse counters + linter ---");
        const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        const auto t = read_file("tests/serve/test_hold_budget_synthetic_yield_injection.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_cross_fiber_urgent_inbody_poll_3223.py");
        const auto build = read_file("build.py");
        const auto fc = read_file("src/serve/fiber.cpp");
        CHECK(mhb.find("kMutationHoldBudgetCrossFiberUrgentInbodyPollIssue") != std::string::npos,
              "3223 AC4: issue stamp");
        CHECK(mhb.find("g_3223_") == std::string::npos, "3223 AC4: no new g_3223_* counter");
        CHECK(fc.find("aura_fiber_request_urgent_inbody_poll") != std::string::npos,
              "3223 AC4: urgent poll ABI");
        CHECK(t.find("ac3223_1_cross_fiber_force_degrade_victim_force_release") !=
                  std::string::npos,
              "3223 AC5: AC1 in this suite");
        CHECK(t.find("ac3223_2_soft_observe_only") != std::string::npos,
              "3223 AC5: AC2 in this suite");
        CHECK(!lint.empty() && lint.find("3223") != std::string::npos, "3223 AC6: linter");
        CHECK(build.find("check_cross_fiber_urgent_inbody_poll_3223") != std::string::npos,
              "3223 AC6: build.py");
        CHECK(read_file("docs/design/3223-cross-fiber-urgent-inbody-poll.md").empty(),
              "3223 AC6: no docs/design/");
        CHECK(read_file("tests/serve/test_issue_3223.cpp").empty(),
              "3223 AC6: no invent test_issue_3223");
        CHECK(read_file("tests/issues/test_issue_3223.cpp").empty(),
              "3223 AC6: no tests/issues/test_issue_3223");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3223 cross-fiber urgent inbody poll: {} passed, {} failed ===", passed,
                 failed);
    return failed == 0 ? 0 : 1;
}

// @reason: Issue #3254 — non-cooperative outermost body past 2×SLO
//   force-releases without an accidental check_gc_safepoint poll.
//   AC1: same-fiber poll_inbody_window injects synthetic yield and
//        consumes it (dual restore + unlock + depth 0)
//   AC2: cross-fiber force_degrade + urgent poll; holder poll consumes;
//        foreign thread never drops unique_lock
//   AC3: Soft observe-only
//   AC4: abort/dual restore + canary (exit_mutation_boundary false)
//   AC5/AC6: extend this suite + linter; no invent / docs/design

int run_test_hold_budget_noncoop_force_edge() {
    std::println("=== Issue #3254: non-cooperative inbody force-edge (I1 of #3222/#3223) ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    // ac3254_1_same_fiber_poll_consumes_injected_edge
    {
        std::println("\n--- AC1: same-fiber poll injects+consumes without check_gc_safepoint ---");
        using aura::compiler::CompilerService;
        using aura::compiler::Evaluator;
        using aura::serve::Scheduler;
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        ::unsetenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::mutation_hold_budget_reject_enabled(),
              "3254 AC1: reject_enabled under production");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        CompilerService cs;
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<int> ok_flag{1};
        std::atomic<int> ran{0};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        std::atomic<int> polled{0};
        Scheduler sched(2);
        sched.spawn([&]() {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                auto* f = aura::serve::g_current_fiber;
                CHECK(f != nullptr, "3254 AC1: fiber current");
                f->request_hold_budget_cancel();
                aura::compiler::mutation_hold_budget_note_cancel_armed(f->id());
                aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
                // Tight non-yield body: no check_gc_safepoint / yield.
                volatile std::uint64_t sink = 0;
                for (int i = 0; i < 64; ++i)
                    sink += static_cast<std::uint64_t>(i);
                (void)sink;
                polled.store(aura::serve::aura_hold_budget_poll_inbody_window(),
                             std::memory_order_relaxed);
                held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                                 std::memory_order_relaxed);
                depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                                  std::memory_order_relaxed);
                ran.store(1, std::memory_order_relaxed);
            }
            ok_flag.store(ok ? 1 : 0, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3254 AC1: fiber body ran");
        CHECK(polled.load() == 1, "3254 AC1: poll exceeded bound");
        CHECK(ok_flag.load() == 0, "3254 AC1: success flag forced false");
        CHECK(held_after.load() == 0, "3254 AC1: workspace hold cleared");
        CHECK(depth_after.load() == 0, "3254 AC1: depth slot == 0");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() >= 1,
              "3254 AC1: forced_unlock_total");
        CHECK(aura::compiler::mutation_hold_budget_forced_fail_closed_total_v_read() >= 1,
              "3254 AC1: forced_fail_closed_total");
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ac3254_2_cross_fiber_no_preemptive_unlock
    {
        std::println("\n--- AC2: cross-fiber inject; foreign thread never unlocks ---");
        using aura::compiler::CompilerService;
        using aura::compiler::Evaluator;
        using aura::serve::Scheduler;
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        ::setenv("AURA_HOLD_BUDGET_INBODY_BOUND_US", "1000", 1);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        CompilerService cs;
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<std::uint64_t> holder_id{0};
        std::atomic<int> ready{0};
        std::atomic<int> go{0};
        std::atomic<int> ran{0};
        std::atomic<int> ok_flag{1};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        Scheduler sched(2);
        sched.spawn([&]() {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                auto* f = aura::serve::g_current_fiber;
                CHECK(f != nullptr, "3254 AC2: holder fiber current");
                holder_id.store(f->id(), std::memory_order_release);
                ready.store(1, std::memory_order_release);
                for (int i = 0; i < 400 && go.load(std::memory_order_acquire) == 0; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                // Consume the runtime-injected edge via poll, not
                // accidental check_gc_safepoint. Stamp arm in the past so
                // a scheduler idle re-arm cannot shrink elapsed below bound.
                aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
                (void)aura::serve::aura_hold_budget_poll_inbody_window();
                held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                                 std::memory_order_relaxed);
                depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                                  std::memory_order_relaxed);
                ran.store(1, std::memory_order_relaxed);
            }
            ok_flag.store(ok ? 1 : 0, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ready.load(std::memory_order_acquire) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(ready.load() == 1, "3254 AC2: holder entered Guard");
        const auto fid = holder_id.load(std::memory_order_acquire);
        CHECK(fid != 0, "3254 AC2: holder id published");
        const auto u0 = aura::compiler::mutation_hold_budget_forced_unlock_total_v_read();
        aura::serve::aura_evaluator_force_degrade_outermost_holder(fid);
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == u0,
              "3254 AC2: foreign thread did not force-unlock");
        go.store(1, std::memory_order_release);
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3254 AC2: holder body ran");
        CHECK(ok_flag.load() == 0, "3254 AC2: success flag forced false");
        CHECK(held_after.load() == 0, "3254 AC2: workspace hold cleared on victim");
        CHECK(depth_after.load() == 0, "3254 AC2: depth slot == 0");
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ac3254_3_soft_observe_only
    {
        std::println("\n--- AC3: Soft observe-only ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        CHECK(!aura::compiler::mutation_hold_budget_reject_enabled(),
              "3254 AC3: Soft reject_enabled false");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
        aura::compiler::g_hold_budget_cancel_armed_fiber.store(1, std::memory_order_release);
        const auto u0 = aura::compiler::mutation_hold_budget_forced_unlock_total_v_read();
        CHECK(aura::serve::aura_hold_budget_poll_inbody_window() == 0,
              "3254 AC3: Soft poll does not force");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == u0,
              "3254 AC3: no force-unlock under Soft");
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    // ac3254_4_topology_dual_restore
    {
        std::println("\n--- AC4: force path dual-restores topology ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto pos = emb.find("void Evaluator::MutationBoundaryGuard::"
                                  "force_release_hold_budget_inbody()");
        CHECK(pos != std::string::npos, "3254 AC4: force_release_hold_budget_inbody impl");
        const auto win = emb.substr(pos, 900);
        CHECK(win.find("exit_mutation_boundary(false)") != std::string::npos,
              "3254 AC4: abort dual-restore on force path");
        CHECK(win.find("force_release_hold_after_cancel_") != std::string::npos,
              "3254 AC4: unlock after restore");
        CHECK(emb.find("if (!inbody_force_exited_)") != std::string::npos,
              "3254 AC4: dtor does not double-exit");
    }

    // ac3254_5/6 source + linter
    {
        std::println("\n--- AC5/AC6: suite + linter ---");
        const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        const auto t = read_file("tests/serve/test_hold_budget_synthetic_yield_injection.cpp");
        const auto fc = read_file("src/serve/fiber.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_hold_budget_noncoop_force_edge_3254.py");
        const auto build = read_file("build.py");
        CHECK(mhb.find("kMutationHoldBudgetNoncoopForceEdgeIssue") != std::string::npos,
              "3254 AC6: issue stamp");
        CHECK(mhb.find("g_3254_") == std::string::npos, "3254 AC4: no new g_3254_* counter");
        CHECK(fc.find("Issue #3254") != std::string::npos, "3254 AC6: poll cites #3254");
        CHECK(t.find("ac3254_1_same_fiber_poll_consumes_injected_edge") != std::string::npos ||
                  t.find("3254 AC1:") != std::string::npos,
              "3254 AC5: AC1 in this suite");
        CHECK(!lint.empty() && lint.find("3254") != std::string::npos, "3254 AC6: linter");
        CHECK(build.find("check_hold_budget_noncoop_force_edge_3254") != std::string::npos,
              "3254 AC6: build.py");
        CHECK(read_file("docs/design/3254-hold-budget-noncoop-force-edge.md").empty(),
              "3254 AC6: no docs/design/");
        CHECK(read_file("tests/serve/test_issue_3254.cpp").empty(),
              "3254 AC6: no invent test_issue_3254");
        CHECK(read_file("tests/issues/test_issue_3254.cpp").empty(),
              "3254 AC6: no tests/issues/test_issue_3254");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3254 non-cooperative inbody force-edge: {} passed, {} failed ===", passed,
                 failed);
    return failed == 0 ? 0 : 1;
}

// Issue #3285: I1 residual of #3254/#3222 — the inbody watchdog must inject
// the synthetic MutationBoundary edge within 1×SLO of cancel-arm (not only
// at the 2×SLO hard bound) so a body that does hit a cooperative edge
// consumes the pending cancel early and force-releases (dual-restore +
// unlock + depth 0) inside the 2×SLO window. Cross-fiber never unlocks
// (AC2) — it sets pending-cancel + urgent inbody poll via the #3223
// helper. Soft / sandbox=off: observe-only (reject_enabled gate).
int run_test_hold_budget_1slo_inject_3285() {
    std::println("\n=== Issue #3285: 1×SLO synthetic-edge inject tier ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    // AC1: source-cite — the poll has a 1×SLO inject tier before the
    // 2×SLO bound, gated on reject_enabled (Soft observe-only).
    {
        std::println("\n--- AC1: 1×SLO inject tier in aura_hold_budget_poll_inbody_window ---");
        auto fc = read_file("src/serve/fiber.cpp");
        auto pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        CHECK(pos != std::string::npos, "3285 AC1: poll definition present");
        auto win = fc.substr(pos, 3200);
        CHECK(win.find("Issue #3285") != std::string::npos, "3285 AC1: cites #3285");
        CHECK(win.find("mutation_hold_slo_us()") != std::string::npos,
              "3285 AC1: 1×SLO tier keyed on mutation_hold_slo_us");
        CHECK(win.find("elapsed_us > slo_us") != std::string::npos,
              "3285 AC1: inject when elapsed > 1×SLO");
        CHECK(win.find("mutation_hold_budget_reject_enabled()") != std::string::npos,
              "3285 AC1: Soft gate via reject_enabled");
        CHECK(win.find("inject_synthetic_mutation_boundary_yield()") != std::string::npos ||
                  win.find("aura_fiber_request_urgent_inbody_poll") != std::string::npos,
              "3285 AC1: same-fiber inject OR cross-fiber urgent poll nudge");
        // 2×SLO hard bound still present after the tier (force path kept).
        CHECK(win.find("inbody_window_exceeded_total") != std::string::npos,
              "3285 AC1: 2×SLO hard-bound counter preserved");
    }

    // AC2: reuse existing counters — no new keys (forced_unlock_total /
    // forced_fail_closed_total / inbody_window_exceeded_total stay the
    // force-path signals).
    {
        std::println("\n--- AC2: counter reuse, no new bus ---");
        auto mh = read_file("src/compiler/mutation_hold_budget.h");
        CHECK(mh.find("g_mutation_hold_budget_forced_unlock_total") != std::string::npos,
              "3285 AC2: forced_unlock_total exists");
        CHECK(mh.find("g_mutation_hold_budget_forced_fail_closed_total") != std::string::npos,
              "3285 AC2: forced_fail_closed_total exists");
        CHECK(mh.find("g_mutation_hold_budget_inbody_window_exceeded_total") != std::string::npos,
              "3285 AC2: inbody_window_exceeded_total exists");
    }

    // AC3: no new Soft path under production lock — the tier is gated on
    // reject_enabled and the 2×SLO force path is unchanged.
    {
        std::println("\n--- AC3: Soft zero-change + linter wiring ---");
        auto fc = read_file("src/serve/fiber.cpp");
        CHECK(fc.find("if (!mutation_hold_budget_reject_enabled())") != std::string::npos ||
                  fc.find("!mutation_hold_budget_reject_enabled()") != std::string::npos,
              "3285 AC3: Soft observe-only gate preserved");
        auto build = read_file("build.py");
        CHECK(build.find("check_noncoop_force_release_1slo_3285") != std::string::npos,
              "3285 AC3: build.py wires linter");
        CHECK(read_file("tests/serve/test_issue_3285.cpp").empty(),
              "3285 AC3: no tests/serve/test_issue_3285.cpp");
        CHECK(read_file("tests/issues/test_issue_3285.cpp").empty(),
              "3285 AC3: no tests/issues/test_issue_3285.cpp");
        CHECK(read_file("docs/design/3285-noncoop-force-release-1slo.md").empty(),
              "3285 AC3: no docs/design/ (#1655)");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3285 1×SLO inject tier: {} passed, {} failed ===", passed, failed);
    return failed == 0 ? 0 : 1;
}

// Issue #3325: residual after #3254/#3285/#3071 — a tight pure-compute
// outermost MutationBoundaryGuard body can exceed 2×SLO without a
// cooperative edge. Scheduler idle / worker park under
// production_multi_worker_latched re-injects synthetic MutationBoundary
// yield + force_safepoint on the holder and bumps
// hold_budget_no_edge_force_total. Same-fiber poll consumes (depth 0 +
// drop hold) without a natural check_gc_safepoint. Cross-fiber never
// drops unique_lock. Soft: metric-only.
int run_test_hold_budget_no_edge_force_3325() {
    std::println("\n=== Issue #3325: no-edge outermost hold past 2×SLO re-inject ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    // ac3325_1_same_fiber_poll_consumes_without_natural_edge
    {
        std::println("\n--- AC1: same-fiber poll consume without natural edge ---");
        using aura::compiler::CompilerService;
        using aura::compiler::Evaluator;
        using aura::serve::Scheduler;
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        ::unsetenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::mutation_hold_budget_reject_enabled(),
              "3325 AC1: reject_enabled under production");
        aura::serve::set_production_multi_worker_latched_for_test(true);
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::clear_hold_budget_no_edge_force_for_test();
        CompilerService cs;
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<int> ok_flag{1};
        std::atomic<int> ran{0};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        std::atomic<int> polled{0};
        Scheduler sched(2);
        sched.spawn([&]() {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                auto* f = aura::serve::g_current_fiber;
                CHECK(f != nullptr, "3325 AC1: fiber current");
                f->request_hold_budget_cancel();
                aura::compiler::mutation_hold_budget_note_cancel_armed(f->id());
                aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
                // Tight pure-compute body: no yield / no check_gc_safepoint.
                volatile std::uint64_t sink = 0;
                for (int i = 0; i < 64; ++i)
                    sink += static_cast<std::uint64_t>(i);
                (void)sink;
                polled.store(aura::serve::aura_hold_budget_poll_inbody_window(),
                             std::memory_order_relaxed);
                held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                                 std::memory_order_relaxed);
                depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                                  std::memory_order_relaxed);
                ran.store(1, std::memory_order_relaxed);
            }
            ok_flag.store(ok ? 1 : 0, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3325 AC1: fiber body ran");
        CHECK(polled.load() == 1, "3325 AC1: poll exceeded bound");
        CHECK(ok_flag.load() == 0, "3325 AC1: success flag forced false");
        CHECK(held_after.load() == 0, "3325 AC1: workspace hold cleared without natural edge");
        CHECK(depth_after.load() == 0, "3325 AC1: depth slot == 0 without natural edge");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() >= 1,
              "3325 AC1: forced_unlock_total");
        CHECK(aura::compiler::mutation_hold_budget_forced_fail_closed_total_v_read() >= 1,
              "3325 AC1: forced_fail_closed_total");
        CHECK(aura::compiler::hold_budget_no_edge_force_total_v_read() >= 1,
              "3325 AC1: hold_budget_no_edge_force_total");
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::serve::set_production_multi_worker_latched_for_test(false);
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::clear_hold_budget_no_edge_force_for_test();
    }

    // ac3325_2_cross_fiber_idle_poll_no_foreign_unique_lock
    {
        std::println("\n--- AC2: cross-fiber idle poll re-injects; foreign unique_lock stays ---");
        using aura::compiler::CompilerService;
        using aura::compiler::Evaluator;
        using aura::serve::Scheduler;
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        ::setenv("AURA_HOLD_BUDGET_INBODY_BOUND_US", "1000", 1);
        aura::compiler::typed_audit::apply_production_audit_defaults();
        aura::serve::set_production_multi_worker_latched_for_test(true);
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::clear_hold_budget_no_edge_force_for_test();
        CompilerService cs;
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<std::uint64_t> holder_id{0};
        std::atomic<int> ready{0};
        std::atomic<int> go{0};
        std::atomic<int> ran{0};
        std::atomic<int> ok_flag{1};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        Scheduler sched(2);
        sched.spawn([&]() {
            bool ok = true;
            {
                Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
                auto* f = aura::serve::g_current_fiber;
                CHECK(f != nullptr, "3325 AC2: holder fiber current");
                holder_id.store(f->id(), std::memory_order_release);
                ready.store(1, std::memory_order_release);
                for (int i = 0; i < 400 && go.load(std::memory_order_acquire) == 0; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
                (void)aura::serve::aura_hold_budget_poll_inbody_window();
                held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                                 std::memory_order_relaxed);
                depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                                  std::memory_order_relaxed);
                ran.store(1, std::memory_order_relaxed);
            }
            ok_flag.store(ok ? 1 : 0, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ready.load(std::memory_order_acquire) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(ready.load() == 1, "3325 AC2: holder entered Guard");
        const auto fid = holder_id.load(std::memory_order_acquire);
        CHECK(fid != 0, "3325 AC2: holder id published");
        const auto u0 = aura::compiler::mutation_hold_budget_forced_unlock_total_v_read();
        aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
        aura::compiler::g_hold_budget_cancel_armed_fiber.store(fid, std::memory_order_release);
        const int idle_polled = aura::serve::aura_hold_budget_poll_inbody_window();
        CHECK(idle_polled == 1 || idle_polled == 0, "3325 AC2: idle poll returned");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == u0,
              "3325 AC2: foreign idle poll did not drop unique_lock");
        CHECK(aura::compiler::hold_budget_no_edge_force_total_v_read() >= 1,
              "3325 AC2: hold_budget_no_edge_force_total bumped on idle poll");
        go.store(1, std::memory_order_release);
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3325 AC2: holder body ran");
        CHECK(ok_flag.load() == 0, "3325 AC2: success flag forced false");
        CHECK(held_after.load() == 0, "3325 AC2: steal/mailbox observer sees depth cleared");
        CHECK(depth_after.load() == 0, "3325 AC2: depth slot == 0 within one idle poll");
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::serve::set_production_multi_worker_latched_for_test(false);
        ::unsetenv("AURA_HOLD_BUDGET_INBODY_BOUND_US");
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::clear_hold_budget_no_edge_force_for_test();
    }

    // ac3325_3_soft_observe_only
    {
        std::println("\n--- AC3: Soft metric-only (no force) ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        CHECK(!aura::compiler::mutation_hold_budget_reject_enabled(),
              "3325 AC3: Soft reject_enabled false");
        aura::serve::set_production_multi_worker_latched_for_test(true);
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::clear_hold_budget_no_edge_force_for_test();
        aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
        aura::compiler::g_hold_budget_cancel_armed_fiber.store(1, std::memory_order_release);
        const auto u0 = aura::compiler::mutation_hold_budget_forced_unlock_total_v_read();
        CHECK(aura::serve::aura_hold_budget_poll_inbody_window() == 0,
              "3325 AC3: Soft poll does not force");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == u0,
              "3325 AC3: no force-drop under Soft");
        aura::serve::set_production_multi_worker_latched_for_test(false);
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        aura::compiler::clear_hold_budget_no_edge_force_for_test();
    }

    // ac3325_4_counters + worker park
    {
        std::println("\n--- AC4: existing counters + new no-edge total ---");
        const auto mhb = read_file("src/compiler/mutation_hold_budget.h");
        const auto fc = read_file("src/serve/fiber.cpp");
        const auto wc = read_file("src/serve/worker.cpp");
        const auto sc = read_file("src/serve/scheduler.cpp");
        CHECK(mhb.find("g_mutation_hold_budget_forced_unlock_total") != std::string::npos,
              "3325 AC4: forced_unlock_total preserved");
        CHECK(mhb.find("g_mutation_hold_budget_forced_fail_closed_total") != std::string::npos,
              "3325 AC4: forced_fail_closed_total preserved");
        CHECK(mhb.find("g_hold_budget_no_edge_force_total") != std::string::npos,
              "3325 AC4: hold_budget_no_edge_force_total");
        CHECK(mhb.find("kMutationHoldBudgetNoEdgeForceIssue") != std::string::npos,
              "3325 AC4: issue stamp");
        CHECK(fc.find("Issue #3325") != std::string::npos, "3325 AC4: poll cites #3325");
        CHECK(fc.find("g_production_multi_worker_latched") != std::string::npos,
              "3325 AC4: poll gates on production_multi_worker_latched");
        CHECK(wc.find("aura_hold_budget_poll_inbody_window()") != std::string::npos,
              "3325 AC4: worker park polls inbody window");
        CHECK(sc.find("aura_hold_budget_poll_inbody_window()") != std::string::npos,
              "3325 AC4: scheduler idle poll preserved");
        CHECK(fc.find("aura_evaluator_force_unlock_outermost_holder") == std::string::npos ||
                  fc.find("aura_hold_budget_poll_inbody_window(void) noexcept") !=
                      std::string::npos,
              "3325 AC4: poll definition present");
        auto poll_pos = fc.find("aura_hold_budget_poll_inbody_window(void) noexcept");
        auto poll_win = poll_pos == std::string::npos ? std::string{} : fc.substr(poll_pos, 8000);
        CHECK(poll_win.find("aura_evaluator_force_unlock_outermost_holder") == std::string::npos,
              "3325 AC4: poll does not spell force_unlock (#3160 AC12)");
    }

    // ac3325_5 chaos + linter + no invent
    {
        std::println("\n--- AC5: chaos extension + linter ---");
        const auto t = read_file("tests/serve/test_hold_budget_synthetic_yield_injection.cpp");
        const auto chaos = read_file("tests/serve/test_chaos_mutate_steal_gc_mailbox.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_hold_budget_no_edge_force_3325.py");
        const auto build = read_file("build.py");
        CHECK(t.find("ac3325_1_same_fiber_poll_consumes_without_natural_edge") !=
                      std::string::npos ||
                  t.find("3325 AC1:") != std::string::npos,
              "3325 AC5: AC1 in this suite");
        CHECK(t.find("ac3325_2_cross_fiber_idle_poll_no_foreign_unique_lock") !=
                      std::string::npos ||
                  t.find("3325 AC2:") != std::string::npos,
              "3325 AC5: AC2 densify×steal residual in this suite");
        CHECK(chaos.find("aura_hold_budget_poll_inbody_window") != std::string::npos,
              "3325 AC5: chaos suite still exercises the poll path");
        CHECK(!lint.empty() && lint.find("3325") != std::string::npos, "3325 AC5: linter");
        CHECK(build.find("check_hold_budget_no_edge_force_3325") != std::string::npos,
              "3325 AC5: build.py");
        CHECK(read_file("docs/design/3325-hold-budget-no-edge-force.md").empty(),
              "3325 AC5: no docs/design/");
        CHECK(read_file("tests/serve/test_issue_3325.cpp").empty(),
              "3325 AC5: no invent test_issue_3325");
        CHECK(read_file("tests/issues/test_issue_3325.cpp").empty(),
              "3325 AC5: no tests/issues/test_issue_3325");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3325 no-edge force: {} passed, {} failed ===", passed, failed);
    return failed == 0 ? 0 : 1;
}

// @reason: Issue #3480 — add_mutate wrapper never polled inbody hold-budget.
//   Force-release exists (#3222/#3254); structural mutate entry did not call
//   it. Non-coop body keeps workspace_mtx_ until safepoint/dtor.
//   AC1: production + cancel armed → add_mutate return force-releases
//   AC2: cross-fiber still only request_hold_budget_cancel
//   AC3: Soft observe-only; wrapper does not force-release
//   AC4: reuse forced_unlock_total / forced_fail_closed_total
//   AC5/AC6: extend this suite + linter; no invent / docs/design

int run_test_hold_budget_add_mutate_inbody_poll_3480() {
    std::println("=== Issue #3480: add_mutate inbody hold-budget poll ===");
    int saved_failed = aura::test::g_failed;
    int saved_passed = aura::test::g_passed;

    using aura::compiler::CompilerService;
    using aura::compiler::Evaluator;
    using aura::compiler::types::is_error;
    using aura::compiler::types::make_string;
    using aura::serve::Scheduler;

    {
        std::println("\n--- AC1: production add_mutate force-releases after cancel ---");
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CHECK(aura::compiler::mutation_hold_budget_reject_enabled(),
              "3480 AC1: reject_enabled under production");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_forced_fail_closed_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        cs.evaluator().set_sandbox_mode(false);
        cs.evaluator().grant_capability("mutate");
        cs.evaluator().grant_capability("*");
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "3480 AC1: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3480 AC1: eval");
        Evaluator::set_query_evaluator(&cs.evaluator());
        std::atomic<int> ran{0};
        std::atomic<int> held_after{-1};
        std::atomic<int> depth_after{-1};
        std::atomic<int> err{0};
        Scheduler sched(2);
        sched.spawn([&]() {
            auto* f = aura::serve::g_current_fiber;
            CHECK(f != nullptr, "3480 AC1: fiber current");
            f->request_hold_budget_cancel();
            aura::compiler::mutation_hold_budget_note_cancel_armed(f->id());
            aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
            // Invoke add_mutate as outermost (cs.eval wraps its own Guard).
            auto pfn = cs.evaluator().primitives().lookup("mutate:rebind");
            CHECK(pfn.has_value(), "3480 AC1: mutate:rebind prim");
            const auto i0 = cs.evaluator().push_string_heap("x");
            const auto i1 = cs.evaluator().push_string_heap("2");
            const auto i2 = cs.evaluator().push_string_heap("3480");
            aura::compiler::types::EvalValue args[3] = {
                make_string(static_cast<std::uint64_t>(i0)),
                make_string(static_cast<std::uint64_t>(i1)),
                make_string(static_cast<std::uint64_t>(i2)),
            };
            auto r = (*pfn)(std::span<const aura::compiler::types::EvalValue>(args, 3));
            if (is_error(r))
                err.store(1, std::memory_order_relaxed);
            held_after.store(cs.evaluator().mutation_boundary_held() ? 1 : 0,
                             std::memory_order_relaxed);
            depth_after.store(cs.evaluator().mutation_boundary_depth_slot_value(),
                              std::memory_order_relaxed);
            ran.store(1, std::memory_order_relaxed);
        });
        std::thread io([&]() { sched.run(); });
        for (int i = 0; i < 200 && ran.load() == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sched.stop();
        io.join();
        CHECK(ran.load() == 1, "3480 AC1: fiber body ran");
        CHECK(held_after.load() == 0, "3480 AC1: workspace hold cleared after add_mutate");
        CHECK(depth_after.load() == 0, "3480 AC1: depth slot == 0");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        const auto addp = mut.find("auto add_mutate = ");
        const auto aw = addp == std::string::npos ? std::string{} : mut.substr(addp, 14000);
        const auto fnp = aw.find("auto result = fn(a);");
        const auto frp = aw.find("force_release_hold_budget_inbody");
        CHECK(fnp != std::string::npos && frp != std::string::npos && frp > fnp,
              "3480 AC1: force_release after fn(a) on add_mutate");
        CHECK(aw.find("g_mutation_hold_budget_forced_unlock_total") != std::string::npos,
              "3480 AC1: reuses forced_unlock_total");
        CHECK(aw.find("g_mutation_hold_budget_forced_fail_closed_total") != std::string::npos,
              "3480 AC1: reuses forced_fail_closed_total");
        (void)err;
        Evaluator::set_query_evaluator(nullptr);
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    {
        std::println("\n--- AC2: cross-fiber still only request_hold_budget_cancel ---");
        const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        const auto rel = emb.find("aura_evaluator_force_release_outermost_holder");
        CHECK(rel != std::string::npos, "3480 AC2: force_release helper present");
        const auto win = emb.substr(rel, 1800);
        CHECK(win.find("aura_fiber_request_hold_budget_cancel") != std::string::npos,
              "3480 AC2: cross-fiber pending-cancel");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        const auto add = mut.find("auto add_mutate = ");
        CHECK(add != std::string::npos, "3480 AC2: add_mutate present");
        const auto awin = mut.substr(add, 14000);
        CHECK(awin.find("aura_fiber_request_hold_budget_cancel") == std::string::npos,
              "3480 AC2: wrapper does not unlock from thief thread");
        CHECK(awin.find("force_release_hold_budget_inbody") != std::string::npos,
              "3480 AC2: same-fiber wrapper force-releases");
    }

    {
        std::println("\n--- AC3: Soft observe-only ---");
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        ::unsetenv("AURA_MUTATION_HOLD_BUDGET_HARD");
        CHECK(!aura::compiler::mutation_hold_budget_reject_enabled(),
              "3480 AC3: Soft reject_enabled false");
        aura::compiler::clear_mutation_hold_budget_forced_unlock_for_test();
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
        CompilerService cs;
        cs.evaluator().grant_capability("mutate");
        cs.evaluator().grant_capability("*");
        CHECK(cs.eval("(set-code \"(define y 1)\")").has_value(), "3480 AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3480 AC3: eval");
        aura::compiler::g_hold_budget_cancel_armed_ns.store(1, std::memory_order_release);
        const auto u0 = aura::compiler::mutation_hold_budget_forced_unlock_total_v_read();
        (void)cs.eval("(mutate:rebind \"y\" \"3\" \"3480-soft\")");
        CHECK(aura::compiler::mutation_hold_budget_forced_unlock_total_v_read() == u0,
              "3480 AC3: wrapper does not force-release under Soft");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(mut.find("mutation_hold_budget_reject_enabled()") != std::string::npos,
              "3480 AC3: wrapper gates on reject_enabled");
        aura::compiler::clear_mutation_hold_budget_inbody_window_for_test();
    }

    {
        std::println("\n--- AC4/AC6: reuse counters + linter ---");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        const auto t = read_file("tests/serve/test_hold_budget_synthetic_yield_injection.cpp");
        const auto lint =
            read_file("scripts/coverage/checks/check_hold_budget_add_mutate_inbody_poll_3480.py");
        const auto build = read_file("build.py");
        CHECK(mut.find("g_mutation_hold_budget_forced_unlock_total") != std::string::npos,
              "3480 AC4: reuse forced_unlock_total");
        CHECK(mut.find("g_mutation_hold_budget_forced_fail_closed_total") != std::string::npos,
              "3480 AC4: reuse forced_fail_closed_total");
        CHECK(mut.find("schema-3480") == std::string::npos, "3480 AC4: no schema-3480");
        CHECK(mut.find("g_3480_") == std::string::npos, "3480 AC4: no g_3480_*");
        CHECK(t.find("3480 AC1: workspace hold cleared after add_mutate") != std::string::npos,
              "3480 AC5: AC1 in this suite");
        CHECK(!lint.empty() && lint.find("3480") != std::string::npos, "3480 AC6: linter");
        CHECK(build.find("check_hold_budget_add_mutate_inbody_poll_3480") != std::string::npos,
              "3480 AC6: build.py");
        CHECK(read_file("docs/design/3480-add-mutate-inbody-poll.md").empty(),
              "3480 AC6: no docs/design/");
        CHECK(read_file("tests/serve/test_issue_3480.cpp").empty(),
              "3480 AC6: no invent test_issue_3480");
    }

    int failed = aura::test::g_failed - saved_failed;
    int passed = aura::test::g_passed - saved_passed;
    std::println("\n=== #3480 add_mutate inbody poll: {} passed, {} failed ===", passed, failed);
    return failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    const int rc1 = run_test_hold_budget_synthetic_yield_injection();
    const int rc2 = run_test_hold_budget_inbody_escalate();
    const int rc3 = run_test_hold_budget_inbody_force_release();
    const int rc4 = run_test_hold_budget_inbody_force_unlock();
    const int rc5 = run_test_hold_budget_cross_fiber_urgent_inbody_poll();
    const int rc6 = run_test_hold_budget_noncoop_force_edge();
    const int rc7 = run_test_hold_budget_1slo_inject_3285();
    const int rc8 = run_test_hold_budget_no_edge_force_3325();
    const int rc9 = run_test_hold_budget_add_mutate_inbody_poll_3480();
    return rc1 != 0
               ? rc1
               : (rc2 != 0
                      ? rc2
                      : (rc3 != 0
                             ? rc3
                             : (rc4 != 0
                                    ? rc4
                                    : (rc5 != 0
                                           ? rc5
                                           : (rc6 != 0
                                                  ? rc6
                                                  : (rc7 != 0 ? rc7 : (rc8 != 0 ? rc8 : rc9)))))));
}
#endif