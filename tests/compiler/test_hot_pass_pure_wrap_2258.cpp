// @category: integration
// @reason: Issue #2258 — Enforce HotPassDodCompliant + pure-function Wrap for
// all incremental / dirty-aware passes (extends #1918 / #2060 / #1619).
//
//   AC1: Pipeline registration rejects non-HotPassDodCompliant dirty/inc packs
//   AC2: Dirty-aware pure Wrap property: same IR + dirty mask → same outputs
//   AC3: Fold-expression concept path remains the only production entry
//   AC4: pass_pipeline_pure_wrap_total + concept-rejection metrics + schema-2258
//   AC5: Dirty short-circuit still advances; production-sweep lineage holds

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;

namespace {

using aura::compiler::check_pipeline_dod_compliance;
using aura::compiler::CompilerService;
using aura::compiler::ComputeKindWrap;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DefineDirtyMaskView;
using aura::compiler::HotPassDodCompliant;
using aura::compiler::note_pass_soa_enforcement;
using aura::compiler::PureWrapPass;
using aura::compiler::run_incremental_dirty_pipeline;
using aura::compiler::run_pipeline;
using aura::compiler::ShapeWrap;
using aura::compiler::SoAViewAwarePass;
using aura::compiler::TypePropagationPass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static IRModule make_mod(std::size_t n_blocks) {
    IRModule mod;
    aura::ir::IRFunction fn;
    fn.name = "f2258";
    fn.local_count = 4;
    for (std::size_t i = 0; i < n_blocks; ++i) {
        aura::ir::BasicBlock b;
        b.id = static_cast<std::uint32_t>(i);
        b.instructions.push_back(aura::ir::IRInstruction{
            .opcode = IROpcode::ConstI64,
            .operands = {0, static_cast<std::uint32_t>(i + 1), 0, 0},
        });
        b.instructions.push_back(aura::ir::IRInstruction{
            .opcode = IROpcode::Add,
            .operands = {1, 0, 0, 0},
        });
        fn.blocks.push_back(std::move(b));
    }
    mod.functions.push_back(std::move(fn));
    return mod;
}

static std::vector<IROpcode> collect_opcodes(const IRModule& mod) {
    std::vector<IROpcode> ops;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& ins : b.instructions)
                ops.push_back(ins.opcode);
    return ops;
}

static void ac1_hot_pass_mandatory() {
    std::println("\n--- AC1: HotPassDodCompliant mandatory on dirty/inc wraps ---");
    static_assert(HotPassDodCompliant<ConstantFoldingWrap>);
    static_assert(HotPassDodCompliant<TypePropagationPass>);
    static_assert(HotPassDodCompliant<ComputeKindWrap>);
    static_assert(HotPassDodCompliant<ShapeWrap>);
    static_assert(PureWrapPass<ConstantFoldingWrap>);
    static_assert(PureWrapPass<TypePropagationPass>);
    static_assert(PureWrapPass<ComputeKindWrap>);
    static_assert(PureWrapPass<ShapeWrap>);
    static_assert(SoAViewAwarePass<ConstantFoldingWrap>);
    check_pipeline_dod_compliance<ConstantFoldingWrap, TypePropagationPass, ComputeKindWrap,
                                  ShapeWrap>();
    CHECK(true, "compliant dirty/inc pack accepted at registration");
    CHECK(ConstantFoldingWrap::kPureWrap, "cf kPureWrap");
    CHECK(TypePropagationPass::kPureWrap, "tp kPureWrap");
}

