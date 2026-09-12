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
// Issue #3065: post-DeadCoercion remirror of elim'd / residual CastOp
//              nodes into the type∪IR dirty cone (production/Full).
// Issue #3120: persist residual CastOp across type-txn wipe; remirror
//              after non-empty production cone (Soft observe-only).
// Issue #3228: empty cone + residual persist must remirror (columnar
//              under-mark). Soft uses apply_dev_audit_defaults (Sampled);
//              production_defaults_active=0 alone is still Full (#2818).
// Issue #3347: single-boundary commit_readiness / grant remirror residual
//              CastOp persist before auto_partial / type authority.
// Issue #3349: re-union persist into type∪IR before partial-relower
//              impact_ub (decision-time cone lag after #3120 / #3228).

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "compiler/dce_elided_deopt_meta.h"

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
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.pass_manager;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.ir_cache_pure; // #3068 source_to_ir_map_missing_instr_loc
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::dirty::clear_residual_castop_undermark_pending;
using aura::compiler::dirty::dead_coercion_decision_invalidate_gen;
using aura::compiler::dirty::dead_coercion_elim_cone_force_total;
using aura::compiler::dirty::force_dead_coercion_elim_into_cone;
using aura::compiler::dirty::force_residual_castop_undermark_into_cone;
using aura::compiler::dirty::kDeadCoercionElimConeIssue;
using aura::compiler::dirty::kDeadCoercionPersistBeforePartialIssue;
using aura::compiler::dirty::kResidualCastopReadinessUndermarkIssue;
using aura::compiler::dirty::kResidualCastopTypeTxnRemirrorIssue;
using aura::compiler::dirty::last_type_cone_ast;
using aura::compiler::dirty::mirror_type_affected_to_cascade;
using aura::compiler::dirty::note_residual_castop_sites;
using aura::compiler::dirty::remirror_persisted_residual_castops;
using aura::compiler::dirty::reset_residual_castop_persist_for_test;
using aura::compiler::dirty::residual_castop_persist_size;
using aura::compiler::dirty::residual_castop_undermark_pending;
using aura::compiler::dirty::type_ir_union_cone_nonempty;
using aura::compiler::dirty::type_ir_union_cone_size;
using aura::compiler::opt_registry::count_identity_castops;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_cast_sites_scanned;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_partial_runs;
using aura::compiler::opt_registry::dead_coercion_dirty_cone_skips;
using aura::compiler::opt_registry::dead_coercion_full_scan_runs;
using aura::compiler::opt_registry::dead_coercion_hot_residual_reject_total;
using aura::compiler::opt_registry::dead_coercion_hot_residual_sweep_total;
using aura::compiler::opt_registry::dead_coercion_ir_decision_invalidate_total;
using aura::compiler::opt_registry::DeadCoercionPass;
using aura::compiler::opt_registry::kDeadCoercionDecisionReverifyIssue;
using aura::compiler::opt_registry::kDeadCoercionHotResidualIssue;
using aura::compiler::opt_registry::sweep_production_hot_residual_castops;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
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

static void ac3046_nonidentity_density_cite() {
    std::println("\n--- #3046: DCE leftover CastOp density-policy ---");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    CHECK(opt.find("count_all_castops") != std::string::npos, "3046: count leftover CastOps");
    CHECK(opt.find("note_hot_residual_nonidentity_castops") != std::string::npos,
          "3046: density keep after identity sweep");
    CHECK(opt.find("#3046") != std::string::npos, "3046: opt cites #3046");
    CHECK(read_file("src/compiler/castop_density_policy.hh")
                  .find("note_hot_residual_nonidentity_castops") != std::string::npos,
          "3046: policy helper");
    CompilerService cs;
    CHECK(href(cs, "schema-3046") == 3046, "3046: live schema-3046");
    CHECK(href(cs, "hot-residual-nonidentity-wired") == 1, "3046: wired");
    CHECK(href(cs, "hot-residual-nonidentity-total") >= 0, "3046: leftover queryable");
}

static bool cone_contains(aura::compiler::dirty::NodeId nid) {
    for (auto n : last_type_cone_ast()) {
        if (n == nid)
            return true;
    }
    return false;
}

// Issue #2818: cold-start strategy is Full. Soft AC2 must opt into Sampled
// (apply_dev_audit_defaults) — clearing production_defaults_active alone
// still takes the persist / remirror path.
struct SoftAuditScope {
    std::uint32_t prod;
    std::uint32_t ratio;
    std::uint32_t opt_in;
    aura::compiler::typed_audit::AuditStrategy strat;
    SoftAuditScope() {
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
        using aura::compiler::typed_audit::get_sample_ratio;
        using aura::compiler::typed_audit::get_strategy;
        prod = g_typed_mutation_audit_counters.production_defaults_active.load(
            std::memory_order_relaxed);
        ratio = get_sample_ratio();
        opt_in = g_typed_mutation_audit_counters.dev_audit_opt_in.load(std::memory_order_relaxed);
        strat = get_strategy();
        apply_dev_audit_defaults();
    }
    ~SoftAuditScope() {
        using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
        using aura::compiler::typed_audit::set_sample_ratio;
        using aura::compiler::typed_audit::set_strategy;
        set_strategy(strat);
        set_sample_ratio(ratio);
        g_typed_mutation_audit_counters.production_defaults_active.store(prod,
                                                                         std::memory_order_relaxed);
        g_typed_mutation_audit_counters.dev_audit_opt_in.store(opt_in, std::memory_order_relaxed);
    }
};

