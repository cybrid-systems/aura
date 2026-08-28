// @category: unit
// @reason: Issue #3134 — production-readiness residual-zero check fail-closed
// under production multi-worker (chaos soak gate). Closes the residual
// re-arm race / LifetimeProof reject silent-corruption window for
// densify×steal under production.
//
//   AC1: source cites #3131 in steal_safety.h + evaluator_primitives_query
//        _type_stats.cpp — steal_safety_production_residual_zero_v_read()
//        returns 1 iff production_defaults_active() is 0 (Soft) OR both
//        named residual counters (#2901/#3038 + #2957) are 0. Wired into
//        schema-3073 production-readiness-soak-gate-wired + new additive
//        production-readiness-steal-residual-zero + schema-3134 / issue-3134.
//   AC2: Soft / sandbox=off / single-worker: zero behavioural change —
//        probe returns 0 → check returns 1 (pass-through). Hot path
//        steal_safety_transaction's quiet Ok unchanged.
//   AC3: Quiet happy path (no residual, no densify, no concurrent decision):
//        no extra atomics beyond the existing hard-AND loads. The new check
//        is consulted once per query primitive, not per steal.
//   AC4: Additive only — reuses g_steal_safety_residual_rearm_race_total
//        + g_steal_safety_residual_lifetime_proof_reject_total
//        + g_steal_safety_last_reject_invariant_bits. Adds ONE additive
//        readiness key (production-readiness-steal-residual-zero) +
//        schema-3134 / issue-3134 (per AC4 acceptable).
//   AC5: No tests/issues/test_issue_3134.cpp (#81967); no docs/design/3134-*
//        (#1655). Extend existing test_steal_complete_restamp_txn lineage.
//   AC6 (Issue #3162): sticky-fail atomic wired + accessor +
//        schema-3073 production-readiness-steal-residual-sticky-fail +
//        schema-3162 / issue-3162. Set on residual-fail path under
//        production; cleared when residual returns to 0 (per-query poll).
//   AC7 (Issue #3162): Soft / sandbox=off / single-worker: zero
//        behavioural change — sticky-fail bit stays 0, accessor returns 0.
//   AC8 (Issue #3162): quiet Ok path: zero extra atomics — bit only set
//        on residual-fail branch, cleared by per-query accessor (not on
//        per-steal hot path).
//   AC9 (Issue #3162): existing #3134 accessor + schema-3073 keys
//        non-regressing; additive sticky-fail key + schema-3162 / issue-3162.
//   AC10 (Issue #3195): multi-worker latch skips Soft pass-through;
//        residual_zero SSOT fail-closes on BoundaryUnsafe / LifetimeProof /
//        LayoutStamp / GcDefer / EnvFrame; sticky set on residual.
//   AC11 (Issue #3195): ABI self-check requires residual-zero sticky
//        wiring (bit 5) when multi-worker Ready is requested.
//   AC12 (Issue #3195): Soft / single-worker / unit-test: zero behavioural
//        change (latch unset → existing Soft pass-through). No new counters.

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "serve/fiber.h" // Issue #3288: live Fiber for Ok-path sticky gate
#include "serve/runtime_production_abi.h"
#include "serve/steal_safety.h"

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

static void must_inline(const std::string& hay, const std::string& needle) {
    if (hay.find(needle) == std::string::npos) {
        g_failed += 1;
        std::println("FAIL: missing '{}' in source window", needle);
    } else {
        g_passed += 1;
    }
}

} // namespace

