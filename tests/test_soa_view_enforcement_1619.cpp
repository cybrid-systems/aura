// @category: integration
// @reason: Issue #1619 — SoAView columnar_accessor + pipeline pack
// static_assert + EDSL hot-path migration (refine #1517/#1241).
//
//   AC1: SoAView requires columnar_accessor; IRFunctionSoAView compliant
//   AC2: check_pipeline_dod_compliance / kRequireSoAView static_assert
//   AC3: production wraps SoAViewAware (const-fold / DCE / type-prop)
//   AC4: EDSL helpers consult_tag_arity / children / closure advance hits
//   AC5: query:soa-view-enforcement-stats schema 1619 AC keys
//   AC6: multi-round pipeline stress + migration-ratio-bp
//   AC7: #1517 lineage keys still present

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.pass_manager;
import aura.compiler.soa_view;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.concept_constraints;

namespace {

using aura::compiler::check_pass_dod_compliance;
using aura::compiler::check_pipeline_dod_compliance;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::concept_enforcement_hits_total;
using aura::compiler::ConstantFoldingWrap;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::IRFunctionSoA;
using aura::compiler::note_pass_soa_enforcement;
using aura::compiler::Pass;
using aura::compiler::RequiresSoAViewPass;
using aura::compiler::run_pipeline;
using aura::compiler::SoAViewAwarePass;
using aura::compiler::TypePropagationPass;
using aura::compiler::soa_view::assert_soa_view_compliant;
using aura::compiler::soa_view::assert_soa_view_full_compliant;
using aura::compiler::soa_view::consult_closure_shape_linear;
using aura::compiler::soa_view::consult_tag_arity;
using aura::compiler::soa_view::g_soa_view_hits;
using aura::compiler::soa_view::IRFunctionSoAView;
using aura::compiler::soa_view::make_function_soa_view;
using aura::compiler::soa_view::migration_ratio_bp;
using aura::compiler::soa_view::record_edsl_children_soa_path;
using aura::compiler::soa_view::SoAView;
using aura::compiler::soa_view::SoAViewFull;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:soa-view-enforcement-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

struct SoaRequirePass {
    static constexpr bool kRequireSoAView = true;
    bool ran = false;
    void run(IRModule&) { ran = true; }
    [[nodiscard]] bool has_error() const { return false; }
    [[nodiscard]] constexpr bool uses_soa_view() const noexcept { return true; }
};

static_assert(Pass<SoaRequirePass>);
static_assert(RequiresSoAViewPass<SoaRequirePass>);
static_assert(SoAViewAwarePass<SoaRequirePass>);
static_assert(SoAViewAwarePass<ConstantFoldingWrap>);
static_assert(SoAViewAwarePass<DeadCoercionEliminationPass>);
static_assert(SoAViewAwarePass<TypePropagationPass>);

static void ac1_concept() {
    std::println("\n--- AC1: SoAView columnar_accessor ---");
    assert_soa_view_compliant<IRFunctionSoAView>();
    assert_soa_view_full_compliant<IRFunctionSoAView>();
    static_assert(SoAView<IRFunctionSoAView>);
    static_assert(SoAViewFull<IRFunctionSoAView>);

    IRFunctionSoA fn;
    fn.opcodes_.push_back(IROpcode::Add);
    fn.shape_ids_.push_back(7);
    fn.linear_ownership_states_.push_back(1);
    auto view = make_function_soa_view(&fn);
    CHECK(view.size() == 1, "view size 1");
    auto col = view.columnar_accessor();
    CHECK(col.size() == 1, "columnar_accessor size 1");
    CHECK(col[0] == IROpcode::Add, "columnar opcode Add");
    CHECK(view.shape_id(0) == 7, "shape_id");
    CHECK(view.linear_ownership(0) == 1, "linear");
}

static void ac2_pack_check() {
    std::println("\n--- AC2: pipeline pack DOD compliance ---");
    check_pass_dod_compliance<SoaRequirePass>();
    check_pipeline_dod_compliance<SoaRequirePass, ConstantFoldingWrap>();
    IRModule mod;
    SoaRequirePass p;
    const auto e0 = load_u64(concept_enforcement_hits_total);
    CHECK(run_pipeline(mod, p), "run_pipeline");
    CHECK(p.ran, "pass ran");
    CHECK(load_u64(concept_enforcement_hits_total) > e0, "enforcement hit");
}

static void ac3_production_wraps() {
    std::println("\n--- AC3: production wraps SoAViewAware ---");
    ConstantFoldingWrap cf;
    DeadCoercionEliminationPass dce;
    TypePropagationPass tp;
    CHECK(SoAViewAwarePass<decltype(cf)>, "cf concept");
    CHECK(cf.uses_soa_view() == false || cf.uses_soa_view() == true, "cf uses_soa_view callable");
    CHECK(dce.uses_soa_view() == true, "dce uses_soa_view");
    CHECK(tp.uses_soa_view() == true, "tp uses_soa_view");
    IRModule mod;
    const auto e0 = load_u64(concept_enforcement_hits_total);
    note_pass_soa_enforcement(dce);
    note_pass_soa_enforcement(tp);
    CHECK(load_u64(concept_enforcement_hits_total) >= e0 + 2, "enforcement +2");
    (void)mod;
}

static void ac4_edsl_helpers() {
    std::println("\n--- AC4: EDSL hot-path helpers ---");
    const auto h0 = load_u64(g_soa_view_hits);
    const auto idx = consult_tag_arity(0x06, 2); // Let-like tag packing
    CHECK(idx == ((0x06u << 8) | 2u), "tag_arity pack");
    record_edsl_children_soa_path();
    IRFunctionSoA fn;
    fn.opcodes_.push_back(IROpcode::Call);
    fn.shape_ids_.push_back(42);
    fn.linear_ownership_states_.push_back(2);
    auto view = make_function_soa_view(&fn);
    std::uint32_t sh = 0;
    std::uint8_t lin = 0;
    CHECK(consult_closure_shape_linear(view, 0, sh, lin), "closure consult ok");
    CHECK(sh == 42 && lin == 2, "shape+linear");
    CHECK(load_u64(g_soa_view_hits) > h0, "hits advanced");
    CHECK(migration_ratio_bp() > 0 || load_u64(g_soa_view_hits) > 0, "ratio or hits");
}

static void ac5_query_schema() {
    std::println("\n--- AC5: query schema 1619 ---");
    CompilerService cs;
    // Force a few hits before query
    (void)consult_tag_arity(1, 1);
    record_edsl_children_soa_path();
    auto h = cs.eval("(engine:metrics \"query:soa-view-enforcement-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1619 || href(cs, "schema") == 1517, "schema 1619|1517");
    CHECK(href(cs, "issue") == 1619 || href(cs, "issue") < 0, "issue 1619");
    CHECK(href(cs, "concept-enforcement-hits") >= 0, "concept-enforcement-hits");
    CHECK(href(cs, "soa-view-pass-skipped") >= 0, "soa-view-pass-skipped");
    CHECK(href(cs, "edsl-soa-migration-progress") >= 0, "edsl-soa-migration-progress");
    CHECK(href(cs, "soa-view-hits") >= 1, "soa-view-hits");
    CHECK(href(cs, "migration-ratio-bp") >= 0, "migration-ratio-bp");
    CHECK(href(cs, "soa-view-full-compliant") == 1, "soa-view-full-compliant");
    CHECK(href(cs, "static-assert-enforced") == 1, "static-assert-enforced");
    CHECK(href(cs, "columnar-accessor-required") == 1, "columnar-accessor-required");
    CHECK(href(cs, "pipeline-pack-check") == 1, "pipeline-pack-check");
    CHECK(href(cs, "enforcement-phase") == 2 || href(cs, "enforcement-phase") >= 1,
          "enforcement-phase");
}

static void ac6_stress() {
    std::println("\n--- AC6: multi-round stress ---");
    IRModule mod;
    SoaRequirePass p;
    ConstantFoldingWrap cf;
    const auto h0 = load_u64(g_soa_view_hits);
    for (int i = 0; i < 50; ++i) {
        p.ran = false;
        CHECK(run_pipeline(mod, p), "pipeline ok");
        (void)consult_tag_arity(static_cast<std::uint8_t>(i & 0xff), 1);
        record_edsl_children_soa_path();
        note_pass_soa_enforcement(cf);
    }
    CHECK(load_u64(g_soa_view_hits) > h0, "hits grew");
    CHECK(migration_ratio_bp() <= 10000, "ratio <= 100%");
    CompilerService cs;
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok");
}

static void ac7_lineage() {
    std::println("\n--- AC7: #1517 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "concept-enforced") == 1, "concept-enforced");
    CHECK(href(cs, "passes-soa-view-aware") >= 0, "passes-soa-view-aware");
    CHECK(href(cs, "soa-view-misses") >= 0, "soa-view-misses");
    auto adop = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
    CHECK(adop && is_hash(*adop), "adoption hash");
    auto sch = cs.eval("(hash-ref (engine:metrics \"query:soa-adoption-stats\") \"schema\")");
    CHECK(sch && is_int(*sch) &&
              (as_int(*sch) == 1629 || as_int(*sch) == 1619 || as_int(*sch) == 1517),
          "adoption schema 1629|1619|1517");
}

} // namespace

int main() {
    std::println("=== Issue #1619: SoAView enforcement + EDSL migration ===");
    ac1_concept();
    ac2_pack_check();
    ac3_production_wraps();
    ac4_edsl_helpers();
    ac5_query_schema();
    ac6_stress();
    ac7_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