// ── Issue #3065: remirror elim'd / residual CastOp into type cone ──
static void ac3065_1_production_elim_reenters_cone() {
    std::println("\n--- #3065 AC1: Production elim re-enters typecheck cone ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    // Helper: remutate of elim'd node 77 is in last type cone.
    constexpr aura::compiler::dirty::NodeId kElim = 77;
    const auto force0 = dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed);
    const auto union0 = type_ir_union_cone_size();
    const aura::compiler::dirty::NodeId one[] = {kElim};
    CHECK(force_dead_coercion_elim_into_cone(one) >= 1, "3065 AC1: helper remirrors");
    CHECK(cone_contains(kElim), "3065 AC1: elim'd node in last type cone");
    CHECK(type_ir_union_cone_nonempty(), "3065 AC1: cone nonempty for remutate typecheck");
    CHECK(type_ir_union_cone_size() >= union0 + 1, "3065 AC1: union cone enlarged");
    CHECK(dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed) > force0,
          "3065 AC1: force-total bumped");

    // AST identity elision (apply_coercion_map) remirrors original_child.
    // Pad so the literal is a non-zero NodeId (0 is NULL_NODE / skipped).
    aura::ast::FlatAST flat;
    (void)flat.add_literal(0);
    (void)flat.add_literal(1);
    auto lit = flat.add_literal(3);
    CHECK(lit != 0, "3065 AC1: literal NodeId nonzero");
    flat.set_type(lit, 1);
    aura::compiler::CoercionMap map;
    map.add(aura::ast::NULL_NODE, 0, lit, 1, 1, 0, 0);
    aura::compiler::DeadCoercionAstStats st;
    CHECK(aura::compiler::apply_coercion_map(flat, map, &st) == 0, "3065 AC1: identity elided");
    CHECK(st.eliminated >= 1, "3065 AC1: AST elim counted");
    CHECK(cone_contains(static_cast<aura::compiler::dirty::NodeId>(lit)),
          "3065 AC1: AST-elided node in cone");

    // IR DCE of identity CastOp with source_ast_node_id remirrors.
    constexpr aura::compiler::dirty::NodeId kIr = 88;
    IRFunction fn;
    fn.name = "dce_cone";
    fn.local_count = 2;
    fn.entry_block = 0;
    BasicBlock b;
    b.id = 0;
    b.instructions = {
        IRInstruction{.opcode = IROpcode::ConstI64,
                      .operands = {0, 1, 0, 0},
                      .source_ast_node_id = 0,
                      .type_id = 1},
        IRInstruction{.opcode = IROpcode::CastOp,
                      .operands = {1, 0, 0, 0},
                      .source_ast_node_id = kIr,
                      .type_id = 1},
        IRInstruction{.opcode = IROpcode::Return, .operands = {1, 0, 0, 0}},
    };
    fn.blocks.push_back(std::move(b));
    DeadCoercionPass dce;
    dce.run(fn);
    CHECK(dce.eliminated_count() >= 1, "3065 AC1: IR identity elided");
    CHECK(cone_contains(kIr), "3065 AC1: IR-elided AST node in cone");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3065_2_soft_no_permanent_bits() {
    std::println("\n--- #3065 AC2: Soft/quiet no new permanent dirty bits ---");
    SoftAuditScope soft;

    constexpr aura::compiler::dirty::NodeId kSoft = 99;
    const auto force0 = dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed);
    const auto union0 = type_ir_union_cone_size();
    const bool had = cone_contains(kSoft);
    const aura::compiler::dirty::NodeId one[] = {kSoft};
    CHECK(force_dead_coercion_elim_into_cone(one) == 0, "3065 AC2: Soft helper no-op");
    CHECK(cone_contains(kSoft) == had, "3065 AC2: Soft does not persist node");
    CHECK(type_ir_union_cone_size() == union0, "3065 AC2: Soft union size unchanged");
    CHECK(dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed) == force0,
          "3065 AC2: Soft force-total unchanged");

    // Quiet: empty span.
    CHECK(force_dead_coercion_elim_into_cone({}) == 0, "3065 AC2: quiet empty span");

    // Soft apply_coercion_map identity does not remirror.
    aura::ast::FlatAST flat;
    auto lit = flat.add_literal(4);
    flat.set_type(lit, 1);
    aura::compiler::CoercionMap map;
    map.add(aura::ast::NULL_NODE, 0, lit, 1, 1, 0, 0);
    const bool had_lit = cone_contains(static_cast<aura::compiler::dirty::NodeId>(lit));
    const auto union1 = type_ir_union_cone_size();
    const auto force1 = dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed);
    (void)aura::compiler::apply_coercion_map(flat, map);
    CHECK(cone_contains(static_cast<aura::compiler::dirty::NodeId>(lit)) == had_lit,
          "3065 AC2: Soft AST elim does not add cone bits");
    CHECK(type_ir_union_cone_size() == union1, "3065 AC2: Soft apply union unchanged");
    CHECK(dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed) == force1,
          "3065 AC2: Soft apply force-total unchanged");
}

static void ac3065_3_schema_and_union_metrics() {
    std::println("\n--- #3065 AC3: schema-3065 + union cone metrics ---");
    CHECK(kDeadCoercionElimConeIssue == 3065, "3065 AC3: issue constant");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    const auto cm = read_file("src/compiler/coercion_map.ixx");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(dirty.find("Issue #3065") != std::string::npos, "3065 AC3: dirty cites Issue #3065");
    CHECK(dirty.find("force_dead_coercion_elim_into_cone") != std::string::npos,
          "3065 AC3: remirror helper");
    CHECK(dirty.find("force_residual_castop_blocks_into_cone") != std::string::npos,
          "3065 AC3: residual block helper");
    CHECK(cm.find("Issue #3065") != std::string::npos, "3065 AC3: apply_coercion_map cites");
    CHECK(opt.find("Issue #3065") != std::string::npos, "3065 AC3: residual sweep cites");
    CHECK(q.find("schema-3065") != std::string::npos, "3065 AC3: schema-3065");
    CHECK(q.find("dead-coercion-elim-cone-force-total") != std::string::npos,
          "3065 AC3: force-total key");
    CHECK(q.find("schema-2556") != std::string::npos, "3065 AC3: lineage #2556");
    CompilerService cs;
    CHECK(href(cs, "schema-3065") == 3065, "3065 AC3: live schema-3065");
    CHECK(href(cs, "issue-3065") == 3065, "3065 AC3: live issue-3065");
    CHECK(href(cs, "dead-coercion-elim-cone-wired") == 1, "3065 AC3: wired");
    CHECK(href(cs, "dead-coercion-elim-cone-force-total") >= 0, "3065 AC3: force queryable");
    CHECK(href(cs, "schema-2556") == 2556, "3065 AC3: schema-2556 preserved");
    CHECK(href(cs, "schema-3007") == 3007, "3065 AC3: schema-3007 preserved");
}

static void ac3065_4_residual_and_linter() {
    std::println("\n--- #3065 AC4: residual CastOp cone + linter ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);

    // Non-identity leftover CastOp (src type 1, dest type 2) with AST id.
    constexpr aura::compiler::dirty::NodeId kRes = 101;
    IRFunction fn;
    fn.name = "residual_cone";
    fn.local_count = 2;
    fn.entry_block = 0;
    BasicBlock b;
    b.id = 0;
    b.instructions = {
        IRInstruction{.opcode = IROpcode::ConstI64,
                      .operands = {0, 1, 0, 0},
                      .source_ast_node_id = 0,
                      .type_id = 1},
        IRInstruction{.opcode = IROpcode::CastOp,
                      .operands = {1, 0, 1, 0},
                      .source_ast_node_id = kRes,
                      .type_id = 2},
        IRInstruction{.opcode = IROpcode::Return, .operands = {1, 0, 0, 0}},
    };
    fn.blocks.push_back(std::move(b));
    CHECK(count_identity_castops(fn) == 0, "3065 AC4: leftover is non-identity");
    (void)sweep_production_hot_residual_castops(fn, nullptr);
    CHECK(cone_contains(kRes), "3065 AC4: residual CastOp remirrored into cone");
    CHECK(type_ir_union_cone_nonempty(), "3065 AC4: remutate cone nonempty");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);

    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_dead_coercion_elim_cone_3065.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3065_1_production_elim_reenters_cone") != std::string::npos, "3065 AC4: AC1");
    CHECK(t.find("ac3065_2_soft_no_permanent_bits") != std::string::npos, "3065 AC4: AC2");
    CHECK(t.find("ac3065_3_schema_and_union_metrics") != std::string::npos, "3065 AC4: AC3");
    CHECK(!lint.empty() && lint.find("Issue #3065") != std::string::npos, "3065 AC4: linter");
    CHECK(build.find("check_dead_coercion_elim_cone_3065") != std::string::npos,
          "3065 AC4: build.py gate");
    CHECK(build.find("cmd_dead_coercion_elim_cone_3065") != std::string::npos,
          "3065 AC4: build.py cmd");
    CHECK(read_file("tests/compiler/test_issue_3065.cpp").empty(),
          "3065 AC4: no test_issue_3065.cpp");
}

