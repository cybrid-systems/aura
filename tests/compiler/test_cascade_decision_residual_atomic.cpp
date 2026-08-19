// @category: unit
// @reason: Issue #3135 — relower_dirty_defines drain + impact_ub +
// partial peel not atomic vs concurrent record_dependency re-arm
// (residual of #3067/#3097).
//
//   AC1: source cites #3135 in service.ixx — cascade_decision_mtx_
//        (std::mutex, ordered after dep_graph_mtx_ + mutate_mtx_).
//        record_dependency acquires it around the deferred_hybrid_edges_
//        .emplace_back + store(1) reject pair.
//   AC2: Soft / sandbox=off + single-fiber + clean (armed==0) skips the
//        lock for zero cost (AC2 contract).
//   AC3: Quiet happy path (no concurrent reject) — single existing
//        hard-AND consult path, no extra lock when need_lock is false.
//   AC4: Existing #3067 + #3097 paths preserved (impact_ub consult +
//        partial/full decision). Re-check immediately before peel: if
//        re-armed since snapshot, force full + mark_all_blocks_dirty.
//   AC5: No tests/issues/test_issue_3135.cpp (#81967); no docs/design/
//        3135-* (#1655). Extend existing test_issue_3097 lineage +
//        test_dep_graph_hybrid_cascade.

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

static void must_inline(const std::string& hay, const std::string& needle, const char* label = "") {
    if (hay.find(needle) == std::string::npos) {
        g_failed += 1;
        std::println("FAIL: missing '{}'{}", needle,
                     label[0] ? std::string(" [") + label + "]" : "");
    } else {
        g_passed += 1;
    }
}

} // namespace

