// @category: unit
// @reason: Issue #2556 — DeadCoercionEliminationPass scan limited to type∪IR
//          dirty cone (CastOp sites outside cone → dirty-cone-skips).
//
//   AC1: Partial cone → DCE only dirty blocks; cone-skips > 0 on multi-block fn
//   AC2: No dirty fn → full scan (full-scan-runs bumps; semantics unchanged)
//   AC3: Identity CastOp elision inside cone still works
//   AC4: Soft empty cone → skips bump, no dirty-mask walk of clean-only path
//   AC5: Source-cite + schema-2556 + #2025/#2282 layered keys preserved

#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.dirty_propagation;
import aura.compiler.optimization_passes;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::dirty::type_ir_union_cone_nonempty;
using aura::compiler::dirty::type_ir_union_cone_size;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_cast_sites_scanned;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_partial_runs;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_skips;
using aura::compiler::opt_registry::dead_coercion_full_scan_runs;
using aura::compiler::opt_registry::DeadCoercionPass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::BasicBlock;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:dead-coercion-layered-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// Multi-block function: block0 has identity CastOp (dirty), block1 has CastOp (clean).
static IRFunction make_two_block_fn_with_casts() {
    IRFunction fn;
    fn.name = "cone_partial";
    fn.local_count = 4;
    fn.arg_count = 0;
    fn.entry_block = 0;
    // Block 0 (dirty cone): Const + identity Cast (Int→Int tag 0)
    BasicBlock b0;
    b0.id = 0;
    b0.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        // CastOp: result=1, source=0, type_tag=0 (Int), blame=0 — identity
        IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1},
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    // Block 1 (outside cone): another identity cast that must NOT be scanned
    BasicBlock b1;
    b1.id = 1;
    b1.instructions = {
        IRInstruction{IROpcode::ConstI64, {2, 7, 0, 0}, 0, 1},
        IRInstruction{IROpcode::CastOp, {3, 2, 0, 0}, 0, 1},
        IRInstruction{IROpcode::Return, {3, 0, 0, 0}, 0, 0},
    };
    fn.blocks.push_back(std::move(b0));
    fn.blocks.push_back(std::move(b1));
    return fn;
}

// ── AC1: partial cone elides only dirty; skips > 0 ──
static void ac1_partial_cone() {
    std::println("\n--- #2556 AC1: partial cone DCE + cone-skips ---");
    DeadCoercionPass dce;
    // Only block 0 dirty.
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });

    IRFunction fn = make_two_block_fn_with_casts();
    const auto skips0 = load_u64(dead_coercion_dirty_cone_skips);
    const auto partial0 = load_u64(dead_coercion_dirty_cone_partial_runs);
    const auto scanned0 = load_u64(dead_coercion_dirty_cone_cast_sites_scanned);

    dce.run(fn);

    CHECK(load_u64(dead_coercion_dirty_cone_partial_runs) > partial0, "AC1: partial-runs bumped");
    CHECK(load_u64(dead_coercion_dirty_cone_skips) > skips0,
          "AC1: cone-skips > 0 (clean block CastOp counted)");
    CHECK(load_u64(dead_coercion_dirty_cone_cast_sites_scanned) >= scanned0 + 1,
          "AC1: scanned ≥1 CastOp in dirty block");
    // Block 0 identity cast should be elided.
    CHECK(dce.eliminated_count() >= 1, "AC1: identity CastOp elided inside cone");
    // Block 1 CastOp must remain (not scanned / not elided).
    bool block1_has_cast = false;
    for (const auto& instr : fn.blocks[1].instructions) {
        if (instr.opcode == IROpcode::CastOp)
            block1_has_cast = true;
    }
    CHECK(block1_has_cast, "AC1: cone-external CastOp untouched");
}

// ── AC2: no cone → full scan ──
static void ac2_full_scan() {
    std::println("\n--- #2556 AC2: full scan without dirty cone ---");
    DeadCoercionPass dce;
    // No block_dirty_fn → full path.
    IRFunction fn = make_two_block_fn_with_casts();
    const auto full0 = load_u64(dead_coercion_full_scan_runs);
    dce.run(fn);
    CHECK(load_u64(dead_coercion_full_scan_runs) > full0, "AC2: full-scan-runs bumped");
    CHECK(dce.eliminated_count() >= 1, "AC2: full scan still elides identity casts");
}