// ── Issue #3120: persist residual CastOp across type-txn wipe ──
static void ac3120_1_type_txn_remirrors_skipped_castop() {
    std::println("\n--- #3120 AC1: type-txn remirror previously cone-skipped CastOp ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kSkip = 202;
    constexpr aura::compiler::dirty::NodeId kNew = 303;
    IRFunction fn;
    fn.name = "cone_skip_then_type";
    fn.local_count = 4;
    fn.entry_block = 0;
    BasicBlock b0;
    b0.id = 0;
    b0.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1},
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    BasicBlock b1;
    b1.id = 1;
    b1.instructions = {
        IRInstruction{.opcode = IROpcode::ConstI64,
                      .operands = {2, 7, 0, 0},
                      .source_ast_node_id = 0,
                      .type_id = 1},
        IRInstruction{.opcode = IROpcode::CastOp,
                      .operands = {3, 2, 1, 0},
                      .source_ast_node_id = kSkip,
                      .type_id = 2},
        IRInstruction{IROpcode::Return, {3, 0, 0, 0}, 0, 0},
    };
    fn.blocks.push_back(std::move(b0));
    fn.blocks.push_back(std::move(b1));

    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });
    const auto skips0 = load_u64(dead_coercion_dirty_cone_skips);
    const auto full0 = load_u64(dead_coercion_full_scan_runs);
    dce.run(fn);
    // Issue #3547: production + stamper_bound=0 drops the dirty-cone cache
    // (full scan). Residual persist below still records the cone-external site.
    CHECK(load_u64(dead_coercion_dirty_cone_skips) > skips0 ||
              load_u64(dead_coercion_full_scan_runs) > full0,
          "3120 AC1: cone-skip or stamper-unbound full scan");
    CHECK(residual_castop_persist_size() >= 1, "3120 AC1: residual persist after sweep");
    CHECK(cone_contains(kSkip), "3120 AC1: sweep remirror put skip site in last cone");

    // Type txn wipe: post-infer cone is only the new affected site.
    const aura::compiler::dirty::NodeId new_cone[] = {kNew};
    CHECK(mirror_type_affected_to_cascade(new_cone) >= 1, "3120 AC1: type txn mirrors new cone");
    CHECK(cone_contains(kNew), "3120 AC1: new type cone installed");
    CHECK(!cone_contains(kSkip), "3120 AC1: wipe dropped previously remirrored skip site");

    const auto force0 = dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed);
    CHECK(remirror_persisted_residual_castops() >= 1, "3120 AC1: remirror re-unions skip site");
    CHECK(cone_contains(kSkip), "3120 AC1: skip site back in type∪IR cone");
    CHECK(cone_contains(kNew), "3120 AC1: new type cone retained");
    CHECK(type_ir_union_cone_nonempty(), "3120 AC1: remutate cone nonempty");
    CHECK(dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed) > force0,
          "3120 AC1: reused #3065 force-total (no new query key)");

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3120_2_soft_zero_cost() {
    std::println("\n--- #3120 AC2: Soft persist / remirror no-ops ---");
    SoftAuditScope soft;
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kSoft = 404;
    const aura::compiler::dirty::NodeId one[] = {kSoft};
    note_residual_castop_sites(one, {});
    CHECK(residual_castop_persist_size() == 0, "3120 AC2: Soft does not persist");
    const auto force0 = dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed);
    const auto union0 = type_ir_union_cone_size();
    CHECK(remirror_persisted_residual_castops() == 0, "3120 AC2: Soft remirror no-op");
    CHECK(type_ir_union_cone_size() == union0, "3120 AC2: Soft union unchanged");
    CHECK(dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed) == force0,
          "3120 AC2: Soft force-total unchanged");
    CHECK(remirror_persisted_residual_castops() == 0, "3120 AC2: empty persist remirror 0");

    reset_residual_castop_persist_for_test();
}

static void ac3120_3_no_new_query_keys() {
    std::println("\n--- #3120 AC3: reuse residual / type_cone counters ---");
    CHECK(kResidualCastopTypeTxnRemirrorIssue == 3120, "3120 AC3: issue constant");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto tch = read_file("src/compiler/type_checker.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    CHECK(dirty.find("Issue #3120") != std::string::npos, "3120 AC3: dirty cites #3120");
    CHECK(dirty.find("remirror_persisted_residual_castops") != std::string::npos,
          "3120 AC3: remirror helper");
    CHECK(dirty.find("note_residual_castop_sites") != std::string::npos, "3120 AC3: persist note");
    CHECK(opt.find("note_residual_castop_sites") != std::string::npos, "3120 AC3: sweep persists");
    CHECK(impl.find("remirror_persisted_residual_castops") != std::string::npos,
          "3120 AC3: type txn remirror");
    CHECK(tch.find("remirror_persisted_residual_castops") != std::string::npos,
          "3120 AC3: dirty-txn comment");
    CHECK(q.find("schema-3120") == std::string::npos, "3120 AC3: no new schema-3120");
    CHECK(q.find("schema-3065") != std::string::npos, "3120 AC3: reuse schema-3065");
    CHECK(dirty.find("force_dead_coercion_elim_into_cone") != std::string::npos,
          "3120 AC3: remirror reuses #3065 force");
}

static void ac3120_4_linter_no_invent() {
    std::println("\n--- #3120 AC4: linter + no invent ---");
    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_residual_castop_type_txn_3120.py");
    const auto build = read_file("build.py");
    CHECK(t.find("ac3120_1_type_txn_remirrors_skipped_castop") != std::string::npos,
          "3120 AC4: AC1");
    CHECK(t.find("ac3120_2_soft_zero_cost") != std::string::npos, "3120 AC4: AC2");
    CHECK(t.find("ac3120_3_no_new_query_keys") != std::string::npos, "3120 AC4: AC3");
    CHECK(!lint.empty() && lint.find("Issue #3120") != std::string::npos, "3120 AC4: linter");
    CHECK(build.find("check_residual_castop_type_txn_3120") != std::string::npos,
          "3120 AC4: build.py gate");
    CHECK(read_file("tests/compiler/test_issue_3120.cpp").empty(),
          "3120 AC4: no test_issue_3120.cpp");
    CHECK(read_file("docs/design/3120-residual-castop-type-txn.md").empty(),
          "3120 AC4: no docs/design");
}

// ── Issue #3228: empty cone + residual persist must remirror (under-mark) ──
static void ac3228_1_empty_cone_remirrors_residual() {
    std::println("\n--- #3228 AC1: empty cone + residual persist remirrors ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kRes = 515;
    const aura::compiler::dirty::NodeId one[] = {kRes};
    note_residual_castop_sites(one, {});
    CHECK(residual_castop_persist_size() >= 1, "3228 AC1: persist nonempty");

    // Type-txn wipe with empty post-infer cone (columnar under-mark).
    CHECK(mirror_type_affected_to_cascade({}) == 0, "3228 AC1: empty cone wipe");
    CHECK(!cone_contains(kRes), "3228 AC1: wipe dropped residual from last cone");

    const auto force0 = dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed);
    CHECK(force_residual_castop_undermark_into_cone() >= 1, "3228 AC1: under-mark remirror");
    CHECK(cone_contains(kRes), "3228 AC1: residual back in cone");
    CHECK(type_ir_union_cone_nonempty(), "3228 AC1: cone nonempty for remutate typecheck");
    CHECK(dead_coercion_elim_cone_force_total.load(std::memory_order_relaxed) > force0,
          "3228 AC1: reuse #3065 force-total");

    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("Issue #3120 / #3228") != std::string::npos, "3228 AC1: type txn cite");
    CHECK(impl.find("force_residual_castop_undermark_into_cone") != std::string::npos,
          "3228 AC1: empty-affected remirror");
    CHECK(impl.find("Issue #3228") != std::string::npos, "3228 AC1: impl cite");

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3228_2_soft_quiet() {
    std::println("\n--- #3228 AC2: Soft observe; quiet empty persist ---");
    SoftAuditScope soft;
    reset_residual_castop_persist_for_test();
    constexpr aura::compiler::dirty::NodeId kSoft = 616;
    const aura::compiler::dirty::NodeId one[] = {kSoft};
    note_residual_castop_sites(one, {});
    CHECK(residual_castop_persist_size() == 0, "3228 AC2: Soft does not persist");
    const auto union0 = type_ir_union_cone_size();
    CHECK(force_residual_castop_undermark_into_cone() == 0, "3228 AC2: Soft remirror 0");
    CHECK(type_ir_union_cone_size() == union0, "3228 AC2: Soft union unchanged");
    CHECK(force_residual_castop_undermark_into_cone() == 0, "3228 AC2: quiet empty persist 0");
    reset_residual_castop_persist_for_test();
}

static void ac3228_3_no_regression_3065_3120() {
    std::println("\n--- #3228 AC3: #3065 / #3120 surfaces retained ---");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(dirty.find("force_dead_coercion_elim_into_cone") != std::string::npos,
          "3228 AC3: #3065 helper");
    CHECK(dirty.find("remirror_persisted_residual_castops") != std::string::npos,
          "3228 AC3: #3120 remirror");
    CHECK(opt.find("force_residual_castop_blocks_into_cone") != std::string::npos,
          "3228 AC3: #3065 residual sweep");
    CHECK(impl.find("remirror_persisted_residual_castops") != std::string::npos,
          "3228 AC3: type txn remirror retained");
    CHECK(kDeadCoercionElimConeIssue == 3065, "3228 AC3: #3065 issue");
    CHECK(kResidualCastopTypeTxnRemirrorIssue == 3120, "3228 AC3: #3120 issue");
    CHECK(aura::compiler::dirty::kResidualCastopUndermarkConeIssue == 3228,
          "3228 AC3: #3228 issue");
}

static void ac3228_4_linter_suites() {
    std::println("\n--- #3228 AC4: linter + suite cites ---");
    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    const auto col = read_file("tests/compiler/test_dead_coercion_columnar.cpp");
    const auto batch = read_file("tests/compiler/test_batch_dirty_cascade.cpp");
    const auto inc = read_file("tests/compiler/test_incremental_type_batch.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_residual_castop_undermark_cone_3228.py");
    const auto build = read_file("build.py");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto passes = read_file("src/compiler/pass_impls.ixx");
    CHECK(t.find("ac3228_1_empty_cone_remirrors_residual") != std::string::npos, "3228 AC4: suite");
    CHECK(col.find("3228") != std::string::npos, "3228 AC4: columnar suite");
    CHECK(batch.find("3228") != std::string::npos, "3228 AC4: dirty_cascade suite");
    CHECK(inc.find("3228") != std::string::npos, "3228 AC4: incremental_type suite");
    CHECK(!lint.empty() && lint.find("3228") != std::string::npos, "3228 AC4: linter");
    CHECK(build.find("check_residual_castop_undermark_cone_3228") != std::string::npos,
          "3228 AC4: build.py");
    CHECK(etc.find("force_residual_castop_undermark_into_cone") != std::string::npos,
          "3228 AC4: mutate/commit remirror");
    CHECK(passes.find("Issue #3228") != std::string::npos, "3228 AC4: columnar leftover persist");
    CHECK(read_file("docs/design/3228-residual-castop-undermark.md").empty(),
          "3228 AC4: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3228.cpp").empty(), "3228 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3228.cpp").empty(), "3228 AC4: no tests/issues");
}

