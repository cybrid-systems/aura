// test_production_sweep.cpp — Merged #1251–#1255 + #1336–#1348 (#1978).
//
// Originally test_production_sweep_1251_1255.cpp +
// test_production_sweep_1336_1348.cpp. Both are Phase 1 production
// sweeps covering distinct production-readiness matrix cells (mark-dirty
// bounds + 200-stride incremental + ADT exhaust + lockfree StableRef +
// auto-compact etc.). Merged with all ACs preserved verbatim.
//
// AC list (all preserved; each AC section cites the original issue#):
//   Issue #1251–#1255 (test_production_sweep_1251_1255.cpp):
//     AC1: query:production-sweep-1251-1255-stats schema + active flags
//     AC2: mark_dirty_upward bounds constants + truncation counter
//     AC3: mutate under Guard bumps boundary wrap + hold samples
//   Issue #1336–#1348 (test_production_sweep_1336_1348.cpp):
//     AC1: query:production-sweep-1336-1348-stats schema + active flags
//     AC2: incremental-tc / solve-delta-worklist / ir-parent-type-stamp
//     AC3: SV pattern presets + DeadCoercionElimination parent-type stamp
//     AC4: FlatAST dirty prune + lockfree ref + soft compact
//     AC5: eval still works after sweep (smoke)

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.ir;
import aura.compiler.pass_manager;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── #1251–#1255 AC1: schema + active flags ──
static void ac_1251_schema() {
    std::println("\n--- #1251–#1255 AC1: query:production-sweep-1251-1255-stats ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:production-sweep-1251-1255-stats\")");
    CHECK(r && is_hash(*r), "sweep stats is hash");
    CHECK(href(cs, "query:production-sweep-1251-1255-stats", "schema") == 1251, "schema");
    CHECK(href(cs, "query:production-sweep-1251-1255-stats", "active") == 1, "active");
    CHECK(href(cs, "query:production-sweep-1251-1255-stats", "mark-dirty-bounds-enforced") == 1,
          "mark_dirty bounds (#1251)");
    CHECK(href(cs, "query:production-sweep-1251-1255-stats", "rollback-compaction-path") == 1,
          "rollback compaction (#1251)");
    CHECK(href(cs, "query:production-sweep-1251-1255-stats", "steal-inner-boundary-hardened") == 1,
          "steal inner boundary (#1254)");
    CHECK(href(cs, "query:production-sweep-1251-1255-stats", "pattern-hygiene-strict-enforced") ==
              1,
          "pattern hygiene (#1255)");
    CHECK(href(cs, "query:production-sweep-1251-1255-stats", "issue-1255") == 1255, "issue-1255");
}

// ── #1251–#1255 AC2: mark_dirty_upward bounds + truncation ──
static void ac_1251_bounds() {
    std::println("\n--- #1251–#1255 AC2: mark_dirty_upward bounds + truncation ---");
    aura::ast::FlatAST flat;
    CHECK(aura::ast::FlatAST::kMarkDirtyMaxDepth == 64, "max depth 64");
    CHECK(aura::ast::FlatAST::kMarkDirtyCountThreshold == 4096, "count threshold 4096");
    auto a = flat.add_literal(1);
    auto b = flat.add_literal(2);
    flat.mark_dirty_upward(a);
    CHECK(flat.mark_dirty_truncated_count() == 0, "no truncation on short chain");
    (void)b;
    CHECK(flat.rollback_compaction_triggered() == 0, "no rollback compaction yet");
}

// ── #1252/#1253: mutate under Guard bumps boundary + hold ──
static void ac_1252_mutate() {
    std::println("\n--- #1252/#1253: mutate under Guard bumps boundary wrap + hold samples ---");
    CompilerService cs;
    auto set = cs.eval("(set-code \"(define x 1)\")");
    (void)set;
    auto ev = cs.eval("(eval-current)");
    (void)ev;
    auto m = cs.eval("(mutate:rebind \"x\" \"2\")");
    (void)m;
    auto wrapped =
        href(cs, "query:production-sweep-1251-1255-stats", "mutation-boundary-primitives-wrapped");
    CHECK(wrapped >= 0, "boundary wrap counter readable");
    auto samples = href(cs, "query:production-sweep-1251-1255-stats", "mutation-hold-samples");
    CHECK(samples >= 0, "hold samples readable");
}

// ── #1336–#1348 AC1: schema + active flags ──
static void ac_1336_schema() {
    std::println("\n--- #1336–#1348 AC1: query:production-sweep-1336-1348-stats ---");
    CompilerService cs;
    constexpr auto Q = "query:production-sweep-1336-1348-stats";
    auto r = cs.eval(aura::test::aura_call_expr(Q));
    CHECK(r && is_hash(*r), "sweep stats is hash");
    CHECK(href(cs, Q, "schema") == 1336, "schema");
    CHECK(href(cs, Q, "active") == 1, "active");
    CHECK(href(cs, Q, "incremental-tc-selective-active") == 1, "incremental TC (#1336)");
    CHECK(href(cs, Q, "solve-delta-worklist-soft-cap") == 256, "worklist soft cap");
    CHECK(href(cs, Q, "ir-parent-type-stamp-active") == 1, "parent type stamp (#1338)");
    CHECK(href(cs, Q, "linear-move-elide-active") == 1, "linear move elide (#1339)");
    CHECK(href(cs, Q, "adt-exhaust-incremental-active") == 1, "ADT exhaust (#1340)");
    CHECK(href(cs, Q, "blame-elision-reason-obs-active") == 1, "blame obs (#1341)");
    CHECK(href(cs, Q, "sv-highlevel-mutate-active") == 1, "SV highlevel mutate (#1344)");
    CHECK(href(cs, Q, "query-sv-pattern-preset-active") == 1, "SV pattern preset");
    CHECK(href(cs, Q, "dirty-upward-prune-active") == 1, "dirty prune (#1345)");
    CHECK(href(cs, Q, "dirty-upward-max-depth-config") == 64, "max depth config");
    CHECK(href(cs, Q, "stable-ref-lockfree-path-active") == 1, "lockfree StableRef (#1346)");
    CHECK(href(cs, Q, "sv-feedback-harness-active") == 1, "SV harness (#1347)");
    CHECK(href(cs, Q, "ast-auto-compact-active") == 1, "auto compact (#1348)");
    CHECK(href(cs, Q, "ast-compaction-threshold") == 1024, "compaction threshold");
    CHECK(href(cs, Q, "issue-1348") == 1348, "issue-1348");
}

