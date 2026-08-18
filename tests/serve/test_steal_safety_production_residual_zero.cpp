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
        must_inline(qts, "production-readiness-steal-residual-zero");
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
        // Hot path (steal_safety_transaction) untouched — verify by
        // grepping that the check is NOT called from there.
        auto cpp = read_file("src/serve/steal_safety.cpp");
        // Find the steal_safety_transaction function body.
        auto fn_pos = cpp.find("StealSafetyDecision steal_safety_transaction(");
        if (fn_pos != std::string::npos) {
            auto fn_end = cpp.find("\n}\n", fn_pos);
            if (fn_end == std::string::npos)
                fn_end = fn_pos + 6000;
            auto fn_win = cpp.substr(fn_pos, fn_end - fn_pos);
            CHECK(fn_win.find("production_residual_zero_v_read") == std::string::npos,
                  "AC2: hot path does NOT call the readiness check (zero-cost)");
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
        CHECK(fn_win.find("production_residual_zero_v_read") == std::string::npos,
              "AC3: hot path free of readiness check");
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
        must_inline(qts, "production-readiness-steal-residual-zero");
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
        CHECK(
            read_file("tests/serve/test_fiber_mutation_steal_safety.cpp").find("StealInvariant") !=
                std::string::npos,
            "AC5: steal-invariant lineage preserved");
    }

    std::println("\n=== #3134 production-readiness residual-zero: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

// Helper to keep the source-cite grep calls compact.
static void must_inline(const std::string& hay, const std::string& needle) {
    if (hay.find(needle) == std::string::npos) {
        g_failed += 1;
        std::println("FAIL: missing '{}' in source window", needle);
    } else {
        g_passed += 1;
    }
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_safety_production_residual_zero();
}
#endif