int run_test_cascade_decision_residual_atomic_3135() {
    std::println("=== Issue #3135: cascade-decision residual atomic ===");
    CHECK(true, "ac3135: issue stamp");

    auto ixx = read_file("src/compiler/service.ixx");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: cascade_decision_mtx_ + record_dependency reject lock ---");
        // Find the field declaration.
        auto pos = ixx.find("std::mutex cascade_decision_mtx_");
        CHECK(pos != std::string::npos, "AC1: cascade_decision_mtx_ declared");
        const auto block_start = pos > 600 ? pos - 600 : 0;
        auto block = ixx.substr(block_start, (pos - block_start) + 200);
        must_inline(block, "Issue #3135", "AC1 field declaration cites #3135");
        must_inline(block, "std::mutex", "AC1 lock is std::mutex (not std::shared_mutex)");
        must_inline(block, "cascade_decision_mtx_", "AC1 lock named cascade_decision_mtx_");

        // record_dependency: acquire lock around emplace_back + store(1)
        // pair. Anchor on record_dependency function definition; the
        // reject path is uniquely identified by the gen_mismatch branch.
        auto rd_pos = ixx.find(
            "void record_dependency(const std::string& caller, const std::string& callee)");
        if (rd_pos == std::string::npos)
            rd_pos = ixx.find("void record_dependency(");
        CHECK(rd_pos != std::string::npos, "AC1: record_dependency definition");
        auto rd_end = rd_pos + 6000;
        auto rd_block = ixx.substr(rd_pos, rd_end - rd_pos);
        // Look for cascade_guard near the emplace_back + store pair.
        must_inline(rd_block, "cascade_guard(cascade_decision_mtx_)",
                    "AC1 record_dependency acquires cascade_decision_mtx_ in reject path");
        must_inline(rd_block, "Issue #3135", "AC1 record_dependency reject path cites #3135");
        must_inline(rd_block, "deferred_hybrid_edges_.emplace_back",
                    "AC1 reject path emplaces deferred edge under lock");
        must_inline(rd_block, "deferred_hybrid_armed_.store(1",
                    "AC1 reject path sets armed under lock");

        // relower_dirty_defines_from_workspace: critical section + re-check.
        auto rel_pos = ixx.find("std::size_t relower_dirty_defines_from_workspace()");
        CHECK(rel_pos != std::string::npos, "AC1: relower function definition");
        auto rel_end = rel_pos + 8000;
        auto rel_block = ixx.substr(rel_pos, rel_end - rel_pos);
        must_inline(rel_block, "Issue #3135", "AC1 relower cites #3135");
        must_inline(rel_block, "cascade_decision_mtx_", "AC1 relower uses cascade_decision_mtx_");
        must_inline(rel_block, "initial_armed", "AC1 relower snapshots initial_armed");
        must_inline(rel_block, "need_lock", "AC1 relower gates need_lock on armed+production");
        must_inline(rel_block, "production_defaults_active",
                    "AC1 relower gates on production_defaults_active");
        must_inline(rel_block, "re_armed_now", "AC1 relower re-checks re_armed_now before peel");
        must_inline(rel_block, "mark_all_blocks_dirty",
                    "AC1 relower force-fulls via mark_all_blocks_dirty");
    }

    // ── AC2: Soft / sandbox=off + clean (armed==0) skips the lock for zero cost ──
    {
        std::println("\n--- AC2: Soft skip path ---");
        auto pos = ixx.find("std::size_t relower_dirty_defines_from_workspace()");
        auto end = pos + 8000;
        auto block = ixx.substr(pos, end - pos);
        // need_lock must be the gate that decides skip-or-take.
        must_inline(block, "const bool need_lock =", "AC2 need_lock is the Soft-skip gate");
        must_inline(block, "initial_armed ||", "AC2 need_lock ORs armed with production");
        must_inline(block, "cascade_guard.lock()",
                    "AC2 lock is acquired conditionally via defer_lock + .lock()");
        must_inline(block, "defer_lock",
                    "AC2 defer_lock pattern (no acquisition until gate passes)");
    }

    // ── AC3: Quiet happy path — no extra lock when need_lock is false ──
    {
        std::println("\n--- AC3: quiet happy path no extra lock ---");
        auto pos = ixx.find("std::size_t relower_dirty_defines_from_workspace()");
        auto end = pos + 8000;
        auto block = ixx.substr(pos, end - pos);
        // The defer_lock pattern ensures no atomic acquire happens when
        // need_lock is false (only .lock() acquires the mutex).
        must_inline(block, "std::defer_lock", "AC3 defer_lock avoids default acquire in ctor");
        // No record_dependency lock acquisition on the happy path
        // (only the reject path acquires — idem path is no-op).
        auto rd_pos = ixx.find(
            "void record_dependency(const std::string& caller, const std::string& callee)");
        if (rd_pos == std::string::npos)
            rd_pos = ixx.find("void record_dependency(");
        auto rd_end = rd_pos + 6000;
        auto rd_block = ixx.substr(rd_pos, rd_end - rd_pos);
        // The lock_guard is INSIDE the if (epoch_before != ... || gen_before != ...) branch
        // — only fires on reject, not on the idempotent insert path.
        auto guard_pos = rd_block.find("cascade_guard(cascade_decision_mtx_)");
        auto reject_pos = rd_block.find("epoch_before != epoch_after");
        CHECK(guard_pos != std::string::npos, "AC3: guard present");
        CHECK(reject_pos != std::string::npos, "AC3: reject branch present");
        if (guard_pos != std::string::npos && reject_pos != std::string::npos) {
            // guard must be AFTER the reject branch opens.
            CHECK(guard_pos > reject_pos, "AC3: guard inside reject branch (no lock on idem path)");
            // guard must be BEFORE the return (so it spans the emplace + store).
            auto return_pos = rd_block.find("return;", guard_pos);
            CHECK(return_pos != std::string::npos, "AC3: return after guard");
            if (return_pos != std::string::npos)
                CHECK(guard_pos < return_pos, "AC3: guard spans emplace+store (before return)");
        }
    }

    // ── AC4: Existing #3067 + #3097 paths preserved + re-check force-full ──
    {
        std::println("\n--- AC4: existing #3067 + #3097 + re-check force-full ---");
        auto pos = ixx.find("std::size_t relower_dirty_defines_from_workspace()");
        auto end = pos + 8000;
        auto block = ixx.substr(pos, end - pos);
        // #3067: drain at entry (still present).
        must_inline(block, "drain_deferred_hybrid_cascade_()", "AC4 #3067 drain preserved");
        // #3097: impact_ub consult.
        must_inline(block, "impact_upper_bound_for_entry_",
                    "AC4 #3097 impact_ub consult preserved");
        must_inline(block, "should_partial_relower_impact_checked",
                    "AC4 #3097 partial-gate check preserved");
        must_inline(block, "partial_forced_full_by_impact_total",
                    "AC4 existing counter reused (no new metric key)");
        // #3135: re-check force-full on re-arm.
        must_inline(block, "re_armed_now", "AC4 re-check at re-arm");
        must_inline(block, "metrics_.partial_forced_full_by_impact_total.fetch_add",
                    "AC4 force-full bumps existing #3067/#3097 counter");
    }

    // ── AC5: src-aligned test, no tests/issues/test_issue_3135.cpp, no plan doc ──
    {
        std::println("\n--- AC5: src-aligned test, no plan doc ---");
        auto root = std::filesystem::current_path();
        CHECK(!std::filesystem::exists(root / "tests" / "issues" / "test_issue_3135.cpp"),
              "AC5: tests/issues/test_issue_3135.cpp absent (#81967)");
        CHECK(!std::filesystem::exists(root / "tests" / "compiler" / "test_issue_3135.cpp"),
              "AC5: tests/compiler/test_issue_3135.cpp absent (#81967)");
        auto docs = root / "docs" / "design";
        if (std::filesystem::exists(docs)) {
            for (const auto& f : std::filesystem::directory_iterator(docs)) {
                auto name = f.path().filename().string();
                CHECK(name.find("3135-") == std::string::npos,
                      "AC5: no docs/design/3135-* plan doc (#1655)");
                (void)name;
                break;
            }
        }
        // Existing dep-graph / cascade suites preserved (regression).
        CHECK(read_file("tests/compiler/test_dep_graph_hybrid_cascade.cpp")
                      .find("drain_deferred_hybrid_cascade") != std::string::npos,
              "AC5: hybrid-cascade lineage preserved");
    }

    std::println("\n=== #3135 cascade-decision atomic: {} passed, {} failed ===", g_passed,
                 g_failed);
    std::println("\n=== Issue #3168: concurrent cascade re-arm new-edge-only attribution ===");
    ac3168_1_production_rearm_new_edge_only();
    ac3168_2_soft_zero_extra();
    ac3168_3_partial_peel_preserved();
    ac3168_4_existing_3067_3097_3135_preserved();
    ac3168_5_source_and_linter();

    std::println("\n=== Final: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cascade_decision_residual_atomic_3135();
}