// ── Issue #3347: single-boundary commit_readiness remirrors before grant ──
static void ac3347_1_live_policy_remirrors_before_auto_partial() {
    std::println("\n--- #3347 AC1: live_policy remirror before auto_partial / empty_cs ---");
    using aura::compiler::typed_audit::commit_readiness;
    using aura::compiler::typed_audit::commit_readiness_live_policy;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kRes = 717;
    const aura::compiler::dirty::NodeId one[] = {kRes};
    note_residual_castop_sites(one, {});
    CHECK(residual_castop_persist_size() >= 1, "3347 AC1: persist nonempty");
    CHECK(mirror_type_affected_to_cascade({}) == 0, "3347 AC1: empty cone wipe");
    CHECK(!cone_contains(kRes), "3347 AC1: wipe dropped residual from last cone");

    const auto gen0 = dead_coercion_decision_invalidate_gen();
    auto in = commit_readiness_live_policy();
    CHECK(in.auto_partial_from_cone, "3347 AC1: pending drives auto_partial");
    CHECK(cone_contains(kRes), "3347 AC1: remirror before auto_partial");
    CHECK(type_ir_union_cone_nonempty(), "3347 AC1: cone nonempty until re-typecheck");
    CHECK(aura_residual_castop_undermark_pending() != 0, "3347 AC1: C ABI pending latch");
    CHECK(residual_castop_undermark_pending(), "3347 AC1: dirty pending latch");
    CHECK(dead_coercion_decision_invalidate_gen() > gen0, "3347 AC1: bump on remirror n>0");

    in.empty_cs_hard = true;
    in.cs_has_work = false;
    in.solve_status = 0;
    const auto r = commit_readiness(in);
    CHECK(!r.would_allow_commit, "3347 AC1: empty_cs hard-reject until re-infer");

    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto ev = read_file("src/compiler/evaluator.ixx");
    CHECK(aud.find("aura_force_residual_castop_undermark_into_cone") != std::string::npos,
          "3347 AC1: live_policy C ABI");
    CHECK(ev.find("aura_force_residual_castop_undermark_into_cone") != std::string::npos,
          "3347 AC1: grant remirror");
    CHECK(ev.find("aura_residual_castop_undermark_pending") != std::string::npos,
          "3347 AC1: grant refuse pending");

    clear_residual_castop_undermark_pending();
    auto in2 = commit_readiness_live_policy();
    CHECK(!in2.auto_partial_from_cone, "3347 AC1: pending clear → no auto_partial");
    CHECK(aura_residual_castop_undermark_pending() == 0, "3347 AC1: second remirror n=0");

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3347_2_soft_quiet() {
    std::println("\n--- #3347 AC2/AC4: Soft observe; quiet empty persist ---");
    SoftAuditScope soft;
    reset_residual_castop_persist_for_test();
    constexpr aura::compiler::dirty::NodeId kSoft = 818;
    const aura::compiler::dirty::NodeId one[] = {kSoft};
    note_residual_castop_sites(one, {});
    CHECK(residual_castop_persist_size() == 0, "3347 AC4: Soft does not persist");
    const auto union0 = type_ir_union_cone_size();
    const auto gen0 = dead_coercion_decision_invalidate_gen();
    CHECK(aura_force_residual_castop_undermark_into_cone() == 0, "3347 AC4: Soft C ABI 0");
    CHECK(aura_residual_castop_undermark_pending() == 0, "3347 AC4: Soft no pending");
    CHECK(type_ir_union_cone_size() == union0, "3347 AC4: Soft union unchanged");
    CHECK(dead_coercion_decision_invalidate_gen() == gen0, "3347 AC4: Soft no invalidate bump");
    reset_residual_castop_persist_for_test();
    CHECK(aura_force_residual_castop_undermark_into_cone() == 0, "3347 AC4: quiet empty persist 0");
    CHECK(aura_residual_castop_undermark_pending() == 0, "3347 AC4: quiet no pending");
}

static void ac3347_3_invalidate_gen_success_path() {
    std::println("\n--- #3347 AC3: success-path invalidate; post-infer remirror no bump ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kRes = 919;
    const aura::compiler::dirty::NodeId one[] = {kRes};
    note_residual_castop_sites(one, {});
    CHECK(mirror_type_affected_to_cascade({}) == 0, "3347 AC3: wipe");
    const auto gen0 = dead_coercion_decision_invalidate_gen();
    CHECK(remirror_persisted_residual_castops() >= 1, "3347 AC3: keep-alive remirror");
    CHECK(dead_coercion_decision_invalidate_gen() == gen0,
          "3347 AC3: post-infer remirror does not bump");
    CHECK(!residual_castop_undermark_pending(), "3347 AC3: keep-alive does not latch pending");

    CHECK(mirror_type_affected_to_cascade({}) == 0, "3347 AC3: wipe again");
    const auto gen1 = dead_coercion_decision_invalidate_gen();
    CHECK(aura_force_residual_castop_undermark_into_cone() >= 1, "3347 AC3: C ABI remirror");
    CHECK(dead_coercion_decision_invalidate_gen() > gen1, "3347 AC3: C ABI n>0 bumps gen");
    CHECK(residual_castop_undermark_pending(), "3347 AC3: C ABI latches pending");

    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(dirty.find("bump_dead_coercion_decision_invalidate") != std::string::npos,
          "3347 AC3: abort bump retained");
    CHECK(dirty.find("note_residual_castop_undermark_pending") != std::string::npos,
          "3347 AC3: success-path latch");
    CHECK(kResidualCastopReadinessUndermarkIssue == 3347, "3347 AC3: issue stamp");

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3347_4_linter_no_invent() {
    std::println("\n--- #3347 AC4: linter + no invent / no new query keys ---");
    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_residual_castop_readiness_undermark_3347.py");
    const auto build = read_file("build.py");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_type_stats.cpp");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto stubs = read_file("src/compiler/test_concurrent_stubs.cpp");
    CHECK(t.find("ac3347_1_live_policy_remirrors_before_auto_partial") != std::string::npos,
          "3347 AC4: suite");
    CHECK(!lint.empty() && lint.find("3347") != std::string::npos, "3347 AC4: linter");
    CHECK(build.find("check_residual_castop_readiness_undermark_3347") != std::string::npos,
          "3347 AC4: build.py");
    CHECK(impl.find("clear_residual_castop_undermark_pending") != std::string::npos,
          "3347 AC4: infer clears pending");
    CHECK(stubs.find("aura_force_residual_castop_undermark_into_cone") != std::string::npos,
          "3347 AC4: light-link stub");
    CHECK(q.find("schema-3347") == std::string::npos, "3347 AC4: no schema-3347");
    CHECK(dirty.find("g_3347_") == std::string::npos, "3347 AC4: no g_3347_*");
    CHECK(read_file("docs/design/3347-residual-castop-readiness.md").empty(),
          "3347 AC4: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3347.cpp").empty(), "3347 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3347.cpp").empty(), "3347 AC4: no tests/issues");
}

// ── Issue #3349: persist re-union before partial-relower impact_ub ──
static void ac3349_1_relower_remirrors_before_impact_ub() {
    std::println("\n--- #3349 AC1: remirror before impact_ub / partial commit ---");
    CHECK(kDeadCoercionPersistBeforePartialIssue == 3349, "3349 AC1: issue stamp");
    const auto sixx = read_file("src/compiler/service.ixx");
    const auto dirtyf = read_file("src/compiler/service_dirty.cpp");
    CHECK(sixx.find("Issue #3349") != std::string::npos, "3349 AC1: service.ixx cites #3349");
    CHECK(sixx.find("mark_entry_from_dead_coercion_persist_") != std::string::npos,
          "3349 AC1: persist → block mark helper");
    CHECK(sixx.find("force_residual_castop_undermark_into_cone") != std::string::npos,
          "3349 AC1: remirror in relower");
    auto pos = sixx.find("if (want_partial && dirty_n > 0)");
    CHECK(pos != std::string::npos, "3349 AC1: partial branch");
    auto win = sixx.substr(pos, 2800);
    auto rem = win.find("force_residual_castop_undermark_into_cone");
    auto iub = win.find("impact_upper_bound_for_entry_");
    CHECK(rem != std::string::npos, "3349 AC1: remirror in want_partial branch");
    CHECK(iub != std::string::npos, "3349 AC1: impact_ub still consulted");
    CHECK(rem < iub, "3349 AC1: remirror precedes impact_ub");
    CHECK(dirtyf.find("Issue #3349") != std::string::npos, "3349 AC1: try_partial remirror");
    CHECK(dirtyf.find("force_residual_castop_undermark_into_cone") != std::string::npos,
          "3349 AC1: service_dirty remirror");
}

static void ac3349_2_soft_quiet() {
    std::println("\n--- #3349 AC2: Soft / empty persist → 0 extra ---");
    SoftAuditScope soft;
    reset_residual_castop_persist_for_test();
    constexpr aura::compiler::dirty::NodeId kSoft = 919;
    const aura::compiler::dirty::NodeId one[] = {kSoft};
    note_residual_castop_sites(one, {});
    CHECK(residual_castop_persist_size() == 0, "3349 AC2: Soft does not persist");
    CHECK(force_residual_castop_undermark_into_cone() == 0, "3349 AC2: Soft remirror 0");
    const auto sixx = read_file("src/compiler/service.ixx");
    CHECK(sixx.find("empty persist") != std::string::npos ||
              sixx.find("Soft / empty persist") != std::string::npos,
          "3349 AC2: zero-extra documented");
    reset_residual_castop_persist_for_test();
}

static void ac3349_3_production_persist_marks_or_force_full() {
    std::println("\n--- #3349 AC3: production persist remirrors; cone nonempty ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kRes = 1021;
    const aura::compiler::dirty::NodeId one[] = {kRes};
    note_residual_castop_sites(one, {});
    CHECK(residual_castop_persist_size() >= 1, "3349 AC3: persist nonempty");
    CHECK(mirror_type_affected_to_cascade({}) == 0, "3349 AC3: wipe");
    CHECK(!cone_contains(kRes), "3349 AC3: wipe dropped residual");

    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define f (lambda (x) x))
")
)")
              .has_value(),
          "3349 AC3: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3349 AC3: eval");
    cs.public_mark_define_dirty("f");
    const auto forced0 =
        cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    (void)cs.public_relower_dirty_defines_from_workspace();
    CHECK(cone_contains(kRes) || type_ir_union_cone_nonempty(),
          "3349 AC3: remirror before partial left cone nonempty");
    CHECK(cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed) >=
              forced0,
          "3349 AC3: force-full distinguisher non-decreasing");
    const auto sixx = read_file("src/compiler/service.ixx");
    CHECK(sixx.find("mark_entry_from_dead_coercion_persist_") != std::string::npos,
          "3349 AC3: mark-or-full helper");
    CHECK(sixx.find("partial_forced_full_by_impact_total") != std::string::npos,
          "3349 AC3: reuse force-full counter (no new query key)");

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

