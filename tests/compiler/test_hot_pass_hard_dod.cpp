// @category: unit
// @reason: Issue #2434 — Harden PureWrapPass + HotPassDodCompliant for all
//          production pipeline stages; eliminate soft unmarked Legacy skips.
//          Issue #3042 — drop residual std::function dirty predicates from
//          PureWrap production stages (column-view / fn-pointer preds).
//          Issue #3234 — Tarjan compute_sccs local recursive struct (zero
//          type-erasure on the pass surface).
//
//   AC1: All production pack stages HotPassDodCompliant (or explicit Legacy)
//   AC2: Production pack note_pass_soa_enforcement → concept_rejection delta 0
//   AC3: Pure Wrap stages still advance pure_wrap_total
//   AC4: static_assert pack holds; dirty short-circuit intact
//   AC5: schema-2434 + source-cite + inventory
//   #3042 AC1–AC5: no std::function dirty members/setters; inlineable preds;
//                  dod + short-circuit + schema-3042 + concept_rejection==0
//   #3234 AC1–AC4: grep-clean pass_impls; InlinePass still runs; same Tarjan
//                  body; source-cite / no invent

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <type_traits>
#include <vector>

import std;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;

namespace {

using aura::compiler::ArityWrap;
using aura::compiler::BlockDirtyPred;
using aura::compiler::check_pipeline_dod_compliance;
using aura::compiler::CompilerService;
using aura::compiler::ComputeKindWrap;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DCEPass;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::DefineDirtyMaskView;
using aura::compiler::HotPassDodCompliant;
using aura::compiler::InlinePass;
using aura::compiler::InstructionDirtyPred;
using aura::compiler::kPassSccNoStdFunctionIssue;
using aura::compiler::kPureWrapNoStdFunctionDirtyIssue;
using aura::compiler::LinearOwnershipPass;
using aura::compiler::MonomorphizePass;
using aura::compiler::note_pass_soa_enforcement;
using aura::compiler::PureWrapPass;
using aura::compiler::run_incremental_dirty_pipeline;
using aura::compiler::run_pipeline;
using aura::compiler::ShapeWrap;
using aura::compiler::SoAViewAwarePass;
using aura::compiler::TCOPass;
using aura::compiler::TypePropagationPass;
using aura::compiler::TypeSpecializationWrap;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static IRModule make_mod(std::size_t n_blocks) {
    IRModule mod;
    aura::ir::IRFunction fn;
    fn.name = "f2434";
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

} // namespace

int run_test_hot_pass_hard_dod() {
    std::println("=== Issue #2434: hard HotPassDodCompliant for all production stages ===");

    // ── AC1: pack stages HotPass + PureWrap ────────────────────────
    {
        std::println("\n--- #2434 AC1: production stages HotPassDodCompliant ---");
        static_assert(HotPassDodCompliant<TypeSpecializationWrap>);
        static_assert(HotPassDodCompliant<TypePropagationPass>);
        static_assert(HotPassDodCompliant<ComputeKindWrap>);
        static_assert(HotPassDodCompliant<ArityWrap>);
        static_assert(HotPassDodCompliant<ConstantFoldingWrap>);
        static_assert(HotPassDodCompliant<DeadCoercionEliminationPass>);
        static_assert(HotPassDodCompliant<ShapeWrap>);
        static_assert(HotPassDodCompliant<DCEPass>);
        static_assert(HotPassDodCompliant<LinearOwnershipPass>);
        static_assert(HotPassDodCompliant<InlinePass>);
        static_assert(HotPassDodCompliant<TCOPass>);
        static_assert(HotPassDodCompliant<MonomorphizePass>);
        static_assert(PureWrapPass<DCEPass>);
        static_assert(PureWrapPass<InlinePass>);
        static_assert(PureWrapPass<TCOPass>);
        static_assert(PureWrapPass<MonomorphizePass>);
        static_assert(PureWrapPass<LinearOwnershipPass>);
        check_pipeline_dod_compliance<TypeSpecializationWrap, TypePropagationPass, ComputeKindWrap,
                                      ArityWrap, ConstantFoldingWrap,
                                      DeadCoercionEliminationPass>();
        check_pipeline_dod_compliance<DCEPass, LinearOwnershipPass, InlinePass, TCOPass,
                                      MonomorphizePass>();
        CHECK(true, "AC1: production packs accepted at registration");
        CHECK(DCEPass::kPureWrap && InlinePass::kPureWrap && TCOPass::kPureWrap, "AC1: kPureWrap");
        // #2524: inventory + static_assert live in pass_pipeline_core (facade re-exports).
        auto pm = read_file("src/compiler/pass_manager.ixx") +
                  read_file("src/compiler/pass_pipeline_core.ixx") +
                  read_file("src/compiler/pass_impls.ixx");
        CHECK(pm.find("check_production_pipeline_packs_2434") != std::string::npos,
              "AC1: production pack inventory");
        CHECK(pm.find("Pipeline stage must be HotPassDodCompliant") != std::string::npos,
              "AC1: hard static_assert for all stages");
    }

    // ── AC2: concept_rejection stays 0 for production pack note ────
    {
        std::println("\n--- #2434 AC2: concept_rejection delta 0 on production pack ---");
        const auto rej0 =
            aura::compiler::pass_pipeline_concept_rejection_total.load(std::memory_order_relaxed);
        const auto pure0 =
            aura::compiler::pass_pipeline_pure_wrap_total.load(std::memory_order_relaxed);
        TypeSpecializationWrap ts;
        TypePropagationPass tp;
        ComputeKindWrap ck;
        ArityWrap ar;
        ConstantFoldingWrap cf;
        DeadCoercionEliminationPass dce;
        note_pass_soa_enforcement(ts);
        note_pass_soa_enforcement(tp);
        note_pass_soa_enforcement(ck);
        note_pass_soa_enforcement(ar);
        note_pass_soa_enforcement(cf);
        note_pass_soa_enforcement(dce);
        DCEPass dce_classic;
        InlinePass inl;
        TCOPass tco;
        MonomorphizePass mono;
        LinearOwnershipPass lo;
        note_pass_soa_enforcement(dce_classic);
        note_pass_soa_enforcement(inl);
        note_pass_soa_enforcement(tco);
        note_pass_soa_enforcement(mono);
        note_pass_soa_enforcement(lo);
        const auto rej1 =
            aura::compiler::pass_pipeline_concept_rejection_total.load(std::memory_order_relaxed);
        const auto pure1 =
            aura::compiler::pass_pipeline_pure_wrap_total.load(std::memory_order_relaxed);
        CHECK(rej1 == rej0, "AC2: concept_rejection delta 0 under production stages");
        CHECK(pure1 > pure0, "AC2/AC3: pure_wrap advanced for PureWrap stages");
    }

    // ── AC3: pure wrap still recorded via run_pipeline ─────────────
    {
        std::println("\n--- #2434 AC3: run_pipeline pure_wrap + no rejection ---");
        auto mod = make_mod(2);
        ConstantFoldingWrap cf;
        ComputeKindWrap ck;
        DCEPass dce;
        const auto pure0 =
            aura::compiler::pass_pipeline_pure_wrap_total.load(std::memory_order_relaxed);
        const auto rej0 =
            aura::compiler::pass_pipeline_concept_rejection_total.load(std::memory_order_relaxed);
        CHECK(run_pipeline(mod, cf, ck, dce), "AC3: run_pipeline fold ok");
        CHECK(aura::compiler::pass_pipeline_pure_wrap_total.load(std::memory_order_relaxed) > pure0,
              "AC3: pure_wrap total advanced");
        CHECK(aura::compiler::pass_pipeline_concept_rejection_total.load(
                  std::memory_order_relaxed) == rej0,
              "AC3: no concept rejection on pure pack");
    }

    // ── AC4: dirty short-circuit still works ───────────────────────
    {
        std::println("\n--- #2434 AC4: dirty short-circuit intact ---");
        auto mod = make_mod(6);
        std::vector<std::vector<std::uint8_t>> clean(1, std::vector<std::uint8_t>(6, 0));
        DefineDirtyMaskView clean_view;
        clean_view.block_dirty_per_func = &clean;
        const auto sc0 =
            aura::compiler::pipeline_dirty_short_circuit_total.load(std::memory_order_relaxed);
        ConstantFoldingWrap cf;
        CHECK(run_incremental_dirty_pipeline(mod, cf, &clean_view), "AC4: clean mask skip");
        CHECK(aura::compiler::pipeline_dirty_short_circuit_total.load(std::memory_order_relaxed) >
                  sc0,
              "AC4: short-circuit advanced");
        check_pipeline_dod_compliance<ConstantFoldingWrap, TypePropagationPass, ShapeWrap>();
        CHECK(true, "AC4: static_assert pack holds");
    }

    // ── AC5: schema-2434 + source-cite ─────────────────────────────
    {
        std::println("\n--- #2434 AC5: schema + source-cite ---");
        // #2524: hard-dod metric + issue cite live in pass_pipeline_core.
        auto pm = read_file("src/compiler/pass_manager.ixx") +
                  read_file("src/compiler/pass_pipeline_core.ixx") +
                  read_file("src/compiler/pass_impls.ixx");
        auto cc = read_file("src/core/concept_constraints.ixx");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(pm.find("Issue #2434") != std::string::npos, "AC5: #2434 in pass_manager");
        CHECK(cc.find("Issue #2434") != std::string::npos, "AC5: #2434 in concept_constraints");
        CHECK(q.find("schema-2434") != std::string::npos, "AC5: schema-2434 query key");
        CHECK(pm.find("pass_pipeline_hard_dod_wired") != std::string::npos,
              "AC5: hard_dod_wired metric");

        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        CHECK(href(cs, "query:soa-view-enforcement-stats", "schema-2434") == 2434,
              "AC5: schema-2434 runtime");
        CHECK(href(cs, "query:soa-view-enforcement-stats", "pass-pipeline-hard-dod-wired") == 1,
              "AC5: hard-dod wired");
        CHECK(href(cs, "query:soa-view-enforcement-stats",
                   "pass-pipeline-production-pack-inventory-wired") == 1,
              "AC5: pack inventory wired");
        // After real eval pipeline, rejection should not explode (may be 0).
        const auto rej =
            href(cs, "query:soa-view-enforcement-stats", "pass-pipeline-concept-rejection-total");
        CHECK(rej >= 0, "AC5: rejection total readable");
    }

    // ── #3042: residual std::function dirty predicates gone ────────
    {
        std::println("\n--- #3042 AC1/AC2: no std::function dirty predicates ---");
        CHECK(kPureWrapNoStdFunctionDirtyIssue == 3042, "3042 AC1: issue constant");
        static_assert(std::is_trivially_copyable_v<BlockDirtyPred>);
        static_assert(std::is_trivially_copyable_v<InstructionDirtyPred>);
        CHECK(true, "3042 AC2: BlockDirtyPred / InstructionDirtyPred trivially copyable");
        auto pm = read_file("src/compiler/pass_manager.ixx") +
                  read_file("src/compiler/pass_pipeline_core.ixx") +
                  read_file("src/compiler/pass_impls.ixx") +
                  read_file("src/compiler/optimization_passes.ixx") +
                  read_file("src/compiler/service.ixx");
        CHECK(pm.find("set_block_dirty_fn(std::function") == std::string::npos,
              "3042 AC1: no std::function block dirty setter");
        CHECK(pm.find("set_instruction_dirty_fn(std::function") == std::string::npos,
              "3042 AC1: no std::function instruction dirty setter");
        CHECK(pm.find("std::function<bool(std::uint32_t)> block_dirty") == std::string::npos,
              "3042 AC1: no std::function block_dirty member");
        CHECK(pm.find("struct BlockDirtyPred") != std::string::npos, "3042 AC2: BlockDirtyPred");
        CHECK(pm.find("struct InstructionDirtyPred") != std::string::npos,
              "3042 AC2: InstructionDirtyPred");

        std::println("\n--- #3042 AC3/AC5: dod + schema + rejection ---");
        check_pipeline_dod_compliance<ConstantFoldingWrap, TypePropagationPass, ComputeKindWrap,
                                      ShapeWrap, DeadCoercionEliminationPass>();
        CHECK(true, "3042 AC3: check_pipeline_dod_compliance still green");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(q.find("schema-3042") != std::string::npos, "3042 AC5: schema-3042 query key");
        const auto rej0 =
            aura::compiler::pass_pipeline_concept_rejection_total.load(std::memory_order_relaxed);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "3042 set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3042 eval");
        CHECK(href(cs, "query:soa-view-enforcement-stats", "schema-3042") == 3042,
              "3042 AC5: schema-3042 runtime");
        CHECK(href(cs, "query:soa-view-enforcement-stats",
                   "pure-wrap-no-std-function-dirty-wired") == 1,
              "3042 AC5: no-std-function dirty wired");
        CHECK(aura::compiler::pass_pipeline_concept_rejection_total.load(
                  std::memory_order_relaxed) == rej0,
              "3042 AC5: pass_pipeline_concept_rejection_total delta 0");
        auto mod = make_mod(4);
        ConstantFoldingWrap cf_prod;
        ComputeKindWrap ck_prod;
        DCEPass dce_prod;
        const auto rej1 =
            aura::compiler::pass_pipeline_concept_rejection_total.load(std::memory_order_relaxed);
        CHECK(run_pipeline(mod, cf_prod, ck_prod, dce_prod), "3042 AC5: production pack");
        CHECK(aura::compiler::pass_pipeline_concept_rejection_total.load(
                  std::memory_order_relaxed) == rej1,
              "3042 AC5: concept_rejection == 0 under production pack");

        std::println("\n--- #3042 AC4: dirty short-circuit + test setter ---");
        ConstantFoldingWrap cf;
        cf.set_block_dirty_fn([](std::uint32_t) { return false; });
        CHECK(!cf.is_block_dirty(0), "3042 AC4: test-only fn pointer setter (all clean)");
        cf.set_block_dirty_fn([](std::uint32_t bi) { return bi == 0; });
        CHECK(cf.is_block_dirty(0) && !cf.is_block_dirty(1), "3042 AC4: block-0 dirty only");
        auto mod6 = make_mod(6);
        std::vector<std::vector<std::uint8_t>> clean(1, std::vector<std::uint8_t>(6, 0));
        DefineDirtyMaskView clean_view;
        clean_view.block_dirty_per_func = &clean;
        const auto sc0 =
            aura::compiler::pipeline_dirty_short_circuit_total.load(std::memory_order_relaxed);
        ConstantFoldingWrap cf2;
        CHECK(run_incremental_dirty_pipeline(mod6, cf2, &clean_view), "3042 AC4: clean mask skip");
        CHECK(aura::compiler::pipeline_dirty_short_circuit_total.load(std::memory_order_relaxed) >
                  sc0,
              "3042 AC4: short-circuit advanced");
    }

