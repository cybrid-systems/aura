// @category: integration
// @reason: Issue #606 Pass/AnalysisPass concept + fold pipeline + Pure wrap adoption
//
// Scope-limited close matching the #601 / #491 / #479 / #604 pattern:
// ship the const-correctness + pure Wrap foundation now; the full
// concept-constrained visitor refactor + hot-path Contracts adoption
// in evaluator_impl.cpp / lowering_impl.cpp is a separate follow-up.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import std;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.compiler.value;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_606_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t stat_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:pass-pipeline-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_606_detail

int aura_issue_606_run() {
    using namespace aura_issue_606_detail;
    std::println("=== Issue #606: Pass concept + pure Wrap adoption ===");

    using aura::ir::IRFunction;
    using aura::ir::IRInstruction;
    using aura::ir::IRModule;
    using aura::ir::IROpcode;

    // The static_asserts in pass_manager.ixx that require
    //   PureAnalysisPass<ComputeKindWrap>
    //   PureAnalysisPass<ArityWrap>
    //   IncrementalPass<ConstantFoldingWrap>
    // to be satisfied would have failed at compile time if the
    // const-correctness / alias work was missing. We add a runtime
    // AC that actually instantiates one of each to make the
    // property observable through test output (not just at compile).
    static_assert(aura::compiler::PureAnalysisPass<aura::compiler::ComputeKindWrap>,
                  "ComputeKindWrap is PureAnalysisPass");
    static_assert(aura::compiler::PureAnalysisPass<aura::compiler::ArityWrap>,
                  "ArityWrap is PureAnalysisPass");
    // Note: ConstantFoldingWrap has run(IRFunction&) + run(BasicBlock&)
    // overloads that fulfill IncrementalPass. We exercise them in AC3 below.

    // AC1: Pure wrap delegation — ShapeWrap/LinearOwnershipWrap exist
    // and route through pipeline fold without observable error.
    {
        std::println("\n--- AC1: ShapeWrap + LinearOwnershipWrap delegation ---");
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "linear_test", .local_count = 4});
        auto& func = mod.functions.back();
        func.blocks.push_back({0});
        // MoveOp(0,1) consumes slot 1; subsequent read of slot 1 is
        // a use-after-move that LinearOwnershipWrap detects.
        func.blocks[0].instructions = {
            {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
            {IROpcode::MoveOp, {2, 1, 0, 0}, 0, 1},
            {IROpcode::Return, {2, 0, 0, 0}, 0, 0}, // not a read of slot 1; clean
        };
        const auto shape_before = aura::compiler::ShapeWrap::pure_delegation_hits();
        const auto linear_before = aura::compiler::LinearOwnershipWrap::pure_delegation_hits();
        aura::compiler::ShapeWrap sw;
        aura::compiler::LinearOwnershipWrap low;
        CHECK(aura::compiler::run_analysis_pipeline(mod, sw, low),
              "fold over ShapeWrap + LinearOwnershipWrap succeeds");
        CHECK(low.has_error() == false,
              "LinearOwnershipWrap sees no use-after-move in clean program");
        CHECK(aura::compiler::ShapeWrap::pure_delegation_hits() == shape_before + 1,
              "ShapeWrap::pure_delegation_hits bumped once");
        CHECK(aura::compiler::LinearOwnershipWrap::pure_delegation_hits() == linear_before + 1,
              "LinearOwnershipWrap::pure_delegation_hits bumped once");
    }

    // AC2: PureAnalysisPass const-correctness — invoking run() via a
    // const reference must compile + work (the load-bearing property).
    {
        std::println("\n--- AC2: const run() works on ComputeKindWrap ---");
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "ck_test", .local_count = 2});
        mod.functions.back().blocks.push_back({0});
        mod.functions.back().blocks[0].instructions = {
            {IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1},
            {IROpcode::Return, {0, 0, 0, 0}, 0, 0},
        };
        const aura::compiler::ComputeKindWrap ck;
        // Const-ref call proves PureAnalysisPass satisfaction at runtime.
        const auto& ck_cref = ck;
        ck_cref.run(mod);
        CHECK(true, "const-ref run() compiles + executes");
    }

    // AC3: IncrementalPass — ConstantFoldingWrap exposes run_function +
    // run_block aliases. Build a module that exercises the alias
    // path separately from run(IRModule&).
    {
        std::println("\n--- AC3: ConstantFoldingWrap incremental aliases ---");
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "fold_test", .local_count = 4});
        auto& func = mod.functions.back();
        func.blocks.push_back({0});
        func.blocks[0].instructions = {
            {IROpcode::ConstI64, {0, 2, 0, 0}, 0, 1},
            {IROpcode::ConstI64, {1, 3, 0, 0}, 0, 1},
            {IROpcode::Add, {2, 0, 1, 0}, 0, 1},
            {IROpcode::Return, {2, 0, 0, 0}, 0, 0},
        };
        aura::compiler::ConstantFoldingWrap cf;
        // run(IRFunction&) alias — should fold the Add(2,3) → ConstI64(5).
        cf.run(mod.functions[0]);
        const auto folded = cf.folded_count();
        CHECK(folded >= 1, std::format("run(IRFunction&) alias folded >= 1 (got {})", folded));
        // Second call on the same block exercises the per-block alias.
        cf.run(mod.functions[0].blocks[0]);
        CHECK(cf.folded_count() >= folded, "run(BasicBlock&) alias does not decrease folded_count");
    }

    // AC4: EDSL observable — (engine:metrics \"query:pass-pipeline-stats\") exposes
    // pure-delegation-{shape,linear,total} counters.
    {
        std::println(
            "\n--- AC4: (engine:metrics \"query:pass-pipeline-stats\") pure-delegation fields ---");
        aura::compiler::CompilerService cs;
        auto stats = cs.eval("(engine:metrics \"query:pass-pipeline-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:pass-pipeline-stats returns a hash");
        const auto shape_pure = stat_int(cs, "pure-delegation-shape");
        const auto linear_pure = stat_int(cs, "pure-delegation-linear");
        const auto total_pure = stat_int(cs, "pure-delegation-total");
        CHECK(shape_pure >= 0, "pure-delegation-shape present + >= 0");
        CHECK(linear_pure >= 0, "pure-delegation-linear present + >= 0");
        CHECK(total_pure >= 0, "pure-delegation-total present + >= 0");
        CHECK(total_pure == shape_pure + linear_pure,
              std::format("total == shape + linear ({} == {} + {})", total_pure, shape_pure,
                          linear_pure));
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_606_run();
}
#endif