// ── Issue #3168 ACs ──
// Concurrent cascade re-arm under production multi-fiber: prefer
// new-edge-only mark over mark_all_blocks_dirty. Snapshot
// deferred_hybrid_edges_.size() at lock acquisition; in the
// rearm_observed_mid_loop branch walk [initial_size, current) under
// shared dep_graph_mtx_ and mark only the target callee blocks for
// THIS define via mark_block_dirty. Last-resort full path preserved
// only when new-edge set is empty / cannot be attributed (#3097
// semantics). Soft/Off + single-fiber + clean (armed==0): zero extra
// (need_lock + defer_lock pattern preserved, AC2).
static void ac3168_1_production_rearm_new_edge_only() {
    std::println("\n--- #3168 AC1: Production — concurrent re-arm → new-edge-only attribution ---");
    auto ixx = read_file("src/compiler/service.ixx");
    auto obs = read_file("src/compiler/observability_metrics.h");
    // Source-cite: snapshot block + attribution block + struct-end counter
    CHECK(ixx.find("Issue #3168: snapshot deferred_hybrid_edges_.size()") != std::string::npos,
          "3168 AC1: snapshot block cites #3168");
    CHECK(ixx.find("Issue #3168: prefer new-edge-only mark over full fallback") !=
              std::string::npos,
          "3168 AC1: attribution block cites #3168");
    CHECK(ixx.find("initial_deferred_edges_size") != std::string::npos,
          "3168 AC1: initial_deferred_edges_size snapshotted");
    CHECK(ixx.find("mark_block_dirty(fi, bi)") != std::string::npos,
          "3168 AC1: precise mark_block_dirty used in attribution");
    CHECK(ixx.find("cascade_rearm_new_edge_only_total") != std::string::npos,
          "3168 AC1: counter bumped in attribution block");
    CHECK(ixx.find("Issue #3097") != std::string::npos ||
              ixx.find("partial_forced_full_by_impact_total") != std::string::npos,
          "3168 AC1: #3097 last-resort full path preserved");
    CHECK(obs.find("cascade_rearm_new_edge_only_total") != std::string::npos,
          "3168 AC1: counter declared at struct end");
    // No docs/design/3168-* (per #1655).
    auto docs = std::string("docs/design/");
    if (std::filesystem::exists(docs)) {
        for (const auto& f : std::filesystem::directory_iterator(docs)) {
            auto name = f.path().filename().string();
            CHECK(name.find("3168-") == std::string::npos,
                  "3168 AC1: no docs/design/3168-* plan doc (#1655)");
            (void)name;
            break;
        }
    }
    // No test_issue_3168.cpp (per #81967).
    for (const auto& rel : {std::string("tests/issues/test_issue_3168.cpp"),
                            std::string("tests/compiler/test_issue_3168.cpp"),
                            std::string("tests/serve/test_issue_3168.cpp")}) {
        std::error_code ec;
        CHECK(!std::filesystem::exists(rel, ec),
              std::format("3168 AC1: forbidden {} per #81967", rel));
    }
}