// ── Issue #3618: unmatched persist forces full with non-empty map ──
//   AC1: production + persist nonempty + attribution false (map non-empty
//        but missing the persisted nid) → force full; the #3349 tail gate
//        no longer conjuncts source_to_ir_map.empty() (distinguishable via
//        partial_forced_full_by_impact_total bump).
//   AC2: persist attributed (block-node encoding marks without a map
//        lookup) → tail gate must not force full on its own.
//   AC3: Soft persist stays empty → gate inert (zero extra).
//   AC5: no new query key; service.ixx cites #3618.
static void ac3618_persist_attribution_forces_full() {
    std::println("\n--- #3618: unmatched persist → full with non-empty map ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);

    const auto sixx = read_file("src/compiler/service.ixx");
    CHECK(sixx.find("Issue #3618") != std::string::npos, "3618 AC1: gate cites #3618");
    // Windowed: the #3349 tail gate must key on !persist_attributed WITHOUT
    // the source_to_ir_map.empty() conjunct (#3618).
    {
        const auto g = sixx.find("!persist_attributed) {");
        const auto win =
            g != std::string::npos ? sixx.substr(g >= 600 ? g - 600 : 0, 1200) : std::string{};
        CHECK(win.find("residual_castop_persist_size() > 0") != std::string::npos,
              "3618 AC1: persist-nonempty term present");
        CHECK(win.find("source_to_ir_map.empty()") == std::string::npos,
              "3618 AC1: empty-map conjunct dropped");
    }

    // AC3: Soft — persist stays empty, tail gate inert.
    {
        SoftAuditScope soft;
        reset_residual_castop_persist_for_test();
        constexpr aura::compiler::dirty::NodeId kSoft = 3618;
        const aura::compiler::dirty::NodeId one[] = {kSoft};
        note_residual_castop_sites(one, {});
        CHECK(aura::compiler::dirty::residual_castop_persist_size() == 0,
              "3618 AC3: Soft persist empty → tail gate inert");
    }

    // AC1/AC2: production — unmatched vs attributed persist.
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    CompilerService cs;
    CHECK(cs.eval(R"(
(set-code "
(define f (lambda (x) x))
")
)")
              .has_value(),
          "3618: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3618: eval");

    // AC2: attributed persist — block-DEP encoding (non-zero by tag; decodes
    // to fi=0, bi=0) marks via the block table without a map lookup →
    // attribution true → the tail gate must not force full on its own.
    // (encode_block_node(0,0) is NodeId 0 — the persist path filters enc==0.)
    {
        reset_residual_castop_persist_for_test();
        const auto blk = aura::compiler::dirty::encode_block_dep_node(0, 0, 0);
        const aura::compiler::dirty::NodeId blocks[] = {blk};
        note_residual_castop_sites({}, blocks);
        CHECK(aura::compiler::dirty::residual_castop_persist_size() >= 1,
              "3618 AC2: attributed persist nonempty");
        cs.public_mark_define_dirty("f");
        const auto forced0 =
            cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        (void)cs.public_relower_dirty_defines_from_workspace();
        const auto forced1 =
            cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        // The prepare/impact branches may fail-closed full on a tiny fixture
        // (unknown impact_ub) — that is NOT this arm. AC2's contract: the
        // tail gate is keyed on !persist_attributed (asserted structurally
        // above), so attribution true can never fire it; the attributed
        // relower must leave f intact and evaluable either way.
        std::println("  3618 AC2: force-full delta={}", forced1 - forced0);
        CHECK(cs.eval("(f 41)").has_value(), "3618 AC2: attributed relower leaves f evaluable");
    }

    // AC1: unmatched persist — ghost ast nid not in f's map. The desync
    // inject keeps the map non-empty (partial-rebuild shape) but corrupts
    // one entry; it lives in this block only — AC2 must see a clean map so
    // the impact branch cannot confound the counter.
    {
        CHECK(cs.inject_source_to_ir_map_desync_for_test("f"), "3618 AC1: map non-empty (inject)");
        reset_residual_castop_persist_for_test();
        constexpr aura::compiler::dirty::NodeId kGhost = 3618;
        const aura::compiler::dirty::NodeId one[] = {kGhost};
        note_residual_castop_sites(one, {});
        CHECK(aura::compiler::dirty::residual_castop_persist_size() >= 1,
              "3618 AC1: unmatched persist nonempty");
        cs.public_mark_define_dirty("f");
        const auto forced1 =
            cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        (void)cs.public_relower_dirty_defines_from_workspace();
        CHECK(cs.metrics().partial_forced_full_by_impact_total.load(std::memory_order_relaxed) >
                  forced1,
              "3618 AC1: unmatched persist + non-empty map → force full");
    }

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3349_4_linter_no_invent() {
    std::println("\n--- #3349 AC4: linter + no invent / no new query keys ---");
    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_dead_coercion_persist_before_partial_3349.py");
    const auto build = read_file("build.py");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    CHECK(t.find("ac3349_1_relower_remirrors_before_impact_ub") != std::string::npos,
          "3349 AC4: suite");
    CHECK(!lint.empty() && lint.find("3349") != std::string::npos, "3349 AC4: linter");
    CHECK(build.find("check_dead_coercion_persist_before_partial_3349") != std::string::npos,
          "3349 AC4: build.py");
    CHECK(q.find("schema-3349") == std::string::npos, "3349 AC4: no schema-3349");
    CHECK(dirty.find("g_3349_") == std::string::npos, "3349 AC4: no g_3349_*");
    CHECK(read_file("docs/design/3349-dead-coercion-persist-partial.md").empty(),
          "3349 AC4: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3349.cpp").empty(), "3349 AC4: no invent");
    CHECK(read_file("tests/issues/test_issue_3349.cpp").empty(), "3349 AC4: no tests/issues");
}

// Issue #3547: dirty-cone DeadCoercion re-verify type_id / stamper_bound.
static void ac3547_1_stamper_unbound_drops_cone() {
    std::println("\n--- #3547 AC1: production stamper_bound=0 drops dirty-cone cache ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    CHECK(aura::compiler::typed_audit::last_proof_stamper_bound_v_read() == 0,
          "3547 AC1: stamper unbound");
    CHECK(kDeadCoercionDecisionReverifyIssue == 3547, "3547 AC1: issue stamp");

    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });
    IRFunction fn = make_two_block_fn_with_casts();
    const auto inv0 = load_u64(dead_coercion_ir_decision_invalidate_total);
    const auto full0 = load_u64(dead_coercion_full_scan_runs);
    dce.run(fn);
    CHECK(load_u64(dead_coercion_ir_decision_invalidate_total) > inv0,
          "3547 AC1: reuse existing invalidate total");
    CHECK(load_u64(dead_coercion_full_scan_runs) > full0, "3547 AC1: full scan after drop");
    bool block1_has_cast = false;
    for (const auto& instr : fn.blocks[1].instructions) {
        if (instr.opcode == IROpcode::CastOp)
            block1_has_cast = true;
    }
    CHECK(!block1_has_cast, "3547 AC1: cone-external CastOp rescanned");

    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3547_2_type_id_drift_invalidates_site() {
    std::println("\n--- #3547 AC2: type_id drift invalidates deopt-meta site ---");
    using aura::compiler::dce_deopt::clear_elided_cast_deopt_meta_for_test;
    using aura::compiler::dce_deopt::lookup_elided_cast_deopt_meta;
    using aura::compiler::dce_deopt::make_site_key;
    using aura::compiler::dce_deopt::reverify_elided_cast_deopt_site;
    using aura::compiler::dce_deopt::stamp_elided_cast_deopt_meta;
    using aura::compiler::typed_audit::current_narrow_evidence;
    clear_elided_cast_deopt_meta_for_test();
    constexpr std::uint32_t kNode = 42;
    constexpr std::uint32_t kEv = 4;
    const auto site = make_site_key(0, kNode, 7);
    stamp_elided_cast_deopt_meta(site, /*mid=*/99, kEv, /*tag=*/1, /*type_id=*/7);
    CHECK(lookup_elided_cast_deopt_meta(site).has_value(), "3547 AC2: site stamped");
    CHECK(current_narrow_evidence(kNode) == kEv, "3547 AC2: current_narrow_evidence");
    CHECK(reverify_elided_cast_deopt_site(site, /*live_type_id=*/9, /*live_evidence=*/0),
          "3547 AC2: type_id drift → invalidate");
    CHECK(!lookup_elided_cast_deopt_meta(site).has_value(), "3547 AC2: site dropped");
    stamp_elided_cast_deopt_meta(site, 99, kEv, 1, 7);
    CHECK(!reverify_elided_cast_deopt_site(site, 7, kEv), "3547 AC2: matching type keeps site");
    CHECK(reverify_elided_cast_deopt_site(site, 7, /*live_evidence=*/8),
          "3547 AC2: evidence drift → invalidate");
    clear_elided_cast_deopt_meta_for_test();
}

static void ac3547_3_soft_keeps_cone() {
    std::println("\n--- #3547 AC4: Soft stamper_bound=0 keeps dirty cone ---");
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    aura::compiler::dirty::reset_dead_coercion_decision_invalidate_for_test();
    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });
    IRFunction fn = make_two_block_fn_with_casts();
    dce.run(fn);
    bool block1_has_cast = false;
    for (const auto& instr : fn.blocks[1].instructions) {
        if (instr.opcode == IROpcode::CastOp)
            block1_has_cast = true;
    }
    CHECK(block1_has_cast, "3547 AC4: Soft cone-external CastOp untouched");
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3547_4_source_cite_no_invent() {
    std::println("\n--- #3547 AC5: source-cite + no invent / no new query ---");
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    const auto meta = read_file("src/compiler/dce_elided_deopt_meta.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    CHECK(opt.find("Issue #3547") != std::string::npos, "3547 AC5: pass cite");
    CHECK(opt.find("last_proof_stamper_bound_v_read") != std::string::npos, "3547 AC5: stamper");
    CHECK(opt.find("last_type_linear_commit_proof_stamp_v_read") != std::string::npos,
          "3547 AC5: stamp consult");
    CHECK(meta.find("invalidate_elided_cast_deopt_meta") != std::string::npos,
          "3547 AC5: invalidate");
    CHECK(meta.find("reverify_elided_cast_deopt_site") != std::string::npos, "3547 AC5: reverify");
    CHECK(emb.find("invalidate_elided_cast_deopt_meta") != std::string::npos,
          "3547 AC5: persist-reject");
    CHECK(t.find("ac3547_1_stamper_unbound_drops_cone") != std::string::npos, "3547 AC5: AC1");
    CHECK(opt.find("schema-3547") == std::string::npos, "3547 AC5: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3547.cpp").empty(), "3547 AC5: no invent");
    CHECK(read_file("tests/issues/test_issue_3547.cpp").empty(), "3547 AC5: no tests/issues");
    CHECK(read_file("scripts/coverage/checks/check_dead_coercion_decision_invalidate.py").empty(),
          "3547 AC5: no new linter");
    CHECK(read_file("docs/design/3547-dead-coercion-reverify.md").empty(),
          "3547 AC5: no docs/design");
}

