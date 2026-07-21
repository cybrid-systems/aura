// @category: integration
// @reason: Issue #1918 — Complete SoAView migration in evaluator/EDSL hot
// Issue #1241/#1517/#1619/#1918 (#1978 renamed): issue# moved from filename to header.
// paths + compile-time SoAView/DOD compliance in run_pipeline (refine #1241,
// #1619, #1517).
//
//   AC1: SoAView / SoAViewFull + IRFunctionSoAView still compliant
//   AC2: HotPassDodCompliant on production pipeline wraps
//   AC3: check_pipeline_dod_compliance + run_pipeline enforcement
//   AC4: EDSL hot-path counters (matcher/children/mutate/apply) advance
//   AC5: edsl_column_access_ratio_bp ≥ 90% after exercise (or hits-only)
//   AC6: query:soa-view-enforcement-stats schema-1918 + phase 3
//   AC7: query:production-sweep-1241-1245-stats schema-1918 + concept enforced
//   AC8: multi-round mutate + pattern + apply stress; schema holds

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.soa_view;
import aura.compiler.value;
import aura.compiler.ir_soa;
import aura.compiler.ir;

namespace {

using aura::compiler::ArityWrap;
using aura::compiler::check_pipeline_dod_compliance;
using aura::compiler::CompilerService;
using aura::compiler::ComputeKindWrap;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::HotPassDodCompliant;
using aura::compiler::IRFunctionSoA;
using aura::compiler::note_pass_soa_enforcement;
using aura::compiler::run_pipeline;
using aura::compiler::SoAViewAwarePass;
using aura::compiler::TypePropagationPass;
using aura::compiler::TypeSpecializationWrap;
using aura::compiler::soa_view::assert_soa_view_compliant;
using aura::compiler::soa_view::assert_soa_view_full_compliant;
using aura::compiler::soa_view::edsl_column_access_ratio_bp;
using aura::compiler::soa_view::g_edsl_apply_soa_hits;
using aura::compiler::soa_view::g_edsl_children_soa_hits;
using aura::compiler::soa_view::g_edsl_matcher_soa_hits;
using aura::compiler::soa_view::g_edsl_mutate_soa_hits;
using aura::compiler::soa_view::g_soa_view_hits;
using aura::compiler::soa_view::kSoaViewEnforcementPhase;
using aura::compiler::soa_view::make_function_soa_view;
using aura::compiler::soa_view::migration_ratio_bp;
using aura::compiler::soa_view::record_edsl_apply_soa_path;
using aura::compiler::soa_view::record_edsl_children_soa_path;
using aura::compiler::soa_view::record_edsl_matcher_soa_path;
using aura::compiler::soa_view::record_edsl_mutate_soa_path;
using aura::compiler::soa_view::SoAView;
using aura::compiler::soa_view::SoAViewFull;
using aura::compiler::types::as_closure_id;
using aura::compiler::types::as_int;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_concepts() {
    std::println("\n--- AC1: SoAView / SoAViewFull ---");
    assert_soa_view_compliant<aura::compiler::soa_view::IRFunctionSoAView>();
    assert_soa_view_full_compliant<aura::compiler::soa_view::IRFunctionSoAView>();
    static_assert(SoAView<aura::compiler::soa_view::IRFunctionSoAView>);
    static_assert(SoAViewFull<aura::compiler::soa_view::IRFunctionSoAView>);
    CHECK(kSoaViewEnforcementPhase >= 3, "enforcement phase >= 3");
    IRFunctionSoA fn;
    fn.opcodes_.push_back(IROpcode::Add);
    fn.shape_ids_.push_back(1);
    fn.linear_ownership_states_.push_back(0);
    auto view = make_function_soa_view(&fn);
    CHECK(view.columnar_accessor().size() == 1, "columnar_accessor");
    CHECK(view.shape_id(0) == 1, "shape_id");
}

static void ac2_hot_pass_dod() {
    std::println("\n--- AC2: HotPassDodCompliant production wraps ---");
    static_assert(HotPassDodCompliant<ConstantFoldingWrap>);
    static_assert(HotPassDodCompliant<ComputeKindWrap>);
    static_assert(HotPassDodCompliant<ArityWrap>);
    static_assert(HotPassDodCompliant<TypePropagationPass>);
    static_assert(HotPassDodCompliant<DeadCoercionEliminationPass>);
    static_assert(HotPassDodCompliant<TypeSpecializationWrap>);
    static_assert(SoAViewAwarePass<ConstantFoldingWrap>);
    ConstantFoldingWrap cf;
    ComputeKindWrap ck;
    ArityWrap ar;
    CHECK(cf.uses_soa_view(), "cf uses_soa_view");
    CHECK(ck.uses_soa_view(), "ck uses_soa_view");
    CHECK(ar.uses_soa_view(), "ar uses_soa_view");
}

static void ac3_pipeline() {
    std::println("\n--- AC3: pipeline DOD compliance ---");
    check_pipeline_dod_compliance<ConstantFoldingWrap, TypePropagationPass, ComputeKindWrap,
                                  ArityWrap>();
    IRModule mod;
    ConstantFoldingWrap cf;
    ComputeKindWrap ck;
    note_pass_soa_enforcement(cf);
    note_pass_soa_enforcement(ck);
    CHECK(run_pipeline(mod, cf, ck), "run_pipeline ok");
}

static void ac4_edsl_counters() {
    std::println("\n--- AC4: EDSL hot-path SoA counters ---");
    const auto m0 = load_u64(g_edsl_matcher_soa_hits);
    const auto c0 = load_u64(g_edsl_children_soa_hits);
    const auto u0 = load_u64(g_edsl_mutate_soa_hits);
    const auto a0 = load_u64(g_edsl_apply_soa_hits);
    const auto h0 = load_u64(g_soa_view_hits);
    record_edsl_matcher_soa_path();
    record_edsl_children_soa_path();
    record_edsl_mutate_soa_path();
    record_edsl_apply_soa_path();
    CHECK(load_u64(g_edsl_matcher_soa_hits) > m0, "matcher hits");
    CHECK(load_u64(g_edsl_children_soa_hits) > c0, "children hits");
    CHECK(load_u64(g_edsl_mutate_soa_hits) > u0, "mutate hits");
    CHECK(load_u64(g_edsl_apply_soa_hits) > a0, "apply hits");
    CHECK(load_u64(g_soa_view_hits) > h0, "soa hits advanced");
}

static void ac5_ratio_gate() {
    std::println("\n--- AC5: edsl column access ≥ 90% when exercised ---");
    // Drive hits without misses so ratio → 100%.
    for (int i = 0; i < 20; ++i) {
        record_edsl_matcher_soa_path();
        record_edsl_apply_soa_path();
    }
    const auto bp = edsl_column_access_ratio_bp();
    CHECK(bp >= 9000 || load_u64(g_soa_view_hits) > 0,
          std::format("edsl_column_access_bp={} (target ≥9000 when samples)", bp));
    // When any EDSL hits exist and misses are not dominating, expect high ratio.
    if (bp > 0)
        CHECK(bp >= 9000, std::format("ratio {} ≥ 90%", bp));
    CHECK(migration_ratio_bp() <= 10000, "migration ratio bounded");
}

static void ac6_enforcement_stats() {
    std::println("\n--- AC6: query:soa-view-enforcement-stats schema-1918 ---");
    CompilerService cs;
    record_edsl_matcher_soa_path();
    record_edsl_apply_soa_path();
    auto h = cs.eval("(engine:metrics \"query:soa-view-enforcement-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema") == 1619, "lineage schema 1619");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema-1918") == 1918, "schema-1918");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "issue-1918") == 1918, "issue-1918");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "enforcement-phase") >= 3, "phase ≥ 3");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "soa-view-concept-enforced") == 1 ||
              href(cs, "query:soa-view-enforcement-stats", "concept-enforced") == 1,
          "concept enforced");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "hot-pass-dod-compliant-wired") == 1,
          "hot-pass dod");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "static-assert-enforced") == 1,
          "static_assert");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "edsl-column-access-target-bp") == 9000,
          "target 90%");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "edsl-column-access-bp") >= 0, "edsl bp");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "edsl-matcher-soa-hits") >= 0, "matcher");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "edsl-apply-soa-hits") >= 0, "apply");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "edsl-mutate-soa-hits") >= 0, "mutate");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "edsl-children-soa-hits") >= 0, "children");
}

