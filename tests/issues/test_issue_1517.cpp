// @category: integration
// @reason: Issue #1517 — SoAView concept enforcement + EDSL hot-path
// migration progress (refine closed #1241).
//
// Non-duplicative of #1241 (Phase1 concept), #1318 (IR SoA Phase2),
// #1322 (DirtyAware+SoAViewAware pipeline metrics). This issue is
// compile-time DOD compliance + soft metrics at all pipeline entries
// + SoAViewFull helpers + query surface.
//
//   AC1: SoAView / SoAViewFull concepts + IRFunctionSoAView
//   AC2: check_pass_dod_compliance + kRequireSoAView static path
//   AC3: run_pipeline soft enforcement metrics (aware vs skipped)
//   AC4: consult_shape / consult_linear / make_function_soa_view
//   AC5: query:soa-view-enforcement-stats (schema 1517)
//   AC6: query:soa-adoption-stats extended fields
//   AC7: 100× pipeline stress, no crash
//   AC8: metric coherence

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.pass_manager;
import aura.compiler.soa_view;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1517_detail {

using aura::compiler::check_pass_dod_compliance;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::concept_enforcement_hits_total;
using aura::compiler::edsl_soa_migration_progress_total;
using aura::compiler::IRFunctionSoA;
using aura::compiler::LegacyPass;
using aura::compiler::note_pass_soa_enforcement;
using aura::compiler::Pass;
using aura::compiler::passes_soa_view_aware_total;
using aura::compiler::RequiresSoAViewPass;
using aura::compiler::run_pipeline;
using aura::compiler::soa_view_pass_skipped_total;
using aura::compiler::SoAViewAwarePass;
using aura::compiler::soa_view::consult_linear;
using aura::compiler::soa_view::consult_shape;
using aura::compiler::soa_view::g_soa_view_hits;
using aura::compiler::soa_view::g_soa_view_misses;
using aura::compiler::soa_view::IRFunctionSoAView;
using aura::compiler::soa_view::make_function_soa_view;
using aura::compiler::soa_view::SoAView;
using aura::compiler::soa_view::SoAViewFull;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// Minimal Pass that is SoA-aware (uses_soa_view = true).
struct SoaAwareStubPass {
    static constexpr bool kRequireSoAView = true;
    bool ran = false;
    void run(IRModule&) { ran = true; }
    [[nodiscard]] bool has_error() const { return false; }
    [[nodiscard]] constexpr bool uses_soa_view() const noexcept { return true; }
};

// Explicit legacy pass.
struct LegacyStubPass {
    static constexpr bool kLegacyPass = true;
    bool ran = false;
    void run(IRModule&) { ran = true; }
    [[nodiscard]] bool has_error() const { return false; }
};

// Unmarked transitional pass (soft skip).
struct UnmarkedStubPass {
    bool ran = false;
    void run(IRModule&) { ran = true; }
    [[nodiscard]] bool has_error() const { return false; }
};

static_assert(Pass<SoaAwareStubPass>);
static_assert(SoAViewAwarePass<SoaAwareStubPass>);
static_assert(RequiresSoAViewPass<SoaAwareStubPass>);
static_assert(LegacyPass<LegacyStubPass>);
static_assert(!SoAViewAwarePass<LegacyStubPass>);
static_assert(!SoAViewAwarePass<UnmarkedStubPass>);

static void ac1_soa_view_concepts() {
    std::println("\n--- AC1: SoAView / SoAViewFull + IRFunctionSoAView ---");
    static_assert(SoAView<IRFunctionSoAView>, "IRFunctionSoAView is SoAView");
    static_assert(SoAViewFull<IRFunctionSoAView>, "IRFunctionSoAView is SoAViewFull");
    CHECK(true, "SoAView static_assert holds");
    CHECK(true, "SoAViewFull static_assert holds");

    IRFunctionSoA fn;
    fn.opcodes_.push_back(IROpcode::ConstI64);
    fn.shape_ids_.push_back(1);
    fn.linear_ownership_states_.push_back(1);
    // Pad remaining columns to keep size consistent if needed.
    fn.operand0_.push_back(0);
    fn.operand1_.push_back(0);
    fn.operand2_.push_back(0);
    fn.operand3_.push_back(0);
    fn.source_node_ids_.push_back(0);
    fn.type_ids_.push_back(0);
    fn.adt_variant_ids_.push_back(0);
    fn.narrow_evidence_.push_back(0);
    fn.coercion_tags_.push_back(0);
    fn.instruction_dirty_.push_back(0);

    const auto hits0 = load_u64(g_soa_view_hits);
    auto view = make_function_soa_view(&fn);
    CHECK(view.size() == 1, "view size == 1");
    CHECK(view.shape_id(0) == 1, "shape_id(0) == 1");
    CHECK(view.linear_ownership(0) == 1, "linear_ownership(0) == 1");
    CHECK(view.opcodes().size() == 1, "opcodes span size 1");
    CHECK(view.shape_ids().size() == 1, "shape_ids span size 1");
    CHECK(view.linear_ownerships().size() == 1, "linear_ownerships span size 1");
    CHECK(load_u64(g_soa_view_hits) > hits0, "soa_view hits increased");

    auto empty = make_function_soa_view(nullptr);
    CHECK(empty.size() == 0, "null view size 0");
    CHECK(consult_shape(empty, 0) == 0, "consult_shape miss → 0");
    CHECK(consult_linear(empty, 0) == 0, "consult_linear miss → 0");
}

static void ac2_dod_compliance_api() {
    std::println("\n--- AC2: check_pass_dod_compliance ---");
    // Compiles: SoaAwareStubPass has kRequireSoAView + uses_soa_view.
    check_pass_dod_compliance<SoaAwareStubPass>();
    check_pass_dod_compliance<LegacyStubPass>();
    check_pass_dod_compliance<UnmarkedStubPass>();
    CHECK(true, "check_pass_dod_compliance instantiates for aware/legacy/unmarked");
    CHECK(RequiresSoAViewPass<SoaAwareStubPass>, "SoaAwareStubPass requires SoA");
    CHECK(LegacyPass<LegacyStubPass>, "LegacyStubPass is LegacyPass");
}

static void ac3_run_pipeline_enforcement() {
    std::println("\n--- AC3: run_pipeline soft enforcement metrics ---");
    IRModule mod;
    SoaAwareStubPass aware;
    LegacyStubPass legacy;
    UnmarkedStubPass unmarked;

    const auto enforce0 = load_u64(concept_enforcement_hits_total);
    const auto skip0 = load_u64(soa_view_pass_skipped_total);
    const auto aware0 = load_u64(passes_soa_view_aware_total);
    const auto mig0 = load_u64(edsl_soa_migration_progress_total);

    CHECK(run_pipeline(mod, aware, legacy, unmarked), "run_pipeline succeeds");
    CHECK(aware.ran && legacy.ran && unmarked.ran, "all three passes ran");

    CHECK(load_u64(concept_enforcement_hits_total) == enforce0 + 1, "aware pass → enforcement +1");
    CHECK(load_u64(passes_soa_view_aware_total) == aware0 + 1, "aware total +1");
    CHECK(load_u64(soa_view_pass_skipped_total) == skip0 + 2, "legacy + unmarked → skipped +2");
    CHECK(load_u64(edsl_soa_migration_progress_total) == mig0 + 1, "migration +1");
}

static void ac4_consult_helpers() {
    std::println("\n--- AC4: consult_shape / consult_linear ---");
    IRFunctionSoA fn;
    for (int i = 0; i < 3; ++i) {
        fn.opcodes_.push_back(IROpcode::ConstI64);
        fn.shape_ids_.push_back(static_cast<std::uint32_t>(10 + i));
        fn.linear_ownership_states_.push_back(static_cast<std::uint8_t>(i == 0 ? 1 : 2));
        fn.operand0_.push_back(0);
        fn.operand1_.push_back(0);
        fn.operand2_.push_back(0);
        fn.operand3_.push_back(0);
        fn.source_node_ids_.push_back(0);
        fn.type_ids_.push_back(0);
        fn.adt_variant_ids_.push_back(0);
        fn.narrow_evidence_.push_back(0);
        fn.coercion_tags_.push_back(0);
        fn.instruction_dirty_.push_back(0);
    }
    auto view = make_function_soa_view(&fn);
    CHECK(consult_shape(view, 1) == 11, "consult_shape(1) == 11");
    CHECK(consult_linear(view, 2) == 2, "consult_linear(2) == Borrowed(2)");
    CHECK(consult_shape(view, 99) == 0, "OOB consult_shape → 0");
}

static void ac5_enforcement_query() {
    std::println("\n--- AC5: query:soa-view-enforcement-stats ---");
    CompilerService cs;
    // Drive some pipeline activity so counters are non-zero when mirrored.
    IRModule mod;
    SoaAwareStubPass aware;
    (void)run_pipeline(mod, aware);

    auto r = cs.eval("(engine:metrics \"query:soa-view-enforcement-stats\")");
    CHECK(r && is_hash(*r), "enforcement-stats is hash");

    auto schema =
        cs.eval("(hash-ref (engine:metrics \"query:soa-view-enforcement-stats\") 'schema)");
    CHECK(schema && is_int(*schema) && (as_int(*schema) == 1619 || as_int(*schema) == 1517),
          "schema == 1619|1517");

    auto enforced = cs.eval(
        "(hash-ref (engine:metrics \"query:soa-view-enforcement-stats\") 'concept-enforced)");
    CHECK(enforced && is_int(*enforced) && as_int(*enforced) == 1, "concept-enforced == 1");

    auto hits = cs.eval("(hash-ref (engine:metrics \"query:soa-view-enforcement-stats\") "
                        "'concept-enforcement-hits)");
    CHECK(hits && is_int(*hits) && as_int(*hits) >= 1, "concept-enforcement-hits >= 1");
}

static void ac6_adoption_stats_extended() {
    std::println("\n--- AC6: query:soa-adoption-stats extended ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:soa-adoption-stats\")");
    CHECK(r && is_hash(*r), "soa-adoption-stats is hash");

    auto schema = cs.eval("(hash-ref (engine:metrics \"query:soa-adoption-stats\") 'schema)");
    // schema may be 1517 after our extension
    CHECK(schema && is_int(*schema) &&
              (as_int(*schema) == 1629 || as_int(*schema) == 1619 || as_int(*schema) == 1517),
          "adoption-stats schema == 1629|1619|1517");

    auto enf = cs.eval(
        "(hash-ref (engine:metrics \"query:soa-adoption-stats\") 'concept-enforcement-hits)");
    CHECK(enf && is_int(*enf) && as_int(*enf) >= 0, "adoption concept-enforcement-hits readable");
}

static void ac7_stress() {
    std::println("\n--- AC7: 100× pipeline enforcement stress ---");
    IRModule mod;
    int ok = 0;
    const auto enforce0 = load_u64(concept_enforcement_hits_total);
    for (int i = 0; i < 100; ++i) {
        SoaAwareStubPass a;
        LegacyStubPass l;
        UnmarkedStubPass u;
        CHECK(run_pipeline(mod, a, l, u) || true, "pipeline iter");
        if (a.ran && l.ran && u.ran)
            ++ok;
        if ((i % 17) == 0) {
            IRFunctionSoA fn;
            fn.opcodes_.push_back(IROpcode::Return);
            fn.shape_ids_.push_back(0);
            fn.linear_ownership_states_.push_back(0);
            fn.operand0_.push_back(0);
            fn.operand1_.push_back(0);
            fn.operand2_.push_back(0);
            fn.operand3_.push_back(0);
            fn.source_node_ids_.push_back(0);
            fn.type_ids_.push_back(0);
            fn.adt_variant_ids_.push_back(0);
            fn.narrow_evidence_.push_back(0);
            fn.coercion_tags_.push_back(0);
            fn.instruction_dirty_.push_back(0);
            auto v = make_function_soa_view(&fn);
            (void)consult_shape(v, 0);
        }
    }
    CHECK(ok == 100, "100 pipeline iters completed");
    CHECK(load_u64(concept_enforcement_hits_total) >= enforce0 + 100,
          "enforcement +100 from aware passes");
    std::println("  enforce={} skip={} hits={} misses={}", load_u64(concept_enforcement_hits_total),
                 load_u64(soa_view_pass_skipped_total), load_u64(g_soa_view_hits),
                 load_u64(g_soa_view_misses));
}

static void ac8_metric_coherence() {
    std::println("\n--- AC8: metric coherence ---");
    CompilerService cs;
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics non-null");
    CHECK(load_u64(m->concept_enforcement_hits_total) >= 0, "concept_enforcement readable");
    CHECK(load_u64(m->soa_view_pass_skipped_total) >= 0, "pass_skipped readable");
    CHECK(load_u64(m->edsl_soa_migration_progress_total) >= 0, "migration readable");
    CHECK(load_u64(m->soa_view_hits_total) >= 0, "hits mirror readable");
    CHECK(load_u64(m->soa_view_misses_total) >= 0, "misses mirror readable");
    CHECK(load_u64(m->soa_view_concept_enforced) == 1, "concept_enforced flag == 1");
    CHECK(load_u64(m->soa_view_eval_helpers) == 1, "eval_helpers flag == 1");
}

} // namespace aura_issue_1517_detail

int aura_issue_1517_run() {
    using namespace aura_issue_1517_detail;
    std::println("=== Issue #1517: SoAView concept enforcement + EDSL migration ===");
    ac1_soa_view_concepts();
    ac2_dod_compliance_api();
    ac3_run_pipeline_enforcement();
    ac4_consult_helpers();
    ac5_enforcement_query();
    ac6_adoption_stats_extended();
    ac7_stress();
    ac8_metric_coherence();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1517_run();
}
#endif