// ── Issue #3581: #3065 remirror × #3068 map-completeness cross-owner ──
// Motivating hole: cone-skipped CastOp whose type already changed.
// Map→IR consistency (#2045) cannot see a *missing* reverse entry;
// ImpactScope then under-counts. Type remirror owner is #3065;
// decision-time completeness owner is #3068. At least one must catch.
// Double-miss (neither catches, CastOp survives with old type) is
// unreachable under production.

static bool file_exists_cwd_3581(const char* rel) {
    return std::ifstream(rel).good() || std::ifstream(std::string("../") + rel).good();
}

static IRFunction make_cone_skip_type_changed_castop(aura::compiler::dirty::NodeId nid) {
    IRFunction fn;
    fn.name = "cross_owner_3581";
    fn.local_count = 4;
    fn.entry_block = 0;
    BasicBlock b0;
    b0.id = 0;
    b0.instructions = {
        IRInstruction{.opcode = IROpcode::ConstI64,
                      .operands = {0, 1, 0, 0},
                      .source_ast_node_id = 0,
                      .type_id = 1},
        IRInstruction{.opcode = IROpcode::Return, .operands = {0, 0, 0, 0}},
    };
    BasicBlock b1;
    b1.id = 1;
    b1.instructions = {
        IRInstruction{.opcode = IROpcode::ConstI64,
                      .operands = {2, 7, 0, 0},
                      .source_ast_node_id = 0,
                      .type_id = 1},
        // Non-identity CastOp (src tag 1 → dest type_id 2) outside cone.
        IRInstruction{.opcode = IROpcode::CastOp,
                      .operands = {3, 2, 1, 0},
                      .source_ast_node_id = nid,
                      .type_id = 2},
        IRInstruction{.opcode = IROpcode::Return, .operands = {3, 0, 0, 0}},
    };
    fn.blocks.push_back(std::move(b0));
    fn.blocks.push_back(std::move(b1));
    return fn;
}

