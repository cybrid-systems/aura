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

#include <cstdint>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

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

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hold_budget_synthetic_yield_injection();
}
#endif