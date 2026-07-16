// @category: integration
// @reason: Issue #1474 — Wire per-block dirty bitmask into actual
// re-lower path (scope-limited close)
//
// Scope-limited close matching #1459 / #1470 / #1473 pattern.
//
// Discovery before this PR (no duplication): the per-block dirty
// bitmask (Issue #196) + relower_define_blocks() / relower_define_function()
// helpers (Issue #224 cycle 3) already existed. The actual gap was
// that relower_define_function() did a WHOLE-function replace
// (entry.irs[func_idx] = std::move(new_func)) — the per-block
// bitmask was bumped but never actually used to select which blocks
// to copy. #1474 wires the per-block selective copy path.
//
// What ships in this PR:
//   - relower_define_function: per-block selective copy path
//     (Issue #1474 cycle 1). When dirty_mask size matches
//     new_func.blocks.size() AND entry.irs[func_idx].blocks.size(),
//     only dirty blocks are copied from new_func into entry.irs.
//     Clean blocks keep their old IR. Shape mismatch (block
//     count changed) falls back to whole-function replace
//     (preserves previous behavior).
//   - 1 new counter: incremental_relower_blocks_total — # of
//     blocks actually replaced across all relower_define_function
//     calls. With per-block selective copy, this grows by
//     dirty_block_count() per call (not total_blocks()).
//   - 1 new derived field: dirty_block_ratio_bp —
//     ir_soa_block_dirty_hits_total / (ir_soa_block_dirty_hits_total
//     + ir_soa_relower_blocks_saved_total) * 10000 (basis points).
//   - 2 new snapshot fields in CompilerSnapshot.
//
// What does NOT ship (deferred to follow-up issues):
//   - AC2: mark_define_dirty cascade for nested lambda
//     (irs.size() > 2) — needs dep_graph_-aware cascade refinement
//   - AC3: lookup_define_v2 / eval-current should prefer partial
//     re-lower — needs wiring of relower_define_blocks into the
//     eval pipeline (currently only called by relower_define_function
//     dispatch in cache_define path)
//
// This test exercises the per-block selective copy path in a 1000
// round stress loop. Each round marks only the body block dirty
// (block 0 of irs[1]) and re-lowers via relower_define_function.
// With the per-block path wired, incremental_relower_blocks_total
// should grow by 1 per round (not by the function's total block
// count). Synthetic 2-function / 1-block-each IR keeps the test
// fast and deterministic; lower_function_at does the real lowering
// of the Lambda AST node from a freshly-parsed flat/pool.

#include <atomic>
#include <cstdint>
#include <format>
#include <iostream>
#include <print>
#include <string>

#include "test_harness.hpp"

import aura.core.arena;
import aura.core.ast;
import aura.compiler.ir;
import aura.compiler.service;
import aura.parser.parser;

namespace aura_issue_1474_detail {

// test_harness.hpp defines `CHECK` already (line ~127). We undefine
// and redefine to print to cout/cerr with our formatting (same
// pattern as other issue_14NN tests).
#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++::aura::test::g_passed;                                                              \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++::aura::test::g_failed;                                                              \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

} // namespace aura_issue_1474_detail

