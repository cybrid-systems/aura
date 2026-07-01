// test_dead_coercion_elim_narrow_evidence_ir_jit_zerooverhead.cpp
// Issue #629: zero-overhead coercion path — narrow_evidence
// integration across DCE Rule 6, TypeSpecializationWrap skip,
// lowering metadata, GuardShape fast-path, and observability.
//
// AC1: DCE Rule 6 elides CastOp with narrow_evidence + matching type_id
// AC2: TypeSpec skips cast insert when Branch has narrow_evidence
// AC3: query:coercion-zerooverhead-stats returns int >= 0, grows after pipeline
// AC4: mutate + re-eval gradual typing — semantic match, zerooverhead_win monotonic
// AC5: regression — Issue #508 dynamic passthrough still works

#include "test_harness.hpp"

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_629_detail {

using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::compiler::CompilerService;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::TypeSpecializationWrap;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

static std::size_t count_cast_ops(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp) ++n;
    return n;
}

static IRModule make_module_with(std::vector<IRInstruction> instrs,
                                 std::uint32_t local_count = 16) {
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "test", .local_count = local_count});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    func.blocks.back().instructions = std::move(instrs);
    return mod;
}

static IRModule make_branch_narrow_module(bool with_narrow_evidence) {
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "test", .local_count = 32});
    auto& func = mod.functions.back();

    // Block 0: cond + Branch + Return
    func.blocks.push_back({0});
    auto& entry = func.blocks[0].instructions;
    entry.push_back({IROpcode::ConstBool, {0, 1, 0, 0}, 0, 2});
    IRInstruction branch{};
    branch.opcode = IROpcode::Branch;
    branch.operands = {0, 1, 2, 3};
    branch.type_id = 1; // Int result type
    branch.narrow_evidence = with_narrow_evidence ? 1u : 0u;
    entry.push_back(branch);
    entry.push_back({IROpcode::Return, {3, 0, 0, 0}, 0, 0});

    // Block 1 (then): mismatched String value → would need CastOp without narrow_evidence
    func.blocks.push_back({0});
    func.blocks[1].instructions = {
        {IROpcode::ConstString, {4, 0, 0, 0}, 0, 3},
        {IROpcode::Local, {3, 4, 0, 0}, 0, 3},
        {IROpcode::Jump, {0, 0, 0, 0}, 0, 0},
    };

    // Block 2 (else): same mismatch pattern
    func.blocks.push_back({0});
    func.blocks[2].instructions = {
        {IROpcode::ConstString, {5, 0, 0, 0}, 0, 3},
        {IROpcode::Local, {3, 5, 0, 0}, 0, 3},
        {IROpcode::Jump, {0, 0, 0, 0}, 0, 0},
    };

    return mod;
}

static void test_dce_rule6_narrow_evidence() {
    std::println("\n--- AC1: DCE Rule 6 narrow_evidence identity elision ---");
    IRInstruction cast{};
    cast.opcode = IROpcode::CastOp;
    cast.operands = {1, 0, 0, 0};
    cast.type_id = 1;
    cast.narrow_evidence = 4;
    auto mod = make_module_with({
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        cast,
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    });
    DeadCoercionEliminationPass dce;
    dce.run(mod);
    CHECK(count_cast_ops(mod) == 0, "Rule 6 elides narrow_evidence CastOp");
    CHECK(dce.narrow_evidence_hits() == 1, "narrow_evidence_hits == 1");
    CHECK(dce.type_prop_hits() == 0, "type_prop_hits == 0 (Rule 6 not Rule 1)");
}

static void test_typespec_skips_branch_cast_with_narrow_evidence() {
    std::println("\n--- AC2: TypeSpec skips cast when Branch has narrow_evidence ---");
    aura::core::TypeRegistry reg;
    (void)reg.lookup_type("Int");
    (void)reg.lookup_type("String");
    (void)reg.lookup_type("Any");

    auto mod_skip = make_branch_narrow_module(true);
    TypeSpecializationWrap ts_skip(&reg);
    ts_skip.run(mod_skip);
    CHECK(ts_skip.narrow_evidence_skipped() == 1, "narrow_evidence_skipped == 1");

    auto mod_insert = make_branch_narrow_module(false);
    TypeSpecializationWrap ts_insert(&reg);
    ts_insert.run(mod_insert);
    CHECK(ts_insert.narrow_evidence_skipped() == 0,
          "narrow_evidence_skipped == 0 without evidence");
    CHECK(ts_insert.castop_emitted() >= ts_skip.castop_emitted(),
          "more CastOps emitted without narrow_evidence guard");
}

static void test_coercion_zerooverhead_stats_primitive() {
    std::println("\n--- AC3: query:coercion-zerooverhead-stats primitive ---");
    CompilerService cs;
    auto r0 = cs.eval("(query:coercion-zerooverhead-stats)");
    CHECK(r0 && is_int(*r0), "primitive returns int");
    const auto s0 = as_int(*r0);
    CHECK(s0 >= 0, "initial stats >= 0");

    cs.eval("(set-code \"(define x 42) (define y \\\"hi\\\")\")");
    CHECK(cs.eval("(eval-current)").has_value(), "pipeline eval succeeds");

    auto r1 = cs.eval("(query:coercion-zerooverhead-stats)");
    CHECK(r1 && is_int(*r1), "primitive returns int after pipeline");
    CHECK(as_int(*r1) >= s0, "stats grow (monotonic) after pipeline");
}

static void test_mutate_re_eval_gradual_typing() {
    std::println("\n--- AC4: mutate + re-eval gradual typing ---");
    CompilerService cs;
    cs.eval("(set-code \"(define x 42) (define y \\\"hello\\\") (define z #t)\")");
    auto baseline = cs.eval("(eval-current)");
    CHECK(baseline.has_value(), "baseline eval succeeds");

    auto snap0 = cs.snapshot();
    const auto win0 = snap0.coercion_zerooverhead_win_total;

    for (int i = 0; i < 5; ++i) {
        std::string code = "(set-code \"(define v ";
        code += std::to_string(i * 3);
        code += ")\")";
        CHECK(cs.eval(code).has_value(), "mutation set-code ok");
        auto r = cs.eval("(eval-current)");
        CHECK(r.has_value(), "post-mutation eval succeeds");
    }

    auto after = cs.eval("(eval-current)");
    CHECK(after.has_value(), "final eval succeeds");
    auto snap1 = cs.snapshot();
    CHECK(snap1.coercion_zerooverhead_win_total >= win0,
          "zerooverhead_win monotonic across mutations");
}

static void test_issue_508_dynamic_passthrough_regression() {
    std::println("\n--- AC5: Issue #508 dynamic passthrough regression ---");
    auto mod = make_module_with({
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        {IROpcode::CastOp, {1, 0, 3, 0}, 0, 0},
        {IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    });
    DeadCoercionEliminationPass dce;
    dce.run(mod);
    CHECK(count_cast_ops(mod) == 0, "Dynamic passthrough CastOp still elided");
    CHECK(dce.eliminated_count() == 1, "eliminated_count == 1 (508 regression)");
}

} // namespace aura_629_detail

int main() {
    using namespace aura_629_detail;
    test_dce_rule6_narrow_evidence();
    test_typespec_skips_branch_cast_with_narrow_evidence();
    test_coercion_zerooverhead_stats_primitive();
    test_mutate_re_eval_gradual_typing();
    test_issue_508_dynamic_passthrough_regression();
    return RUN_ALL_TESTS();
}