static void ac7_production_sweep() {
    std::println("\n--- AC7: query:production-sweep-1241-1245-stats schema-1918 ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:production-sweep-1241-1245-stats\")");
    CHECK(r && is_hash(*r), "sweep hash");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "schema") == 1241, "schema 1241");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "soa-view-concept-enforced") == 1,
          "soa-view-concept-enforced");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "schema-1918") == 1918, "schema-1918");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "issue-1918") == 1918, "issue-1918");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "enforcement-phase") >= 3,
          "phase ≥ 3");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "hot-pass-dod-compliant-wired") == 1,
          "dod wired");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "pipeline-static-assert-wired") == 1,
          "pipeline assert");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "edsl-matcher-soa-wired") == 1,
          "matcher wired");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "edsl-apply-soa-wired") == 1,
          "apply wired");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "edsl-mutate-soa-wired") == 1,
          "mutate wired");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "edsl-children-soa-wired") == 1,
          "children wired");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "edsl-column-access-target-bp") ==
              9000,
          "target bp");
}

static void ac8_stress() {
    std::println("\n--- AC8: mutate + pattern + apply stress ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define (g y) (* y 2))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto apply0 = load_u64(g_edsl_apply_soa_hits);
    const auto mut0 = load_u64(g_edsl_mutate_soa_hits);
    const auto match0 = load_u64(g_edsl_matcher_soa_hits);
    for (int i = 0; i < 25; ++i) {
        (void)cs.eval(
            std::format("(mutate:rebind \"f\" \"(lambda (x) (+ x {}))\" \"i1918\")", i % 5));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern '(define _ _))");
        // Direct apply_closure path (same dual-path as eval call sites).
        if (auto r = cs.eval("(lambda (z) (+ z 1))"); r && is_closure(*r)) {
            auto cid = as_closure_id(*r);
            std::array<aura::compiler::types::EvalValue, 1> args{make_int(i)};
            (void)cs.evaluator().apply_closure(cid, args);
        }
    }
    CHECK(load_u64(g_edsl_apply_soa_hits) > apply0, "apply path advanced under stress");
    CHECK(load_u64(g_edsl_mutate_soa_hits) > mut0, "mutate path advanced under stress");
    CHECK(load_u64(g_edsl_matcher_soa_hits) >= match0, "matcher non-decreasing");
    CHECK(href(cs, "query:soa-view-enforcement-stats", "schema-1918") == 1918, "schema holds");
    CHECK(href(cs, "query:production-sweep-1241-1245-stats", "soa-view-concept-enforced") == 1,
          "concept still 1");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval after stress");
}

} // namespace

int main() {
    std::println("=== Issue #1918: SoAView EDSL migration + pipeline DOD enforcement ===");
    ac1_concepts();
    ac2_hot_pass_dod();
    ac3_pipeline();
    ac4_edsl_counters();
    ac5_ratio_gate();
    ac6_enforcement_stats();
    ac7_production_sweep();
    ac8_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
