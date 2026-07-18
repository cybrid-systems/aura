// @category: integration
// @reason: Issue #1661 — Shape-aware + linear ownership collaborative
// Pass with GuardShape, EscapeAnalysis and ConstantFolding
// (refine #462 / #1531 / #606).
//
//   AC1: ShapeAwareFoldingPass linear elide via escape_map collab
//   AC2: narrow_evidence CastOp fold (not count-only)
//   AC3: GuardShape + specialized_for opportunity counts
//   AC4: query:shape-folding-stats schema 1661 + AC metric aliases
//   AC5: collab wire flags (escape / CF / linear / GuardShape / re-lower)
//   AC6: mutate + re-eval stress; metrics non-decreasing; no crash
//   AC7: #462 lineage fields still present

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::ShapeAwareFoldingPass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:shape-folding-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static void ac1_linear_elide_escape_collab() {
    std::println("\n--- AC1: linear elide + escape_map collab ---");
    aura::ir::IRModule mod;
    mod.entry_function_id = 0;
    aura::ir::IRFunction func;
    func.id = 0;
    func.name = "elide1661";
    func.local_count = 2;
    func.blocks.push_back({0});
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::ConstI64,
        .operands = {0u, 7u, 0u, 0u},
        .linear_ownership_state = 1, // Owned
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::MoveOp,
        .operands = {1u, 0u, 0u, 0u},
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Return,
        .operands = {0u, 0u, 0u, 0u},
    });
    // Simulate EscapeAnalysisPass: non-escaping source.
    func.escape_map = {0, 0};
    mod.functions.push_back(func);

    ShapeAwareFoldingPass saf;
    saf.run(mod);
    CHECK(saf.linear_elide_count() == 1, "linear elide via func.escape_map");
    CHECK(saf.fold_count() >= 1, "fold_count >= 1");
    CHECK(mod.functions[0].blocks[0].instructions[1].opcode == aura::ir::IROpcode::Nop,
          "MoveOp → Nop");
}

static void ac2_narrow_cast_fold() {
    std::println("\n--- AC2: narrow_evidence CastOp fold ---");
    aura::ir::IRModule mod;
    mod.entry_function_id = 0;
    aura::ir::IRFunction func;
    func.id = 0;
    func.name = "narrow1661";
    func.local_count = 2;
    func.blocks.push_back({0});
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Local,
        .operands = {0u, 0u, 0u, 0u},
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::CastOp,
        .operands = {1u, 0u, 0u, 0u},
        .narrow_evidence = 1,
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Return,
        .operands = {0u, 0u, 0u, 0u},
    });
    mod.functions.push_back(func);

    ShapeAwareFoldingPass saf;
    saf.run(mod);
    CHECK(saf.narrow_check_count() == 1, "narrow_check_count == 1");
    CHECK(saf.fold_count() == 1, "CastOp folded into fold_count");
    CHECK(mod.functions[0].blocks[0].instructions[1].opcode == aura::ir::IROpcode::Nop,
          "CastOp → Nop");
}

static void ac3_guardshape_specialized() {
    std::println("\n--- AC3: GuardShape + specialized_for ---");
    aura::ir::IRModule mod;
    mod.entry_function_id = 0;
    aura::ir::IRFunction func;
    func.id = 0;
    func.name = "spec1661";
    func.local_count = 1;
    func.specialized_for = 1; // shape-specialized
    func.blocks.push_back({0});
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::GuardShape,
        .operands = {0u, 0u, 1u, 0u},
        .shape_id = 1,
    });
    func.blocks[0].instructions.push_back(aura::ir::IRInstruction{
        .opcode = aura::ir::IROpcode::Return,
        .operands = {0u, 0u, 0u, 0u},
    });
    mod.functions.push_back(func);

    ShapeAwareFoldingPass saf;
    saf.run(mod);
    CHECK(saf.guard_shape_hits() == 1, "guard_shape_hits == 1");
    CHECK(saf.specialized_shape_fold_opportunities() >= 1, "specialized opportunities >= 1");
}

static void ac4_schema_1661() {
    std::println("\n--- AC4: schema 1661 + AC metric aliases ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:shape-folding-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1661, "schema 1661");
    CHECK(href(cs, "issue") == 1661, "issue 1661");
    CHECK(href(cs, "shape_aware_fold_hits") >= 0, "shape_aware_fold_hits");
    CHECK(href(cs, "linear_ownership_dce_savings") >= 0, "linear_ownership_dce_savings");
    CHECK(href(cs, "guardshape_inserted_count") >= 0, "guardshape_inserted_count");
    CHECK(href(cs, "specialized-shape-fold-opportunities") >= 0, "specialized opportunities");
    CHECK(href(cs, "shape-fold-count") >= 0, "shape-fold-count lineage");
}

static void ac5_wire_flags() {
    std::println("\n--- AC5: collab wire flags ---");
    CompilerService cs;
    CHECK(href(cs, "escape-analysis-collab-wired") == 1, "escape collab");
    CHECK(href(cs, "constant-fold-collab-wired") == 1, "CF collab");
    CHECK(href(cs, "linear-ownership-collab-wired") == 1, "linear collab");
    CHECK(href(cs, "guardshape-collab-wired") == 1, "GuardShape collab");
    CHECK(href(cs, "narrow-evidence-cast-fold-wired") == 1, "narrow CastOp fold");
    CHECK(href(cs, "mutation-relower-collab-wired") == 1, "mutation re-lower");
    CHECK(href(cs, "shape-linear-collaborative-mandate-active") == 1, "mandate");
}

static void ac6_mutate_stress() {
    std::println("\n--- AC6: mutate + re-eval stress ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (f 2)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto fold0 = m->shape_fold_count.load(std::memory_order_relaxed);
    const auto elide0 = m->shape_linear_elide_count.load(std::memory_order_relaxed);

    for (int i = 0; i < 80; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"#1661\")", i % 9));
        (void)cs.eval("(eval-current)");
        if ((i % 11) == 0)
            (void)cs.eval("(query:pattern '(define _ _))");
    }
    CHECK(href(cs, "schema") == 1661, "schema holds under stress");
    CHECK(m->shape_fold_count.load(std::memory_order_relaxed) >= fold0, "fold non-decreasing");
    CHECK(m->shape_linear_elide_count.load(std::memory_order_relaxed) >= elide0,
          "elide non-decreasing");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

static void ac7_lineage_462() {
    std::println("\n--- AC7: #462 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "shape-fold-count") >= 0, "shape-fold-count");
    CHECK(href(cs, "shape-linear-elide-count") >= 0, "shape-linear-elide-count");
    CHECK(href(cs, "shape-narrow-check-count") >= 0, "shape-narrow-check-count");
    CHECK(href(cs, "guard-shape-hits") >= 0, "guard-shape-hits");
}

} // namespace

int main() {
    std::println("=== Issue #1661: shape + linear collaborative pass ===");
    ac1_linear_elide_escape_collab();
    ac2_narrow_cast_fold();
    ac3_guardshape_specialized();
    ac4_schema_1661();
    ac5_wire_flags();
    ac6_mutate_stress();
    ac7_lineage_462();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