// ── #1347: verify:parse-* harness bumps counters ──
static void ac_1347_parse() {
    std::println("\n--- #1347: verify:parse-* harness ---");
    CompilerService cs;
    constexpr auto Q = "query:production-sweep-1336-1348-stats";
    auto c = cs.eval("(verify:parse-coverage-feedback \"0 hit_rate=0.45\")");
    CHECK(c && is_int(*c), "parse-coverage-feedback returns int count");
    auto a = cs.eval("(verify:parse-assert-failure \"0 assert_p_ready\")");
    CHECK(a && is_int(*a), "parse-assert-failure returns int count");
    CHECK(href(cs, Q, "verify-parse-coverage-total") >= 1, "coverage parse counted");
    CHECK(href(cs, Q, "verify-parse-assert-total") >= 1, "assert parse counted");
}

// ── #1344: SV pattern presets callable ──
static void ac_1344_sv() {
    std::println("\n--- #1344: SV pattern presets ---");
    CompilerService cs;
    auto n = cs.eval("(query:sv-interface)");
    CHECK(n && is_int(*n) && as_int(*n) >= 0, "query:sv-interface");
    auto p = cs.eval("(query:sv-property)");
    CHECK(p && is_int(*p) && as_int(*p) >= 0, "query:sv-property");
}

// ── #1338/#1341: DeadCoercionElimination parent-type stamp ──
static void ac_1338_dce() {
    std::println("\n--- #1338/#1341: DeadCoercionElimination parent-type stamp ---");
    IRModule mod;
    IRFunction f;
    f.name = "dce_test";
    f.arg_count = 1;
    f.local_count = 3;
    f.blocks.resize(1);
    f.blocks[0].id = 0;
    f.blocks[0].instructions.push_back(IRInstruction{
        .opcode = IROpcode::Local,
        .operands = {0, 0, 0, 0},
        .type_id = 7,
    });
    f.blocks[0].instructions.push_back(IRInstruction{
        .opcode = IROpcode::CastOp,
        .operands = {1, 0, 7, 0},
        .type_id = 7,
        .narrow_evidence = 0x1,
    });
    f.blocks[0].instructions.push_back(IRInstruction{
        .opcode = IROpcode::Return,
        .operands = {1, 0, 0, 0},
    });
    mod.functions.push_back(std::move(f));

    aura::compiler::DeadCoercionEliminationPass dce;
    dce.run(mod);
    CHECK(dce.eliminated_count() >= 1, "DCE eliminated identity/narrow cast");
    CHECK(dce.narrow_evidence_hits() + dce.type_prop_hits() >= 1, "elision reason counted");
}

// ── #1345/#1346/#1348: FlatAST dirty prune + lockfree + soft compact ──
static void ac_1345_flatast() {
    std::println("\n--- #1345/#1346/#1348: FlatAST dirty prune + lockfree + soft compact ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    aura::ast::FlatAST flat(alloc);
    const auto root = flat.add_node(aura::ast::NodeTag::Define);
    flat.root = root;
    if (root < flat.size()) {
        flat.mark_dirty_upward_fast(root, aura::ast::FlatAST::kGeneralDirty, 0, /*max_depth=*/8,
                                    /*stop_at_boundary=*/true);
        CHECK(flat.mark_dirty_boundary_prune_count() >= 1, "boundary prune on Define");
        auto ref = flat.make_ref(root);
        auto v = ref.validate_or_refresh(flat);
        CHECK(v.has_value(), "validate_or_refresh ok");
        CHECK(flat.lockfree_stable_ref_validate_count() >= 1, "lockfree validate counted");
    }
    flat.set_compaction_free_list_threshold(1);
    flat.begin_atomic_batch();
    flat.commit_atomic_batch();
    CHECK(flat.auto_compact_on_commit_count() >= 0, "auto compact counter readable");
}

// ── Smoke: eval still works after sweep ──
static void smoke_eval() {
    std::println("\n--- smoke: eval still works ---");
    CompilerService cs;
    auto a = cs.eval("(+ 20 22)");
    CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
}

} // namespace

int main() {
    std::println("=== Merged production sweep: #1251–#1255 + #1336–#1348 ===");
    // #1251–#1255 ACs
    ac_1251_schema();
    ac_1251_bounds();
    ac_1252_mutate();
    // #1336–#1348 ACs
    ac_1336_schema();
    ac_1347_parse();
    ac_1344_sv();
    ac_1338_dce();
    ac_1345_flatast();
    smoke_eval();
    std::println("\n=== Results: {} passed, {} failed ===", ::aura::test::g_passed,
                 ::aura::test::g_failed);
    return ::aura::test::g_failed ? 1 : 0;
}