int aura_issue_1474_run() {
    using namespace aura_issue_1474_detail;

    aura::compiler::CompilerService cs;

    // ── AC1 setup: store_define_v2 with synthetic 2-function / 1-block-each IR ──
    // irs[0] = __top__ entry function, irs[1] = body Lambda function.
    // The bitmask is initialized by store_define_v2 → init_block_dirty_from_irs
    // (sizes to irs[i].blocks.size(), marks all dirty) → clear_all_block_dirty
    // (flips to clean). Net effect: bitmask mirrors irs shape, all clean.
    aura::ir::IRFunction entry_fn;
    entry_fn.id = 0;
    entry_fn.name = "__top__";
    entry_fn.entry_block = 0;
    entry_fn.blocks.push_back({0, {}, {}});
    aura::ir::IRFunction body_fn;
    body_fn.id = 1;
    body_fn.name = "f_inner";
    body_fn.entry_block = 0;
    body_fn.blocks.push_back({0, {}, {}});
    std::vector<aura::ir::IRFunction> irs;
    irs.push_back(entry_fn);
    irs.push_back(body_fn);
    cs.store_define_v2("f", "(define (f x) (+ x 1))", std::move(irs),
                       std::vector<aura::ir::ClosureBridgeData>{}, std::vector<std::string>{});

    const auto* entry = cs.get_define_v2("f");
    CHECK(entry != nullptr, "ir_cache_v2_[\"f\"] exists after store_define_v2");
    if (entry == nullptr) {
        return ::aura::test::g_failed == 0 ? 0 : 1;
    }
    CHECK(entry->irs.size() == 2, "entry has 2 functions (entry + body)");
    CHECK(entry->block_dirty_per_func_.size() == 2, "bitmask sized to 2 functions");
    CHECK(entry->block_dirty_per_func_[1].size() == 1, "body function bitmask has 1 block");
    CHECK(entry->block_dirty_per_func_[1][0] == 0, "body block starts clean");

    // ── AC2 setup: parse the source into a fresh flat + pool, find the Lambda node id ──
    // lower_function_at() needs a real Lambda AST node — the synthetic IRs in v2
    // don't carry AST context, so we re-parse the source to drive the lower.
    aura::ast::ASTArena arena(4096);
    auto a = arena.allocator();
    aura::ast::FlatAST flat(a);
    aura::ast::StringPool pool(a);
    auto pr = aura::parser::parse_to_flat("(define (f x) (+ x 1))", flat, pool);
    CHECK(pr.success, "parse_to_flat succeeded");
    if (pr.success)
        flat.root = pr.root;

    // Find the Define's Lambda child (the body of `f`).
    aura::ast::NodeId lambda_id = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < flat.size(); ++id) {
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Define && v.sym_id != aura::ast::INVALID_SYM &&
            std::string(pool.resolve(v.sym_id)) == "f") {
            lambda_id = v.child(0);
            break;
        }
    }
    CHECK(lambda_id != aura::ast::NULL_NODE, "found lambda_id for f");

    // ── AC3: verify initial counter state ──
    const auto initial_blocks =
        cs.metrics().incremental_relower_blocks_total.load(std::memory_order_relaxed);
    const auto initial_per_fn =
        cs.metrics().relower_per_function_called_count.load(std::memory_order_relaxed);
    CHECK(initial_blocks == 0, "initial incremental_relower_blocks_total == 0");
    CHECK(initial_per_fn == 0, "initial relower_per_function_called_count == 0");

    // ── AC4: 1000 rounds of mark dirty + per-function re-lower ──
    // Each round marks only the body block (block 0 of irs[1]) dirty.
    // With per-block selective copy, incremental_relower_blocks_total
    // should grow by 1 per round (not by the function's total block count).
    constexpr int kRounds = 1000;
    int fail_count = 0;
    for (int i = 0; i < kRounds; ++i) {
        cs.mark_block_dirty_v2("f", /*func_idx=*/1, /*block_idx=*/0);
        const bool ok = cs.relower_define_function("f", /*func_idx=*/1, flat, pool, lambda_id);
        if (!ok) {
            ++fail_count;
        }
    }
    CHECK(fail_count == 0,
          std::format("all 1000 rounds of relower_define_function succeeded ({} failed)",
                      fail_count));

    // ── AC5: incremental_relower_blocks_total grew by 1000 (1 block per round) ──
    // This is the per-block win: the previous whole-function replace
    // would have grown this by total_blocks() (e.g. 1 for our 1-block
    // body, but the per-round counter for a multi-block function would
    // have grown by N). For 1000 rounds of 1 dirty block, this MUST
    // be exactly 1000.
    const auto after_blocks =
        cs.metrics().incremental_relower_blocks_total.load(std::memory_order_relaxed);
    CHECK(after_blocks == static_cast<std::uint64_t>(kRounds),
          std::format("incremental_relower_blocks_total == {} (got {})", kRounds, after_blocks));

    // ── AC6: relower_per_function_called_count grew by 1000 ──
    const auto after_per_fn =
        cs.metrics().relower_per_function_called_count.load(std::memory_order_relaxed);
    CHECK(after_per_fn == static_cast<std::uint64_t>(kRounds),
          std::format("relower_per_function_called_count == {} (got {})", kRounds, after_per_fn));

    // ── AC7: ir_soa_block_dirty_hits_total grew (1000 mark dirty calls) ──
    // mark_block_dirty_v2 bumps this by 1 per call.
    const auto after_hits =
        cs.metrics().ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed);
    CHECK(after_hits >= static_cast<std::uint64_t>(kRounds),
          std::format("ir_soa_block_dirty_hits_total >= {} (got {})", kRounds, after_hits));

    // ── AC8: snapshot fields are populated correctly ──
    auto snap = cs.snapshot();
    CHECK(snap.incremental_relower_blocks_total == static_cast<std::uint64_t>(kRounds),
          std::format("snapshot.incremental_relower_blocks_total == {} (got {})", kRounds,
                      snap.incremental_relower_blocks_total));

    // ── AC9: dirty_block_ratio_bp is computed from the existing fields ──
    const auto hits = cs.metrics().ir_soa_block_dirty_hits_total.load(std::memory_order_relaxed);
    const auto saved =
        cs.metrics().ir_soa_relower_blocks_saved_total.load(std::memory_order_relaxed);
    const std::uint64_t sum = hits + saved;
    const std::uint64_t expected_bp = sum > 0 ? (hits * 10000u) / sum : 0;
    CHECK(snap.dirty_block_ratio_bp == expected_bp,
          std::format("snapshot.dirty_block_ratio_bp matches computed ({} == {}, hits={} saved={})",
                      snap.dirty_block_ratio_bp, expected_bp, hits, saved));

    // ── AC10: bitmask is clean after 1000 per-block re-lowers ──
    const auto* entry2 = cs.get_define_v2("f");
    CHECK(entry2 != nullptr, "ir_cache_v2_[\"f\"] still exists after stress");
    if (entry2 != nullptr) {
        CHECK(entry2->dirty == false, "entry.dirty flag is clear (all blocks re-lowered)");
        CHECK(entry2->dirty_block_count() == 0, "dirty_block_count() == 0 (all bits clear)");
    }

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int main() {
    return aura_issue_1474_run();
}
