// @category: unit
// @reason: pure C++ CoercionMap + IR DCE; no CompilerService
//
// test_issue_1425.cpp — Issue #1425: DeadCoercionEliminationPass
// wire-up + AST-level identity elision at CoercionMap apply.
//
// Background: #1418 wired IR DeadCoercionEliminationPass into
// run_pipeline packs (eval_ir / exec_jit / hot-swap). This issue
// is the twin: elide identity coercions when applying the
// deferred CoercionMap (before IR lowering), so dead CoercionNodes
// never enter the tree. Complements IR DCE (defense in depth).
//
// ACs:
//   AC1: CoercionMap with 5 entries (2 identity + 3 necessary)
//        → apply elides 2, inserts 3 CoercionNodes
//   AC2: mark_eliminated / eliminated_count on CoercionMap
//   AC3: IR DCE still elides 2 of 5 CastOps (regression #1418)
//   AC4: run_pipeline includes DeadCoercionEliminationPass (Pass)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.core.ast;
import aura.core.arena;
import aura.compiler.coercion_map;
import aura.compiler.pass_manager;
import aura.compiler.ir;

namespace test_issue_1425_detail {

using aura::ast::NodeTag;
using aura::compiler::apply_coercion_map;
using aura::compiler::CoercionMap;
using aura::compiler::DeadCoercionAstStats;
using aura::compiler::DeadCoercionEliminationPass;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

static int count_coercion_nodes(const aura::ast::FlatAST& flat) {
    int n = 0;
    for (aura::ast::NodeId i = 0; i < flat.size(); ++i) {
        if (flat.get(i).tag == NodeTag::Coercion)
            ++n;
    }
    return n;
}

// ── AC1: AST identity elision ───────────────────────────────────
//
// Build a Call with 5 literal args. Map 5 coercions onto the args:
//   slots 1,2: identity (child type_id == target type_id=1)
//   slots 3,4,5: necessary (child type_id=0, target=1)
// apply_coercion_map → eliminated=2, applied=3.

bool test_ac1_ast_identity_elision() {
    std::println("\n--- AC1: AST CoercionMap identity elision (2 of 5) ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto f_sym = pool->intern("f");
    auto f_var = flat->add_variable(f_sym);
    aura::ast::NodeId args[5];
    for (int i = 0; i < 5; ++i)
        args[i] = flat->add_literal(i + 1);
    // Stamp identity types on first two args (type_id=1).
    flat->set_type(args[0], 1);
    flat->set_type(args[1], 1);
    // args[2..4] leave type_id=0 (unknown) → necessary coercions.

    auto call = flat->add_call(f_var, args);
    flat->root = call;

    CoercionMap cm;
    // CoercionEntry targets child slots of Call: child 0 is callee,
    // children 1..5 are args. add_call layout: [callee, arg0, ...]
    for (int i = 0; i < 5; ++i) {
        cm.add(call, static_cast<std::uint32_t>(i + 1), args[i],
               /*type_tag=*/0, /*type_id=*/1, /*line=*/0, /*col=*/0);
    }
    CHECK(cm.size() == 5, "AC1.setup: 5 coercion entries");

    DeadCoercionAstStats stats;
    const auto applied = apply_coercion_map(*flat, cm, &stats, &cm);
    const auto coercion_nodes = count_coercion_nodes(*flat);

    std::println("  applied={} eliminated={} kept={} coercion_nodes={} map.elim={}", applied,
                 stats.eliminated, stats.kept, coercion_nodes, cm.eliminated_count());

    CHECK(stats.eliminated == 2, "AC1: 2 identity coercions eliminated");
    CHECK(applied == 3, "AC1: 3 CoercionNodes applied");
    CHECK(stats.kept == 3, "AC1: kept == 3");
    CHECK(coercion_nodes == 3, "AC1: FlatAST has exactly 3 Coercion nodes");
    CHECK(cm.eliminated_count() == 2, "AC1: CoercionMap.mark_eliminated count == 2");
    return true;
}

// ── AC2: defaults — no elision when type_ids don't match ────────

bool test_ac2_no_false_elision() {
    std::println("\n--- AC2: non-identity coercions are kept ---");
    auto arena = std::make_unique<aura::ast::ASTArena>();
    auto alloc = arena->allocator();
    auto* flat = arena->create<aura::ast::FlatAST>(alloc);
    auto* pool = arena->create<aura::ast::StringPool>(alloc);

    auto f_sym = pool->intern("g");
    auto f_var = flat->add_variable(f_sym);
    auto a0 = flat->add_literal(1);
    auto a1 = flat->add_literal(2);
    // Different type_ids: child has 2, target is 1 → not identity.
    flat->set_type(a0, 2);
    flat->set_type(a1, 2);
    auto call = flat->add_call(f_var, std::array{a0, a1});
    flat->root = call;

    CoercionMap cm;
    cm.add(call, 1, a0, 0, 1, 0, 0);
    cm.add(call, 2, a1, 0, 1, 0, 0);

    DeadCoercionAstStats stats;
    apply_coercion_map(*flat, cm, &stats, &cm);
    CHECK(stats.eliminated == 0, "AC2: no false identity elision");
    CHECK(stats.applied == 2, "AC2: both non-identity applied");
    CHECK(count_coercion_nodes(*flat) == 2, "AC2: 2 Coercion nodes present");
    return true;
}

// ── AC3: IR DCE 2 of 5 (regression of #1418 AC) ─────────────────

bool test_ac3_ir_dce_two_of_five() {
    std::println("\n--- AC3: IR DeadCoercionEliminationPass 2 of 5 ---");
    IRModule mod;
    IRFunction func;
    func.id = 0;
    func.name = "dce1425";
    func.entry_block = 0;
    func.local_count = 16;
    aura::ir::BasicBlock blk;
    blk.id = 0;
    auto& ins = blk.instructions;

    // 2 identity CastOps
    ins.push_back(IRInstruction{IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::ConstI64, {2, 9, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::CastOp, {3, 2, 0, 0}, 0, 1});
    // 3 necessary CastOps
    ins.push_back(IRInstruction{IROpcode::ConstI64, {4, 1, 0, 0}, 0, 0});
    ins.push_back(IRInstruction{IROpcode::CastOp, {5, 4, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::ConstI64, {6, 2, 0, 0}, 0, 0});
    ins.push_back(IRInstruction{IROpcode::CastOp, {7, 6, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::ConstI64, {8, 3, 0, 0}, 0, 0});
    ins.push_back(IRInstruction{IROpcode::CastOp, {9, 8, 0, 0}, 0, 1});
    ins.push_back(IRInstruction{IROpcode::Return, {5, 0, 0, 0}, 0, 0});

    func.blocks.push_back(std::move(blk));
    mod.functions.push_back(std::move(func));

    DeadCoercionEliminationPass dce;
    dce.run(mod);
    int remaining = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++remaining;

    std::println("  eliminated={} remaining_castops={}", dce.eliminated_count(), remaining);
    CHECK(dce.eliminated_count() == 2, "AC3: IR DCE eliminates 2");
    CHECK(remaining == 3, "AC3: 3 CastOps remain");
    return true;
}

// ── AC4: run_pipeline invokes DCE ───────────────────────────────

bool test_ac4_run_pipeline_dce() {
    std::println("\n--- AC4: run_pipeline includes DeadCoercionEliminationPass ---");
    IRModule mod;
    IRFunction func;
    func.name = "pipe";
    func.local_count = 4;
    aura::ir::BasicBlock blk;
    blk.id = 0;
    blk.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1},
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    func.blocks.push_back(std::move(blk));
    mod.functions.push_back(std::move(func));

    DeadCoercionEliminationPass dce;
    const bool ok = aura::compiler::run_pipeline(mod, dce);
    CHECK(ok, "AC4: run_pipeline ok");
    CHECK(dce.eliminated_count() == 1, "AC4: DCE inside run_pipeline elided identity CastOp");
    CHECK(mod.functions[0].blocks[0].instructions[1].opcode == IROpcode::Local,
          "AC4: CastOp → Local after pipeline");
    return true;
}

} // namespace test_issue_1425_detail

int aura_issue_1425_run() {
    using namespace test_issue_1425_detail;
    std::println("=== Issue #1425: DeadCoercion AST elision + IR pipeline ===");
    bool all_ok = true;
    all_ok &= test_ac1_ast_identity_elision();
    all_ok &= test_ac2_no_false_elision();
    all_ok &= test_ac3_ir_dce_two_of_five();
    all_ok &= test_ac4_run_pipeline_dce();
    if (all_ok && g_failed == 0) {
        std::println("\n=== ALL ACs PASS ===");
        return 0;
    }
    std::println("\n=== Some ACs FAILED (g_failed={}) ===", g_failed);
    return 1;
}

int main() {
    return aura_issue_1425_run();
}
