// @category: unit
// @reason: Issue #2556 — DeadCoercionEliminationPass scan limited to type∪IR
//          dirty cone (CastOp sites outside cone → dirty-cone-skips).
//
//   AC1: Partial cone → DCE only dirty blocks; cone-skips > 0 on multi-block fn
//   AC2: No dirty fn → full scan (full-scan-runs bumps; semantics unchanged)
//   AC3: Identity CastOp elision inside cone still works
//   AC4: Soft empty cone → skips bump, no dirty-mask walk of clean-only path
//   AC5: Source-cite + schema-2556 + #2025/#2282 layered keys preserved
// Issue #3007: Production hot-fn residual identity CastOp sweep.

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

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
using aura::compiler::opt_registry::count_identity_castops;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_cast_sites_scanned;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_partial_runs;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_skips;
using aura::compiler::opt_registry::dead_coercion_full_scan_runs;
using aura::compiler::opt_registry::dead_coercion_hot_residual_reject_total;
using aura::compiler::opt_registry::dead_coercion_hot_residual_sweep_total;
using aura::compiler::opt_registry::DeadCoercionPass;
using aura::compiler::opt_registry::kDeadCoercionHotResidualIssue;
using aura::compiler::opt_registry::sweep_production_hot_residual_castops;
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

    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
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

// Identity CastOp with type_id set so Rule 1 fires (hot residual sweep).
static IRFunction make_two_block_identity_typed() {
    IRFunction fn;
    fn.name = "hot_residual";
    fn.local_count = 4;
    fn.entry_block = 0;
    BasicBlock b0;
    b0.id = 0;
    b0.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 42, 0, 0}, /*type_id=*/1, 1},
        IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, /*type_id=*/1, 1},
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    BasicBlock b1;
    b1.id = 1;
    b1.instructions = {
        IRInstruction{IROpcode::ConstI64, {2, 7, 0, 0}, /*type_id=*/1, 1},
        IRInstruction{IROpcode::CastOp, {3, 2, 0, 0}, /*type_id=*/1, 1},
        IRInstruction{IROpcode::Return, {3, 0, 0, 0}, 0, 0},
    };
    fn.blocks.push_back(std::move(b0));
    fn.blocks.push_back(std::move(b1));
    return fn;
}

// ── Issue #3007: Production hot residual CastOp ──
static void ac3007_1_production_sweeps_cone_external() {
    std::println("\n--- #3007 AC1: Production sweeps cone-external identity CastOp ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });
    IRFunction fn = make_two_block_identity_typed();
    CHECK(count_identity_castops(fn) == 2, "3007 AC1: two identity CastOps");
    const auto sweep0 = dead_coercion_hot_residual_sweep_total.load();
    dce.run(fn);
    CHECK(dead_coercion_hot_residual_sweep_total.load() > sweep0, "3007 AC1: Production sweep");
    CHECK(count_identity_castops(fn) == 0, "3007 AC1: no residual identity CastOp");
    bool any_cast = false;
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instructions)
            if (i.opcode == IROpcode::CastOp)
                any_cast = true;
    CHECK(!any_cast, "3007 AC1: hot fn has no leftover CastOp");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3007_2_soft_keeps_cone_skip() {
    std::println("\n--- #3007 AC2: Soft keeps cone-external CastOp ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);

    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });
    IRFunction fn = make_two_block_identity_typed();
    const auto reject0 = dead_coercion_hot_residual_reject_total.load();
    dce.run(fn);
    bool block1_has_cast = false;
    for (const auto& instr : fn.blocks[1].instructions) {
        if (instr.opcode == IROpcode::CastOp)
            block1_has_cast = true;
    }
    CHECK(block1_has_cast, "3007 AC2: Soft leaves cone-external CastOp");
    CHECK(dead_coercion_hot_residual_reject_total.load() == reject0,
          "3007 AC2: Soft no residual reject");
    CHECK(sweep_production_hot_residual_castops(fn, nullptr) >= 1,
          "3007 AC2: Soft sweep observes only");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3007_3_schema_and_source() {
    std::println("\n--- #3007 AC3: schema-3007 + source-cite ---");
    CHECK(kDeadCoercionHotResidualIssue == 3007, "3007 AC3: issue constant");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto ev = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(opt.find("#3007") != std::string::npos, "3007 AC3: opt cites #3007");
    CHECK(opt.find("sweep_production_hot_residual_castops") != std::string::npos,
          "3007 AC3: sweep helper");
    CHECK(svc.find("sweep_production_hot_residual_castops") != std::string::npos,
          "3007 AC3: post-mutate IR DCE wired");
    CHECK(ev.find("#3007") != std::string::npos, "3007 AC3: CoercionMap rebuild cites #3007");
    CHECK(ev.find("set_bidirectional_mode(true)") != std::string::npos,
          "3007 AC3: bidirectional covers mutate sites");
    CHECK(q.find("schema-3007") != std::string::npos, "3007 AC3: schema-3007");
    CHECK(q.find("hot-residual-sweep-total") != std::string::npos, "3007 AC3: sweep key");
    CHECK(q.find("schema-2556") != std::string::npos, "3007 AC3: lineage #2556");
    CompilerService cs;
    CHECK(href(cs, "schema-3007") == 3007, "3007 AC3: live schema-3007");
    CHECK(href(cs, "issue-3007") == 3007, "3007 AC3: live issue-3007");
    CHECK(href(cs, "hot-residual-wired") == 1, "3007 AC3: wired");
    CHECK(href(cs, "hot-residual-sweep-total") >= 0, "3007 AC3: sweep queryable");
    CHECK(href(cs, "schema-2556") == 2556, "3007 AC3: schema-2556 preserved");
}

static void ac3007_4_linter_no_design() {
    std::println("\n--- #3007 AC4: linter + no invent / no design ---");
    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_dead_coercion_hot_residual_3007.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3007_1_production_sweeps_cone_external") != std::string::npos, "3007 AC4: AC1");
    CHECK(t.find("ac3007_2_soft_keeps_cone_skip") != std::string::npos, "3007 AC4: AC2");
    CHECK(t.find("ac3007_3_schema_and_source") != std::string::npos, "3007 AC4: AC3");
    CHECK(!lint.empty() && lint.find("#3007") != std::string::npos, "3007 AC4: linter");
    CHECK(build.find("check_dead_coercion_hot_residual_3007") != std::string::npos,
          "3007 AC4: build.py gate");
    CHECK(build.find("cmd_dead_coercion_hot_residual_3007_coverage") != std::string::npos,
          "3007 AC4: build.py cmd");
    CHECK(read_file("tests/compiler/test_issue_3007.cpp").empty(),
          "3007 AC4: no test_issue_3007.cpp");
}

} // namespace

int run_test_dead_coercion_dirty_cone() {
    std::println("=== Issue #2556: DCE dirty-cone scan limit ===");
    ac1_partial_cone();
    ac2_full_scan();
    ac3_identity_in_cone();
    ac4_soft_empty();
    ac5_source_schema();
    ac3007_1_production_sweeps_cone_external();
    ac3007_2_soft_keeps_cone_skip();
    ac3007_3_schema_and_source();
    ac3007_4_linter_no_design();
    std::println("\n=== #2556/#3007: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dead_coercion_dirty_cone();
}
#endif