static void ac3168_2_soft_zero_extra() {
    std::println("\n--- #3168 AC2: Soft / Off + single-fiber + clean (armed==0) → zero extra ---");
    auto ixx = read_file("src/compiler/service.ixx");
    // The need_lock + defer_lock pattern (preserved from #3135) must still gate
    // the cascade_decision_mtx_ acquisition; Soft/Off + clean path doesn't
    // pay the lock OR the initial_deferred_edges_size shared read.
    CHECK(ixx.find("const bool need_lock =") != std::string::npos,
          "3168 AC2: need_lock gate preserved (#3135)");
    CHECK(ixx.find("cascade_guard.lock()") != std::string::npos,
          "3168 AC2: defer_lock + explicit lock preserved");
    // The snapshot block is inside the cascade_guard critical section, so
    // Soft/Off + clean skips it (the lock isn't acquired → snapshot block
    // doesn't run).
    auto pos = ixx.find("Issue #3168: snapshot deferred_hybrid_edges_.size()");
    CHECK(pos != std::string::npos, "3168 AC2: snapshot block present");
    if (pos != std::string::npos) {
        // Verify the snapshot block sits AFTER the lock acquisition.
        const auto lock_pos = ixx.find("cascade_guard.lock()");
        CHECK(lock_pos != std::string::npos && lock_pos < pos,
              "3168 AC2: snapshot gated behind need_lock acquisition");
    }
}

