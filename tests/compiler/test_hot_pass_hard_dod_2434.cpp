// @category: unit
// @reason: Issue #2434 — Harden PureWrapPass + HotPassDodCompliant for all
//          production pipeline stages; eliminate soft unmarked Legacy skips.
//
//   AC1: All production pack stages HotPassDodCompliant (or explicit Legacy)
//   AC2: Production pack note_pass_soa_enforcement → concept_rejection delta 0
//   AC3: Pure Wrap stages still advance pure_wrap_total
//   AC4: static_assert pack holds; dirty short-circuit intact
//   AC5: schema-2434 + source-cite + inventory

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.ir;

namespace {

using aura::compiler::ArityWrap;
using aura::compiler::check_pipeline_dod_compliance;
using aura::compiler::CompilerService;
using aura::compiler::ComputeKindWrap;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DCEPass;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::DefineDirtyMaskView;
using aura::compiler::HotPassDodCompliant;
using aura::compiler::InlinePass;
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

int main() {
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

    std::println("\n=== #2434 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