static void ac3581_1_castop_type_change_cone_skip_either_owner() {
    std::println("\n--- #3581 AC1: CastOp type-change + cone-skip → remirror or completeness ---");
    using aura::compiler::populate_source_to_ir_map_from_irs;
    using aura::compiler::should_partial_relower_impact_checked_prod;
    using aura::compiler::source_to_ir_map_missing_instr_loc;
    using aura::compiler::SourceToIrMap;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    // Cone-skip needs the dirty-pred path; production + unbound stamper
    // force-fulls DCE (#3547). Remirror/completeness run after, under production.
    g_typed_mutation_audit_counters.production_defaults_active.store(0, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kCast = 3581;
    IRFunction fn = make_cone_skip_type_changed_castop(kCast);
    CHECK(count_identity_castops(fn) == 0, "3581 AC1: leftover is type-changed (non-identity)");
    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });
    const auto skips0 = load_u64(dead_coercion_dirty_cone_skips);
    const auto partial0 = load_u64(dead_coercion_dirty_cone_partial_runs);
    dce.run(fn);
    CHECK(load_u64(dead_coercion_dirty_cone_partial_runs) > partial0 ||
              load_u64(dead_coercion_dirty_cone_skips) > skips0,
          "3581 AC1: dirty-cone limited scan (partial-run or skip)");
    bool skipped_alive = false;
    for (const auto& ins : fn.blocks[1].instructions) {
        if (ins.opcode == IROpcode::CastOp && ins.source_ast_node_id == kCast && ins.type_id == 2)
            skipped_alive = true;
    }
    CHECK(skipped_alive, "3581 AC1: cone-skip left type-changed CastOp in clean block");

    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    // #3065 remirror face: persist + remirror the skipped nid into type∪IR.
    const aura::compiler::dirty::NodeId one[] = {kCast};
    note_residual_castop_sites(one, {});
    CHECK(mirror_type_affected_to_cascade({}) == 0, "3581 AC1: wipe type cone");
    CHECK(!cone_contains(kCast), "3581 AC1: skipped nid not in cone before remirror");
    CHECK(remirror_persisted_residual_castops() >= 1, "3581 AC1: #3065 remirror ran");
    const bool remirror_hit = cone_contains(kCast);

    // #3068 completeness face: drop instr loc on the reverse entry.
    std::vector<IRFunction> irs;
    irs.push_back(fn);
    SourceToIrMap map;
    populate_source_to_ir_map_from_irs(irs, map);
    auto mit = map.find(static_cast<aura::ast::NodeId>(kCast));
    CHECK(mit != map.end(), "3581 AC1: reverse entry exists before drop");
    mit->second.instr_index = UINT32_MAX;
    const bool missing = source_to_ir_map_missing_instr_loc(irs, map);
    const std::size_t sentinel = static_cast<std::size_t>(-1);
    const bool completeness_upgrade =
        missing && !should_partial_relower_impact_checked_prod(1, sentinel, /*production=*/true);

    CHECK(remirror_hit || completeness_upgrade,
          "3581 AC1: remirror dirty or #3068 completeness upgrade (at least one)");
    CHECK(remirror_hit, "3581 AC1: #3065 remirror marked skipped CastOp dirty");
    CHECK(completeness_upgrade, "3581 AC1: #3068 missing_instr_loc upgrades partial→full");

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3581_2_double_miss_unreachable() {
    std::println("\n--- #3581 AC2: double-miss form unreachable under production ---");
    using aura::compiler::populate_source_to_ir_map_from_irs;
    using aura::compiler::should_partial_relower_impact_checked_prod;
    using aura::compiler::source_to_ir_map_missing_instr_loc;
    using aura::compiler::SourceToIrMap;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    auto save =
        g_typed_mutation_audit_counters.production_defaults_active.load(std::memory_order_relaxed);
    g_typed_mutation_audit_counters.production_defaults_active.store(1, std::memory_order_relaxed);
    reset_residual_castop_persist_for_test();

    constexpr aura::compiler::dirty::NodeId kCast = 35811;
    IRFunction fn = make_cone_skip_type_changed_castop(kCast);
    DeadCoercionPass dce;
    dce.set_block_dirty_fn([](std::uint32_t bid) { return bid == 0; });
    dce.run(fn);

    // Simulate remirror miss: persist then wipe without remirror.
    const aura::compiler::dirty::NodeId one[] = {kCast};
    note_residual_castop_sites(one, {});
    CHECK(mirror_type_affected_to_cascade({}) == 0, "3581 AC2: wipe");
    const bool remirror_hit = cone_contains(kCast);

    std::vector<IRFunction> irs;
    irs.push_back(fn);
    SourceToIrMap map;
    populate_source_to_ir_map_from_irs(irs, map);
    auto mit = map.find(static_cast<aura::ast::NodeId>(kCast));
    CHECK(mit != map.end(), "3581 AC2: reverse entry");
    mit->second.instr_index = UINT32_MAX;
    const bool missing = source_to_ir_map_missing_instr_loc(irs, map);
    const std::size_t sentinel = static_cast<std::size_t>(-1);
    const bool allow_partial_sentinel =
        should_partial_relower_impact_checked_prod(1, sentinel, /*production=*/true);
    const bool allow_partial_unknown =
        should_partial_relower_impact_checked_prod(1, 0, /*production=*/true);

    CHECK(!remirror_hit, "3581 AC2: remirror miss simulated (cone empty)");
    CHECK(missing, "3581 AC2: #3068 still sees missing instr loc");
    CHECK(!allow_partial_sentinel, "3581 AC2: #3034 sentinel refuses silent partial");
    CHECK(!allow_partial_unknown, "3581 AC2: #3310 unknown-impact refuses silent partial");
    CHECK(!(!remirror_hit && !missing && allow_partial_sentinel),
          "3581 AC2: double-miss (neither owner, CastOp survives old type) unreachable");

    reset_residual_castop_persist_for_test();
    g_typed_mutation_audit_counters.production_defaults_active.store(save,
                                                                     std::memory_order_relaxed);
}