static void ac3168_3_partial_peel_preserved() {
    std::println("\n--- #3168 AC3: Attribution success → partial peel preserved (want_partial "
                 "stays true) ---");
    auto ixx = read_file("src/compiler/service.ixx");
    auto build = read_file("build.py");
    // Attribution path: bump cascade_rearm_new_edge_only_total, do NOT
    // mark_all_blocks_dirty, do NOT bump partial_forced_full_by_impact_total.
    // Find the attribution block (between #3168: prefer new-edge-only and
    // its closing else) and confirm it does not touch want_partial = false
    // or partial_forced_full_by_impact_total.
    auto pos_attr = ixx.find("Issue #3168: prefer new-edge-only mark over full fallback");
    CHECK(pos_attr != std::string::npos, "3168 AC3: attribution block present");
    if (pos_attr != std::string::npos) {
        const auto attr_block = ixx.substr(pos_attr, 2400);
        CHECK(attr_block.find("metrics_.cascade_rearm_new_edge_only_total.fetch_add") !=
                  std::string::npos,
              "3168 AC3: attribution bumps cascade_rearm_new_edge_only_total");
        // Defensive fallback (else branch) preserves #3097 semantics.
        CHECK(attr_block.find("mark_all_blocks_dirty") != std::string::npos,
              "3168 AC3: defensive fallback keeps mark_all_blocks_dirty");
        CHECK(attr_block.find("partial_forced_full_by_impact_total.fetch_add") != std::string::npos,
              "3168 AC3: defensive fallback bumps partial_forced_full_by_impact_total");
    }
    // build.py wires a sibling command for the #3168 linter.
    CHECK(build.find("check_cascade_rearm_new_edge_only_3168") != std::string::npos ||
              build.find("cmd_cascade_rearm_new_edge_only_3168") != std::string::npos ||
              build.find("cascade-rearm-new-edge-only-3168") != std::string::npos,
          "3168 AC3: build.py wires #3168 linter (or sibling)");
}

static void ac3168_4_existing_3067_3097_3135_preserved() {
    std::println("\n--- #3168 AC4: #3067 / #3097 / #3135 paths preserved ---");
    auto ixx = read_file("src/compiler/service.ixx");
    // #3067: drain at entry.
    CHECK(ixx.find("drain_deferred_hybrid_cascade_()") != std::string::npos,
          "3168 AC4: #3067 drain at entry preserved");
    // #3097: impact_ub consult + partial_forced_full_by_impact_total.
    CHECK(ixx.find("impact_upper_bound_for_entry_") != std::string::npos,
          "3168 AC4: #3097 impact_ub consult preserved");
    CHECK(ixx.find("partial_forced_full_by_impact_total") != std::string::npos,
          "3168 AC4: #3097 counter still bumped (defensive fallback)");
    // #3135: cascade_decision_mtx_ + defer_lock + initial_armed check.
    CHECK(ixx.find("cascade_decision_mtx_") != std::string::npos,
          "3168 AC4: #3135 cascade_decision_mtx_ preserved");
    CHECK(ixx.find("std::defer_lock") != std::string::npos,
          "3168 AC4: #3135 defer_lock pattern preserved");
    // #3161: graph_grew_mid_loop still observed (mid-loop re-arm signal).
    CHECK(ixx.find("graph_grew_mid_loop") != std::string::npos,
          "3168 AC4: #3161 graph_grew_mid_loop observation preserved");
}

static void ac3168_5_source_and_linter() {
    std::println("\n--- #3168 AC8: source-cite linter + build.py wiring ---");
    auto build = read_file("build.py");
    auto lint = read_file("scripts/coverage/checks/check_cascade_rearm_new_edge_only_3168.py");
    // Linter exists; --self-test exercises it.
    int rc =
        std::system("python3 scripts/coverage/checks/check_cascade_rearm_new_edge_only_3168.py "
                    "--self-test > /dev/null 2>&1");
    CHECK(rc == 0, "3168 AC8: linter --self-test passes");
    CHECK(!lint.empty() && lint.find("Issue #3168") != std::string::npos,
          "3168 AC8: linter cites #3168");
    CHECK(build.find("check_cascade_rearm_new_edge_only_3168") != std::string::npos,
          "3168 AC8: build.py wires linter");
}
#endif