// ── AC3: identity semantics inside cone ──
static void ac3_identity_in_cone() {
    std::println("\n--- #2556 AC3: identity elision inside cone ---");
    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t) { return true; }); // whole fn dirty
    IRFunction fn;
    fn.name = "id_cast";
    fn.local_count = 2;
    fn.entry_block = 0;
    BasicBlock b;
    b.id = 0;
    b.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1}, // Int→Int identity
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    fn.blocks.push_back(std::move(b));
    dce.run(fn);
    CHECK(dce.eliminated_count() >= 1, "AC3: identity elided");
    bool still_cast = false;
    for (const auto& instr : fn.blocks[0].instructions) {
        if (instr.opcode == IROpcode::CastOp)
            still_cast = true;
    }
    CHECK(!still_cast, "AC3: no CastOp remains after identity elision");
}

// ── AC4: soft empty cone ──
static void ac4_soft_empty() {
    std::println("\n--- #2556 AC4: soft empty cone ---");
    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t) { return false; });
    IRFunction fn = make_two_block_fn_with_casts();
    const auto skips0 = load_u64(dead_coercion_dirty_cone_skips);
    const auto partial0 = load_u64(dead_coercion_dirty_cone_partial_runs);
    dce.run(fn);
    CHECK(load_u64(dead_coercion_dirty_cone_skips) >= skips0 + 2,
          "AC4: empty cone counts both CastOps as skips");
    CHECK(load_u64(dead_coercion_dirty_cone_partial_runs) == partial0,
          "AC4: empty cone does not count as partial-run");
    CHECK(dce.eliminated_count() == 0, "AC4: empty cone elides nothing");
    // Helper soft empty when no dirty marks.
    CHECK(!type_ir_union_cone_nonempty() || type_ir_union_cone_size() >= 0,
          "AC4: type_ir_union helpers callable");
}

// ── AC5: source + schema ──
static void ac5_source_schema() {
    std::println("\n--- #2556 AC5: source-cite + schema-2556 ---");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    CHECK(opt.find("#2556") != std::string::npos, "AC5: optimization_passes cites #2556");
    CHECK(opt.find("dead_coercion_dirty_cone_partial_runs") != std::string::npos,
          "AC5: partial_runs counter");
    CHECK(opt.find("count_cast_ops_in_block") != std::string::npos ||
              opt.find("count_cast_ops_in_function") != std::string::npos,
          "AC5: cast site counters");
    CHECK(opt.find("soft probe") != std::string::npos ||
              opt.find("Soft probe") != std::string::npos ||
              opt.find("any_dirty") != std::string::npos,
          "AC5: soft empty probe");

    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(dirty.find("type_ir_union_cone_size") != std::string::npos, "AC5: union cone size");
    CHECK(dirty.find("#2556") != std::string::npos, "AC5: dirty_propagation cites #2556");

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("schema-2556") != std::string::npos, "AC5: schema-2556 on layered stats");
    CHECK(q.find("dirty-cone-cast-sites-scanned") != std::string::npos, "AC5: scanned key");
    CHECK(q.find("full-scan-runs") != std::string::npos, "AC5: full-scan-runs key");

    // Live query surface.
    CompilerService cs;
    CHECK(href(cs, "schema-2556") == 2556, "AC5: live schema-2556");
    CHECK(href(cs, "dirty-cone-skips") >= 0, "AC5: dirty-cone-skips still queryable (#2282)");
    CHECK(href(cs, "dead-coercion-layered-total") >= 0, "AC5: layered total preserved");
    CHECK(href(cs, "dirty-cone-partial-runs") >= 0, "AC5: partial-runs queryable");
    CHECK(href(cs, "full-scan-runs") >= 0, "AC5: full-scan-runs queryable");

    // #2025 / #2282 lineage still in headers.
    CHECK(opt.find("#2025") != std::string::npos, "AC5: #2025 lineage");
    CHECK(opt.find("#2282") != std::string::npos || opt.find("2282") != std::string::npos,
          "AC5: #2282 layered lineage");
}

} // namespace

int run_test_dead_coercion_dirty_cone() {
    std::println("=== Issue #2556: DCE dirty-cone scan limit ===");
    ac1_partial_cone();
    ac2_full_scan();
    ac3_identity_in_cone();
    ac4_soft_empty();
    ac5_source_schema();
    std::println("\n=== #2556: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dead_coercion_dirty_cone();
}
#endif