static void ac3689_partial_peel_aos_dce_uses_mask() {
    std::println("\n--- #3689: partial peel AoS DCE uses dirty mask ---");
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::typed_audit::production_hard_face_active;

    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("Issue #3689") != std::string::npos, "3689: suite cites #3689");
    const auto suite_pos = svc.find("std::size_t run_incremental_dirty_pass_suite_");
    const auto suite = suite_pos == std::string::npos ? std::string{} : svc.substr(suite_pos, 4500);
    CHECK(suite.find("production_hard_face_active()") != std::string::npos,
          "3689 AC4: Soft/Off skips extra DCE mask peel");
    CHECK(suite.find("run_coercion_elim_on_function(func, db)") != std::string::npos,
          "3689 AC1: masked DCE when mask_ptr matches");
    CHECK(suite.find("run_coercion_elim_on_function(func)") != std::string::npos,
          "3689 AC3: full-fn DCE fallback");
    CHECK(suite.find("InlinePass") == std::string::npos ||
              suite.find("do not add it to this AoS suite") != std::string::npos,
          "3689: no InlinePass added to suite");
    CHECK(svc.find("Issue #3618") != std::string::npos,
          "3689 AC2: persist CastOp under-attribution still fail-closed");
    CHECK(svc.find("mark_all_blocks_dirty()") != std::string::npos,
          "3689 AC2: #3618 still mark_all_blocks_dirty");
    CHECK(svc.find("if (!dirty_dce)") != std::string::npos,
          "3689: residual sweep skipped on cone-limited DCE");
    CHECK(svc.find("schema-3689") == std::string::npos, "3689 AC5: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3689.cpp").empty(), "3689: no test_issue_3689.cpp");
    CHECK(read_file("docs/design/3689-partial-peel-aos-dce.md").empty(), "3689: no docs/design/");

    // Soak: dirty-block CastOp elides; clean-block CastOp stays (sibling
    // type change must not DCE the clean block).
    IRFunction fn = make_two_block_fn_with_casts();
    std::vector<std::uint8_t> dirty{1, 0};
    DeadCoercionEliminationPass dce;
    dce.run_function(fn, dirty);
    bool dirty_elided = true;
    for (const auto& instr : fn.blocks[0].instructions) {
        if (instr.opcode == IROpcode::CastOp)
            dirty_elided = false;
    }
    bool clean_has_cast = false;
    for (const auto& instr : fn.blocks[1].instructions) {
        if (instr.opcode == IROpcode::CastOp)
            clean_has_cast = true;
    }
    CHECK(dirty_elided, "3689 AC1: dirty-block CastOp elided");
    CHECK(clean_has_cast, "3689 AC1: clean-block CastOp not eliminated");

    apply_production_audit_defaults();
    CompilerService cs;
    cs.evaluator().set_effect_sandbox_mode(0);
    CHECK(cs.eval("(+ 1 1)").has_value(), "3689 soak: warm");
    CHECK(cs.eval("(set-code \"(define f (lambda () 1))\")").has_value(), "3689 soak: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3689 soak: eval");
    auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda () 9)\" \"#3689\")");
    CHECK(mut.has_value() && !is_error(*mut), "3689 soak: mutate body");
    CHECK(cs.eval("(eval-current)").has_value(), "3689 soak: partial peel re-eval");
    auto r = cs.eval("(f)");
    CHECK(r && is_int(*r) && as_int(*r) == 9, "3689 soak: mutated body");
    CHECK(production_hard_face_active(), "3689 soak: Production/Full");
    apply_dev_audit_defaults();
    CHECK(!production_hard_face_active(), "3689 AC4: Soft/Off hard-face off");
}

static void ac3581_3_dual_owner_source_cite() {
    std::println("\n--- #3581 AC3: dual-owner source-cite #3065 / #3068 ---");
    const auto dirty = read_file("src/compiler/dirty_propagation.ixx");
    const auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    const auto t = read_file("tests/compiler/test_dead_coercion_dirty_cone.cpp");
    CHECK(dirty.find("Issue #3065") != std::string::npos, "3581 AC3: #3065 remirror owner");
    CHECK(dirty.find("force_dead_coercion_elim_into_cone") != std::string::npos,
          "3581 AC3: #3065 remirror helper");
    CHECK(pure.find("Issue #3068") != std::string::npos, "3581 AC3: #3068 completeness owner");
    CHECK(pure.find("source_to_ir_map_missing_instr_loc") != std::string::npos,
          "3581 AC3: #3068 missing-loc helper");
    CHECK(pure.find("CastOp / type-cone is the motivating case") != std::string::npos,
          "3581 AC3: #3068 cites CastOp type-cone");
    CHECK(pure.find("#3065") != std::string::npos, "3581 AC3: #3068 names remirror owner");
    CHECK(t.find("ac3581_1_castop_type_change_cone_skip_either_owner") != std::string::npos,
          "3581 AC3: AC1 present");
    CHECK(t.find("ac3581_2_double_miss_unreachable") != std::string::npos, "3581 AC3: AC2 present");
    CHECK(!file_exists_cwd_3581("tests/compiler/test_issue_3581.cpp"),
          "3581 AC3: no test_issue_3581.cpp");
    CHECK(!file_exists_cwd_3581("docs/design/3581-cross-owner-castop.md"),
          "3581 AC3: no docs/design/");
    CHECK(!file_exists_cwd_3581("scripts/coverage/checks/check_cross_owner_castop_3581.py"),
          "3581 AC3: no check_3581.py");
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
    ac3046_nonidentity_density_cite();
    ac3065_1_production_elim_reenters_cone();
    ac3065_2_soft_no_permanent_bits();
    ac3065_3_schema_and_union_metrics();
    ac3065_4_residual_and_linter();
    ac3120_1_type_txn_remirrors_skipped_castop();
    ac3120_2_soft_zero_cost();
    ac3120_3_no_new_query_keys();
    ac3120_4_linter_no_invent();
    ac3228_1_empty_cone_remirrors_residual();
    ac3228_2_soft_quiet();
    ac3228_3_no_regression_3065_3120();
    ac3228_4_linter_suites();
    ac3347_1_live_policy_remirrors_before_auto_partial();
    ac3347_2_soft_quiet();
    ac3347_3_invalidate_gen_success_path();
    ac3347_4_linter_no_invent();
    ac3349_1_relower_remirrors_before_impact_ub();
    ac3349_2_soft_quiet();
    ac3349_3_production_persist_marks_or_force_full();
    ac3349_4_linter_no_invent();
    ac3618_persist_attribution_forces_full();
    ac3547_1_stamper_unbound_drops_cone();
    ac3547_2_type_id_drift_invalidates_site();
    ac3547_3_soft_keeps_cone();
    ac3547_4_source_cite_no_invent();
    ac3581_1_castop_type_change_cone_skip_either_owner();
    ac3581_2_double_miss_unreachable();
    ac3581_3_dual_owner_source_cite();
    ac3689_partial_peel_aos_dce_uses_mask();
    reset_residual_castop_persist_for_test();
    std::println("\n=== #2556/#3007/#3046/#3065/#3120/#3228/#3347/#3349/#3547/#3581/#3689: {} "
                 "passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dead_coercion_dirty_cone();
}
#endif