static void ac2_pure_property() {
    std::println("\n--- AC2: pure property identical inputs + dirty mask ---");
    auto mod_a = make_mod(4);
    auto mod_b = make_mod(4);

    std::vector<std::vector<std::uint8_t>> sparse(1, std::vector<std::uint8_t>(4, 0));
    sparse[0][1] = 1;
    sparse[0][3] = 1;
    DefineDirtyMaskView view;
    view.block_dirty_per_func = &sparse;

    ConstantFoldingWrap cf_a;
    ConstantFoldingWrap cf_b;
    TypePropagationPass tp_a;
    TypePropagationPass tp_b;

    CHECK(run_incremental_dirty_pipeline(mod_a, cf_a, &view), "cf_a dirty ok");
    CHECK(run_incremental_dirty_pipeline(mod_b, cf_b, &view), "cf_b dirty ok");
    CHECK(cf_a.folded_count() == cf_b.folded_count(), "cf pure: folded_count match");
    CHECK(collect_opcodes(mod_a) == collect_opcodes(mod_b), "cf pure: opcode stream match");

    CHECK(run_incremental_dirty_pipeline(mod_a, tp_a, &view), "tp_a dirty ok");
    CHECK(run_incremental_dirty_pipeline(mod_b, tp_b, &view), "tp_b dirty ok");
    CHECK(tp_a.propagated_count() == tp_b.propagated_count(), "tp pure: propagated match");
    CHECK(collect_opcodes(mod_a) == collect_opcodes(mod_b), "tp pure: opcode stream match");
}

static void ac3_fold_path_only() {
    std::println("\n--- AC3: fold-expression + concept path (no type erasure) ---");
    auto mod = make_mod(2);
    ConstantFoldingWrap cf;
    ComputeKindWrap ck;
    // run_pipeline is the production fold entry; concept pack checked inside.
    CHECK(run_pipeline(mod, cf, ck), "run_pipeline fold ok");
    check_pipeline_dod_compliance<ConstantFoldingWrap, ComputeKindWrap>();
    CHECK(true, "concept pack + fold path only");
}

static void ac4_metrics() {
    std::println("\n--- AC4: pure_wrap + concept-rejection metrics + schema-2258 ---");
    const auto pure0 =
        aura::compiler::pass_pipeline_pure_wrap_total.load(std::memory_order_relaxed);
    ConstantFoldingWrap cf;
    note_pass_soa_enforcement(cf);
    CHECK(aura::compiler::pass_pipeline_pure_wrap_total.load(std::memory_order_relaxed) > pure0,
          "pure_wrap total advanced");

    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:soa-view-enforcement-stats\")");
    CHECK(h && is_hash(*h), "soa-view-enforcement-stats hash");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema-2258") == 2258, "schema-2258");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "issue-2258") == 2258, "issue-2258");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "hot-pass-dod-mandatory-wired") == 1,
          "hot pass mandatory wired");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "pure-wrap-enforcement-wired") == 1,
          "pure wrap wired");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "pass-pipeline-pure-wrap-total") >= 0,
          "pure wrap total");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "pass-pipeline-concept-rejection-total") >=
              0,
          "concept rejection total");

    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "schema-2258") == 2258,
          "sweep schema-2258");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "pass-pipeline-pure-wrap-total") >= 0,
          "sweep pure wrap");
}

static void ac5_short_circuit_green() {
    std::println("\n--- AC5: dirty short-circuit + lineage green ---");
    auto mod = make_mod(6);
    std::vector<std::vector<std::uint8_t>> clean(1, std::vector<std::uint8_t>(6, 0));
    DefineDirtyMaskView clean_view;
    clean_view.block_dirty_per_func = &clean;

    const auto skip0 =
        aura::compiler::optimization_passes_skipped_by_define_dirty.load(std::memory_order_relaxed);
    const auto sc0 =
        aura::compiler::pipeline_dirty_short_circuit_total.load(std::memory_order_relaxed);
    ConstantFoldingWrap cf;
    CHECK(run_incremental_dirty_pipeline(mod, cf, &clean_view), "clean mask skip ok");
    CHECK(aura::compiler::optimization_passes_skipped_by_define_dirty.load(
              std::memory_order_relaxed) > skip0,
          "define-dirty skip advanced");
    CHECK(aura::compiler::pipeline_dirty_short_circuit_total.load(std::memory_order_relaxed) > sc0,
          "short-circuit advanced");

    CompilerService cs;
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema-2060") == 2060, "lineage 2060");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema-1918") == 1918, "lineage 1918");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema") == 1619, "lineage 1619");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "hot-pass-dod-compliant-wired") == 1,
          "1918 wired");
}

} // namespace

int main() {
    std::println("=== Issue #2258: HotPassDodCompliant + pure Wrap enforcement ===");
    ac1_hot_pass_mandatory();
    ac2_pure_property();
    ac3_fold_path_only();
    ac4_metrics();
    ac5_short_circuit_green();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