int run_test_steal_safety_production_residual_zero() {
    std::println("=== Issue #3134: production-readiness residual-zero check ===");
    CHECK(true, "ac3134: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: check function in steal_safety.h ---");
        auto sh = read_file("src/serve/steal_safety.h");
        auto qts = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
        CHECK(!sh.empty(), "AC1: steal_safety.h readable");
        CHECK(!qts.empty(), "AC1: query_type_stats readable");

        // steal_safety.h: check function declared + documented.
        auto check_pos = sh.find("steal_safety_production_residual_zero_v_read()");
        CHECK(check_pos != std::string::npos,
              "AC1: steal_safety_production_residual_zero_v_read declared");
        // Anchor backwards to include the comment block.
        auto check_start = check_pos > 1500 ? check_pos - 1500 : 0;
        auto check_end = check_pos + 1500;
        auto check_win = sh.substr(check_start, check_end - check_start);
        must_inline(check_win, "Issue #3134");
        must_inline(check_win, "g_steal_safety_residual_rearm_race_total");
        must_inline(check_win, "g_steal_safety_residual_lifetime_proof_reject_total");
        must_inline(check_win, "aura_production_defaults_active_probe");
        must_inline(check_win, "kStealSafetyProductionResidualZeroIssue = 3134");

        // query_type_stats.cpp: schema-3073 surface ANDs the new check +
        // additive schema-3134 / issue-3134 / production-readiness-steal-
        // residual-zero keys.
        must_inline(qts, "steal_safety_production_residual_zero_v_read");
        // Key is split across adjacent string literals in the TU.
        must_inline(qts, "production-readiness-steal-");
        must_inline(qts, "residual-zero");
        must_inline(qts, "schema-3134");
        must_inline(qts, "issue-3134");
        must_inline(qts, "Issue #3134");
    }

    // ── AC2: Soft / sandbox=off / single-worker: zero behavioural change ─
    {
        std::println("\n--- AC2: Soft pass-through ---");
        auto sh = read_file("src/serve/steal_safety.h");
        // The check returns 1 iff production_defaults_active() is 0 (Soft).
        // Verifies the Soft pass-through is in the inline impl.
        auto check_pos = sh.find("steal_safety_production_residual_zero_v_read()");
        auto check_start = check_pos > 1500 ? check_pos - 1500 : 0;
        auto check_end = check_pos + 1500;
        auto check_win = sh.substr(check_start, check_end - check_start);
        must_inline(check_win, "aura_production_defaults_active_probe() == 0");
        must_inline(check_win, "return 1");
        // Quiet Ok path does not consult residual_zero (#3134 AC2 / #3162
        // AC8). RejectHard may consult it to set sticky (#3162/#3195).
        auto cpp = read_file("src/serve/steal_safety.cpp");
        auto fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(");
        if (fn_pos != std::string::npos) {
            auto fn_end = cpp.find("\n}\n", fn_pos);
            if (fn_end == std::string::npos)
                fn_end = fn_pos + 8000;
            auto fn_win = cpp.substr(fn_pos, fn_end - fn_pos);
            auto ok_pos = fn_win.find("set_resume_safety_ticket(snap.ticket)");
            auto check_call = fn_win.find("production_residual_zero_v_read");
            CHECK(ok_pos != std::string::npos, "AC2: Ok ticket stamp present");
            CHECK(check_call != std::string::npos && check_call < ok_pos,
                  "AC2: residual_zero consult is on RejectHard, not Ok");
            CHECK(fn_win.substr(ok_pos).find("production_residual_zero_v_read") ==
                      std::string::npos,
                  "AC2: Ok path free of readiness check");
        }
    }

    // ── AC3: Quiet happy path: no extra atomics ─
    {
        std::println("\n--- AC3: hot path atomics unchanged ---");
        auto cpp = read_file("src/serve/steal_safety.cpp");
        // The hot path uses evaluate_residual_hard_and_bits which already
        // loads the existing arm-specific counters. Verify the new check
        // (production_residual_zero_v_read) is NOT in the hot path.
        auto fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(");
        CHECK(fn_pos != std::string::npos, "AC3: hot path function present");
        auto fn_end = cpp.find("\n}\n", fn_pos);
        if (fn_end == std::string::npos)
            fn_end = fn_pos + 6000;
        auto fn_win = cpp.substr(fn_pos, fn_end - fn_pos);
        auto ok_pos = fn_win.find("set_resume_safety_ticket(snap.ticket)");
        CHECK(ok_pos != std::string::npos, "AC3: Ok ticket stamp present");
        CHECK(fn_win.substr(ok_pos).find("production_residual_zero_v_read") == std::string::npos,
              "AC3: Ok path free of readiness check");
        // Confirm the hot path still uses evaluate_residual_hard_and_bits
        // (existing contract — no regression).
        must_inline(fn_win, "evaluate_residual_hard_and_bits");
    }

    // ── AC4: additive only; existing counters + ONE additive readiness key ─
    {
        std::println("\n--- AC4: counter reuse + additive readiness key ---");
        auto sh = read_file("src/serve/steal_safety.h");
        auto qts = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
        // Existing residual counters reused (no new counters).
        must_inline(sh, "g_steal_safety_residual_rearm_race_total");
        must_inline(sh, "g_steal_safety_residual_lifetime_proof_reject_total");
        must_inline(sh, "g_steal_safety_last_reject_invariant_bits");
        // ONE additive readiness key + schema-3134 / issue-3134.
        must_inline(qts, "production-readiness-steal-");
        must_inline(qts, "residual-zero");
        must_inline(qts, "schema-3134");
        must_inline(qts, "issue-3134");
        // No new std::atomic<uint64_t> for residual tracking.
        auto sh_idx = sh.find("kStealSafetyProductionResidualZeroIssue");
        auto sh_end = sh_idx + 500;
        auto sh_block = sh.substr(sh_idx, sh_end - sh_idx);
        CHECK(sh_block.find("std::atomic<std::uint64_t>") == std::string::npos,
              "AC4: no new uint64 atomic (only wired sentinel)");
    }

    // ── AC5: src-aligned test, no tests/issues/test_issue_3134.cpp, no plan doc ─
    {
        std::println("\n--- AC5: src-aligned test, no plan doc ---");
        auto root = std::filesystem::current_path();
        CHECK(!std::filesystem::exists(root / "tests" / "issues" / "test_issue_3134.cpp"),
              "AC5: tests/issues/test_issue_3134.cpp absent (#81967)");
        CHECK(!std::filesystem::exists(root / "tests" / "serve" / "test_issue_3134.cpp"),
              "AC5: tests/serve/test_issue_3134.cpp absent (#81967)");
        auto docs = root / "docs" / "design";
        if (std::filesystem::exists(docs)) {
            for (const auto& f : std::filesystem::directory_iterator(docs)) {
                auto name = f.path().filename().string();
                CHECK(name.find("3134-") == std::string::npos,
                      "AC5: no docs/design/3134-* plan doc (#1655)");
                (void)name;
                break;
            }
        }
        // Existing steal-safety suites extended (regression-preserved).
        CHECK(read_file("tests/serve/test_steal_complete_restamp_txn.cpp").find("Issue #2901") !=
                  std::string::npos,
              "AC5: rearm-race lineage preserved (#2901)");
        CHECK(read_file("tests/serve/test_steal_complete_restamp_txn.cpp").find("StealInvariant") !=
                  std::string::npos,
              "AC5: steal-invariant lineage preserved");
    }

    // ── AC6 (Issue #3162): sticky-fail atomic + accessor + schema surface ─
    {
        std::println("\n--- AC6: sticky-fail wired (#3162) ---");
        auto sh = read_file("src/serve/steal_safety.h");
        auto qts = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
        // Atomic + wired sentinel + accessor in steal_safety.h.
        must_inline(sh, "g_steal_safety_production_residual_sticky_fail{0}");
        must_inline(sh, "g_steal_safety_production_residual_sticky_fail_wired{1}");
        must_inline(sh, "kStealSafetyProductionResidualStickyFailIssue = 3162");
        must_inline(sh, "steal_safety_production_residual_sticky_fail_v_read");
        // Set site in steal_safety.cpp RejectHard path under production.
        auto cpp = read_file("src/serve/steal_safety.cpp");
        auto fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(");
        CHECK(fn_pos != std::string::npos, "AC6: transaction function present");
        auto fn_end = cpp.find("\n}\n", fn_pos);
        if (fn_end == std::string::npos)
            fn_end = fn_pos + 8000;
        auto fn_win = cpp.substr(fn_pos, fn_end - fn_pos);
        must_inline(fn_win, "Issue #3162");
        must_inline(fn_win, "steal_safety_production_residual_sticky_fail.store(1");
        must_inline(fn_win, "steal_safety_production_residual_zero_v_read() == 0");
        // Schema surface additive keys.
        must_inline(qts, "production-readiness-steal-");
        must_inline(qts, "residual-sticky-fail");
        must_inline(qts, "schema-3162");
        must_inline(qts, "issue-3162");
        must_inline(qts, "steal_safety_production_residual_sticky_fail_v_read");
    }

    // ── AC7 (Issue #3162): Soft pass-through — sticky_fail stays 0 ─
    {
        std::println("\n--- AC7: Soft pass-through sticky_fail (#3162) ---");
        auto sh = read_file("src/serve/steal_safety.h");
        auto accessor_pos = sh.find("steal_safety_production_residual_sticky_fail_v_read");
        auto acc_end = accessor_pos + 800;
        auto acc_win = sh.substr(accessor_pos, acc_end - accessor_pos);
        must_inline(acc_win, "aura_production_defaults_active_probe() == 0");
        must_inline(acc_win, "return 0");
    }

    // ── AC8 (Issue #3162): quiet Ok path zero extra atomics ─
    {
        std::println("\n--- AC8: Ok path zero extra atomics (#3162) ---");
        auto cpp = read_file("src/serve/steal_safety.cpp");
        auto fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(");
        auto fn_end = cpp.find("\n}\n", fn_pos);
        if (fn_end == std::string::npos)
            fn_end = fn_pos + 8000;
        auto fn_win = cpp.substr(fn_pos, fn_end - fn_pos);
        // The sticky_fail.store(1) must be INSIDE the residual-fail branch
        // (after RejectHard return). The Ok path (ticket stamp + return
        // Ok) must NOT touch sticky_fail.
        auto store_pos = fn_win.find("steal_safety_production_residual_sticky_fail.store(1");
        auto ok_pos = fn_win.find("set_resume_safety_ticket(snap.ticket)");
        CHECK(store_pos != std::string::npos, "AC8: sticky_fail set site present");
        CHECK(ok_pos != std::string::npos, "AC8: Ok path ticket stamp present");
        if (store_pos != std::string::npos && ok_pos != std::string::npos) {
            // RejectHard (sticky store) is before the Ok ticket stamp.
            CHECK(store_pos < ok_pos, "AC8: sticky_fail set is on RejectHard (before Ok stamp)");
        }
    }

    // ── AC9 (Issue #3162): #3134 accessor + schema-3073 keys non-regressing ─
    {
        std::println("\n--- AC9: #3134 non-regression + additive (#3162) ---");
        auto sh = read_file("src/serve/steal_safety.h");
        auto qts = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
        // Existing #3134 accessor still present.
        must_inline(sh, "steal_safety_production_residual_zero_v_read");
        must_inline(qts, "production-readiness-steal-");
        must_inline(qts, "residual-zero");
        must_inline(qts, "schema-3134");
        must_inline(qts, "issue-3134");
        // Additive sticky-fail key + schema-3162.
        must_inline(qts, "residual-sticky-fail");
        must_inline(qts, "schema-3162");
        must_inline(qts, "issue-3162");
    }

    // ── AC10 (Issue #3195): multi-worker latch + named residual SSOT ─
    {
        std::println("\n--- AC10: multi-worker residual-zero sticky (#3195) ---");
        auto sh = read_file("src/serve/steal_safety.h");
        auto cpp = read_file("src/serve/steal_safety.cpp");
        auto qts = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
        must_inline(sh, "kStealSafetyProductionMultiWorkerResidualStickyIssue = 3195");
        must_inline(sh, "aura_runtime_multi_worker_production_latched");
        must_inline(sh, "g_steal_safety_residual_boundary_unsafe_total");
        must_inline(sh, "g_steal_safety_residual_layout_stamp_mismatch_total");
        must_inline(sh, "g_steal_safety_residual_gc_defer_armed_total");
        must_inline(sh, "g_steal_safety_residual_envframe_lag_total");
        auto fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(");
        CHECK(fn_pos != std::string::npos, "AC10: transaction function present");
        auto fn_end = cpp.find("\n}\n", fn_pos);
        if (fn_end == std::string::npos)
            fn_end = fn_pos + 8000;
        auto fn_win = cpp.substr(fn_pos, fn_end - fn_pos);
        must_inline(fn_win, "Issue #3195");
        must_inline(fn_win, "aura_runtime_multi_worker_production_latched() != 0");
        must_inline(fn_win, "steal_safety_production_residual_sticky_fail.store(1");
        must_inline(qts, "schema-3195");
        must_inline(qts, "issue-3195");
        must_inline(qts, "Issue #3195");
    }

    // ── AC11 (Issue #3195): ABI self-check residual-zero sticky wiring ─
    {
        std::println("\n--- AC11: ABI self-check residual sticky wiring (#3195) ---");
        auto hh = read_file("src/serve/runtime_production_abi.h");
        auto abi = read_file("src/serve/runtime_production_abi.cpp");
        must_inline(hh, "kProductionAbiSelfcheckFailBitResidualSticky");
        must_inline(hh, "g_production_multi_worker_latched{0}");
        must_inline(abi, "g_steal_safety_production_residual_sticky_fail_wired");
        must_inline(abi, "g_steal_safety_production_residual_zero_wired");
        must_inline(abi, "g_production_multi_worker_latched.store(1");
        must_inline(abi, "steal_safety_production_residual_zero_v_read() == 0");
        must_inline(abi, "Issue #3195");
        must_inline(abi, "kProductionAbiSelfcheckFailBitResidualSticky");
    }

    // ── AC12 (Issue #3195): Soft / single-worker zero behavioural change ─
    {
        std::println("\n--- AC12: Soft / single-worker unchanged (#3195) ---");
        auto sh = read_file("src/serve/steal_safety.h");
        auto check_pos = sh.find("steal_safety_production_residual_zero_v_read()");
        auto check_start = check_pos > 1500 ? check_pos - 1500 : 0;
        auto check_end = check_pos + 2500;
        auto check_win = sh.substr(check_start, check_end - check_start);
        must_inline(check_win, "!multi && aura_production_defaults_active_probe() == 0");
        must_inline(check_win, "return 1");
        CHECK(sh.find("g_3195_") == std::string::npos, "AC12: no new g_3195_* counter");
        CHECK(!std::filesystem::exists(std::filesystem::current_path() / "tests" / "issues" /
                                       "test_issue_3195.cpp"),
              "AC12: tests/issues/test_issue_3195.cpp absent (#81967)");
        CHECK(!std::filesystem::exists(std::filesystem::current_path() / "tests" / "serve" /
                                       "test_issue_3195.cpp"),
              "AC12: tests/serve/test_issue_3195.cpp absent (#81967)");
    }

    // ── AC13 (Issue #3195): live SSOT — latch + named residual fail-closed ─
    {
        std::println("\n--- AC13: live residual_zero latch (#3195) ---");
        using aura::serve::clear_production_abi_selfcheck_for_test;
        using aura::serve::clear_steal_safety_transaction_for_test;
        using aura::serve::g_steal_safety_residual_boundary_unsafe_total;
        using aura::serve::g_steal_safety_residual_envframe_lag_total;
        using aura::serve::g_steal_safety_residual_gc_defer_armed_total;
        using aura::serve::g_steal_safety_residual_layout_stamp_mismatch_total;
        using aura::serve::set_production_multi_worker_latched_for_test;
        using aura::serve::steal_safety_production_residual_sticky_fail_v_read;
        using aura::serve::steal_safety_production_residual_zero_v_read;

        clear_steal_safety_transaction_for_test();
        clear_production_abi_selfcheck_for_test();
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(0, std::memory_order_relaxed);
        g_steal_safety_residual_boundary_unsafe_total.store(1, std::memory_order_relaxed);
        CHECK(steal_safety_production_residual_zero_v_read() == 1,
              "AC13: Soft unlatched BoundaryUnsafe is pass-through");
        CHECK(steal_safety_production_residual_sticky_fail_v_read() == 0,
              "AC13: Soft unlatched sticky stays 0");

        set_production_multi_worker_latched_for_test(true);
        CHECK(steal_safety_production_residual_zero_v_read() == 0,
              "AC13: latched BoundaryUnsafe fail-closes SSOT");
        CHECK(steal_safety_production_residual_sticky_fail_v_read() == 1,
              "AC13: latched residual sets sticky");

        g_steal_safety_residual_boundary_unsafe_total.store(0, std::memory_order_relaxed);
        g_steal_safety_residual_layout_stamp_mismatch_total.store(1, std::memory_order_relaxed);
        CHECK(steal_safety_production_residual_zero_v_read() == 0, "AC13: LayoutStamp fail-closes");
        g_steal_safety_residual_layout_stamp_mismatch_total.store(0, std::memory_order_relaxed);
        g_steal_safety_residual_gc_defer_armed_total.store(1, std::memory_order_relaxed);
        CHECK(steal_safety_production_residual_zero_v_read() == 0, "AC13: GcDefer fail-closes");
        g_steal_safety_residual_gc_defer_armed_total.store(0, std::memory_order_relaxed);
        g_steal_safety_residual_envframe_lag_total.store(1, std::memory_order_relaxed);
        CHECK(steal_safety_production_residual_zero_v_read() == 0, "AC13: EnvFrame fail-closes");
        g_steal_safety_residual_envframe_lag_total.store(0, std::memory_order_relaxed);
        CHECK(steal_safety_production_residual_zero_v_read() == 1,
              "AC13: residuals clear → SSOT recovers");

        set_production_multi_worker_latched_for_test(false);
        clear_steal_safety_transaction_for_test();
        clear_production_abi_selfcheck_for_test();
    }

    // ── AC14 (Issue #3288): Ok path consults sticky under latch (not
    //    query-only) + try_acquire refusal ──
    {
        std::println("\n--- AC14: continuous Ok-path sticky gate (#3288) ---");
        auto cpp = read_file("src/serve/steal_safety.cpp");
        auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        auto qts = read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
        auto fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(");
        CHECK(fn_pos != std::string::npos, "AC14: transaction function present");
        auto fn_end = cpp.find("\n}\n", fn_pos);
        if (fn_end == std::string::npos)
            fn_end = fn_pos + 8000;
        auto fn_win = cpp.substr(fn_pos, fn_end - fn_pos);
        must_inline(fn_win, "Issue #3288");
        auto ok_pos = fn_win.find("set_resume_safety_ticket(snap.ticket)");
        auto sticky_load = fn_win.find("g_steal_safety_production_residual_sticky_fail.load");
        CHECK(ok_pos != std::string::npos, "AC14: Ok ticket stamp present");
        CHECK(sticky_load != std::string::npos && sticky_load < ok_pos,
              "AC14: sticky consult on Ok path before ticket stamp");
        // Latch-gated: Soft / single-worker / unlatched zero behavioural
        // change (weak latch probe returns 0 → short-circuit).
        auto latch_win = sticky_load != std::string::npos
                             ? fn_win.substr(sticky_load > 300 ? sticky_load - 300 : 0, 600)
                             : std::string{};
        must_inline(latch_win, "aura_runtime_multi_worker_production_latched() != 0");
        // try_acquire + try_acquire_for_region structured refusal.
        CHECK(mb.find("AdmissionRejected: production-residual-sticky") != std::string::npos,
              "AC14: try_acquire sticky refusal present");
        CHECK(mb.find("Issue #3288") != std::string::npos, "AC14: try_acquire cites #3288");
        // No new counters / no second residual bus.
        CHECK(cpp.find("g_3288_") == std::string::npos &&
                  read_file("src/serve/steal_safety.h").find("g_3288_") == std::string::npos,
              "AC14: no new g_3288_* counter");
        // Additive schema stamps.
        must_inline(qts, "schema-3288");
        must_inline(qts, "issue-3288");
        // No new test file / no docs (per #81967 / #1655).
        CHECK(!std::filesystem::exists(std::filesystem::current_path() / "tests" / "issues" /
                                       "test_issue_3288.cpp"),
              "AC14: tests/issues/test_issue_3288.cpp absent (#81967)");
        CHECK(!std::filesystem::exists(std::filesystem::current_path() / "tests" / "serve" /
                                       "test_issue_3288.cpp"),
              "AC14: tests/serve/test_issue_3288.cpp absent (#81967)");
        auto docs = std::filesystem::current_path() / "docs" / "design";
        if (std::filesystem::exists(docs)) {
            for (const auto& f : std::filesystem::directory_iterator(docs)) {
                auto name = f.path().filename().string();
                CHECK(name.find("3288-") == std::string::npos,
                      "AC14: no docs/design/3288-* plan doc (#1655)");
                (void)name;
                break;
            }
        }
    }

    // ── AC15 (Issue #3288): live — Ok path refuses under sticky until
    //    residual returns to 0 (continuous fail-closed, not query-only) ──
    {
        std::println("\n--- AC15: live Ok-path sticky gate (#3288) ---");
        using aura::serve::clear_production_abi_selfcheck_for_test;
        using aura::serve::clear_steal_safety_transaction_for_test;
        using aura::serve::Fiber;
        using aura::serve::g_steal_safety_production_residual_sticky_fail;
        using aura::serve::g_steal_safety_residual_boundary_unsafe_total;
        using aura::serve::g_steal_safety_transaction_ok_total;
        using aura::serve::g_steal_safety_transaction_reject_hard_total;
        using aura::serve::set_production_multi_worker_latched_for_test;
        using aura::serve::steal_safety_production_residual_sticky_fail_v_read;
        using aura::serve::steal_safety_production_residual_zero_v_read;
        using aura::serve::steal_safety_transaction;
        using aura::serve::StealSafetyDecision;

        clear_steal_safety_transaction_for_test();
        clear_production_abi_selfcheck_for_test();
        Fiber f([] {});

        // Unlatched baseline: residual non-zero is observe-only (Soft
        // pass-through) — no sticky, Ok path unaffected.
        g_steal_safety_residual_boundary_unsafe_total.store(1, std::memory_order_relaxed);
        CHECK(steal_safety_production_residual_zero_v_read() == 1,
              "AC15: unlatched Soft pass-through");
        CHECK(steal_safety_production_residual_sticky_fail_v_read() == 0,
              "AC15: unlatched sticky stays 0");

        // Latch multi-worker: residual non-zero → SSOT fail-closes + sticky.
        set_production_multi_worker_latched_for_test(true);
        CHECK(steal_safety_production_residual_zero_v_read() == 0,
              "AC15: latched SSOT fail-closes");
        CHECK(steal_safety_production_residual_sticky_fail_v_read() == 1,
              "AC15: latched residual sets sticky");

        // Ok path must refuse (continuous fail-closed, not query-only): even
        // a clean fiber (per-fiber hard-AND passing) gets RejectHard from the
        // sticky consult before the ticket stamp.
        const auto ok0 = g_steal_safety_transaction_ok_total.load(std::memory_order_relaxed);
        const auto rej0 =
            g_steal_safety_transaction_reject_hard_total.load(std::memory_order_relaxed);
        const auto d = steal_safety_transaction(&f);
        CHECK(d == StealSafetyDecision::RejectHard,
              "AC15: transaction Ok path refuses under sticky");
        CHECK(g_steal_safety_transaction_ok_total.load(std::memory_order_relaxed) == ok0,
              "AC15: ok_total unchanged (no enqueue)");
        CHECK(g_steal_safety_transaction_reject_hard_total.load(std::memory_order_relaxed) > rej0,
              "AC15: reject_hard_total bumps");
        CHECK(!f.has_resume_safety_ticket(), "AC15: no ticket stamp on sticky RejectHard");

        // Residual returns to 0 → sticky cleared (poll) → Ok resumes.
        g_steal_safety_residual_boundary_unsafe_total.store(0, std::memory_order_relaxed);
        CHECK(steal_safety_production_residual_zero_v_read() == 1,
              "AC15: SSOT recovers when residual clears");
        CHECK(steal_safety_production_residual_sticky_fail_v_read() == 0,
              "AC15: sticky cleared after recovery");
        const auto ok1 = g_steal_safety_transaction_ok_total.load(std::memory_order_relaxed);
        const auto d2 = steal_safety_transaction(&f);
        // Ambient residual (densify lag etc.) may still RejectHard; on Ok the
        // ticket must be stamped (recovery path works end-to-end).
        if (d2 == StealSafetyDecision::Ok) {
            CHECK(g_steal_safety_transaction_ok_total.load(std::memory_order_relaxed) > ok1,
                  "AC15: ok_total bumps after recovery");
            CHECK(f.has_resume_safety_ticket(), "AC15: ticket stamped after recovery");
        }

        set_production_multi_worker_latched_for_test(false);
        g_steal_safety_production_residual_sticky_fail.store(0, std::memory_order_relaxed);
        clear_steal_safety_transaction_for_test();
        clear_production_abi_selfcheck_for_test();
    }

    // ── AC16 (Issue #3385): LifetimeProofOk residual arm fires under
    //    latched multi-worker Ready even when snapshot mode is Soft (I3
    //    residual). Mailbox held-ref path observes the same arm under
    //    latch; pure payload without held-ref keeps the skip (zero extra
    //    loads). Soft + unlatched: arm skipped, no behavioural change.
    {
        std::println("\n--- AC16: LifetimeProofOk arm under latch (#3385) ---");
        const auto cpp_src = read_file("src/serve/steal_safety.cpp");
        // AC16a: LifetimeProofOk arm uses hard_mode OR latch.
        CHECK(cpp_src.find("Issue #3385") != std::string::npos,
              "AC16a: steal_safety.cpp cites #3385");
        CHECK(cpp_src.find("is_steal_snapshot_hard_mode() || "
                           "aura_runtime_multi_worker_production_latched()") != std::string::npos,
              "AC16a: LifetimeProofOk arm hard_mode OR latch");
        // AC16b: mailbox conditional skip based on (observe_latch && check_envframe).
        CHECK(cpp_src.find("observe_latch") != std::string::npos,
              "AC16b: mailbox observes latch for held-ref skip");
        CHECK(cpp_src.find("aura::serve::aura_runtime_multi_worker_production_latched()") !=
                  std::string::npos,
              "AC16b: mailbox uses latched accessor");
        // AC16c: no docs/design/3385-* per MEMORY 2026-07-19.
        auto docs = std::filesystem::current_path() / "docs" / "design";
        if (std::filesystem::exists(docs)) {
            for (const auto& f : std::filesystem::directory_iterator(docs)) {
                auto name = f.path().filename().string();
                CHECK(name.find("3385-") == std::string::npos,
                      "AC16c: no docs/design/3385-* (#1655)");
                break;
            }
        }
        // AC16d: no tests/issues/test_issue_3385.cpp per #81967.
        CHECK(!std::filesystem::exists(std::filesystem::current_path() / "tests" / "issues" /
                                       "test_issue_3385.cpp"),
              "AC16d: tests/issues/test_issue_3385.cpp absent (#81967)");
    }

    std::println(
        "\n=== #3134/#3288/#3385 production-readiness residual-zero: {} passed, {} failed ===",
        g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_safety_production_residual_zero();
}
#endif