    // ── #3234: Tarjan compute_sccs — no type-erased callable ──────
    {
        std::println("\n--- #3234 AC1: pass_impls has zero std::function ---");
        CHECK(kPassSccNoStdFunctionIssue == 3234, "3234 AC1: issue constant");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        CHECK(!impls.empty(), "3234 AC1: read pass_impls.ixx");
        CHECK(impls.find("std::function") == std::string::npos, "3234 AC1: no std::function");
        CHECK(impls.find("#include <functional>") == std::string::npos,
              "3234 AC1: no <functional>");
        CHECK(impls.find("struct StrongConnect") != std::string::npos, "3234 AC1: StrongConnect");
        CHECK(impls.find("(*this)(w)") != std::string::npos, "3234 AC1: recurse via operator()");

        std::println("\n--- #3234 AC2: InlinePass / SCC-dependent run still green ---");
        auto mod = make_mod(2);
        InlinePass inliner;
        inliner.run(mod);
        CHECK(mod.functions.size() == 1, "3234 AC2: module intact");
        CHECK(impls.find("compute_sccs") != std::string::npos, "3234 AC2: compute_sccs");
        CHECK(impls.find("scc_id_of_fid_") != std::string::npos, "3234 AC2: scc map");

        std::println("\n--- #3234 AC3: same Tarjan body / reverse-topo ids ---");
        CHECK(impls.find("lowlink") != std::string::npos, "3234 AC3: lowlink");
        CHECK(impls.find("on_stack") != std::string::npos, "3234 AC3: on_stack");
        CHECK(impls.find("numbered in reverse topological order") != std::string::npos,
              "3234 AC3: reverse-topo");
        InlinePass inliner2;
        inliner2.run(mod);
        CHECK(mod.functions.size() == 1, "3234 AC3: second run still green");

        std::println("\n--- #3234 AC4: source-cite; no invent ---");
        const auto build = read_file("build.py");
        CHECK(impls.find("Issue #3234") != std::string::npos, "3234 AC4: pass_impls cite");
        CHECK(build.find("check_pass_scc_no_std_function_3234") != std::string::npos,
              "3234 AC4: build.py");
        CHECK(read_file("docs/design/3234-pass-scc.md").empty(), "3234 AC4: no docs/design");
        CHECK(read_file("tests/compiler/test_issue_3234.cpp").empty(), "3234 AC4: no invent");
    }

    std::println("\n=== #2434/#3042/#3234 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hot_pass_hard_dod();
}
#endif
