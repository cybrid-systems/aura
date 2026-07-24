// test_obs_schema_matrix.cpp — Domain suite: observability + production schemas
//
// Replaces the "one test_issue_N.cpp per stats surface" pattern for
// schema/bump gate ACs. Also folds Wave 2 production sweep field-list
// gates (was test_domain_production_sweep.cpp).
//
// Add new Close #N observability surfaces by editing
// tests/domain/cases/obs_schema_cases.hpp; production flag surfaces via
// cases/production_sweep_cases.hpp — do not add a new binary.
//
// Coverage:
//   - StandardTotalHits: total/hits/savings/active + schema + bump
//   - FieldList: schema + required field keys present
//   - Production field-list schemas (Wave 2 production_* fold)
//   - Light primitives used by late surfaces (terminal:*, primitives:alias)
//
// See tests/domain/README.md for the testing policy.

#include "test_harness.hpp"
#include "obs_schema_cases.hpp"
#include "production_sweep_cases.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::obs::FieldListCase;
using aura::test::obs::kFieldListCases;
using aura::test::obs::kFieldListCasesCount;
using aura::test::obs::kStandardCases;
using aura::test::obs::kStandardCasesCount;
using aura::test::obs::StandardCase;
using aura::test::prod::kProdCases;
using aura::test::prod::kProdCasesCount;
using aura::test::prod::ProdCase;

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Map bump_slug → Evaluator bump helpers (standard total/hits/savings).
void bump_standard(Evaluator& ev, const char* slug) {
    if (!slug || !*slug)
        return;
    // Keep this switch in sync with kStandardCases[].bump_slug.
    // Prefer adding a row + CASE over a new test_issue_*.cpp.
#define CASE(s)                                                                                    \
    if (std::strcmp(slug, #s) == 0) {                                                              \
        ev.bump_##s();                                                                             \
        ev.bump_##s##_hit();                                                                       \
        ev.bump_##s##_savings(2);                                                                  \
        return;                                                                                    \
    }
    CASE(pass_shape_epoch)
    CASE(edsl_hotpath_real)
    CASE(dead_coercion_elim)
    CASE(occurrence_renarrow)
    CASE(linear_escape_mutate)
    CASE(typed_mutate_coercion)
    CASE(fiber_epoch_type)
    CASE(sv_feedback_mutate)
    CASE(seva_harness_v2)
    CASE(typed_mut_audit)
    CASE(stable_ref_full_v2)
    CASE(longrun_ai_infra)
    CASE(ai_native_meta)
    CASE(orch_telemetry)
    CASE(per_fiber_ex_state)
    CASE(aot_hotswap_pipe)
    CASE(macro_hyg_query_v2)
    CASE(reflect_edsl_v2)
    CASE(selfevo_hyg_dirty)
    CASE(sv_fb_closedloop)
    CASE(pattern_defuse_hyg)
    CASE(stable_ref_mutlog)
    CASE(dirty_impact_v2)
    CASE(live_irclosure_gc)
    CASE(src_marker_linear)
    CASE(term_buf_diff)
    CASE(render_obs_v2)
    CASE(render_jit_soa)
    CASE(arena_ldefrag_v2)
    CASE(irsoa_dirty_v2)
    CASE(val_shape_ceval_v2)
    CASE(defuse_infer_part)
    CASE(own_escape_post)
    CASE(typed_audit_pass)
    CASE(sv_backend_bi)
    CASE(large_sv_pattern)
    CASE(longrun_sref_dirty)
    CASE(sv_eda_prims)
    CASE(prim_quota_fiber)
    CASE(decl_prim_reg)
    CASE(prim_ns_alias)
    CASE(guard_steal_gc_v2)
    CASE(dirty_ircache_cons)
    CASE(stats_builder_ref)
    CASE(load_or_zero_help)
    CASE(cpp26_mod_sweep)
    CASE(metrics_meta_refl)
    CASE(test_harness_boot)
    CASE(bundle_codegen_dec)
    CASE(test_bundle_mig)
    CASE(test_profile_flag)
    CASE(test_harness_mod)
    CASE(test_json_report)
    CASE(gcc16_modules_env)
#undef CASE
}

void run_standard_cases(CompilerService& cs) {
    auto& ev = cs.evaluator();
    std::println("\n=== Domain suite: standard total/hits schemas ({}) ===", kStandardCasesCount);
    for (std::size_t i = 0; i < kStandardCasesCount; ++i) {
        const StandardCase& c = kStandardCases[i];
        auto h = cs.eval(aura::test::aura_call_expr(c.query));
        CHECK(h && is_hash(*h), std::format("{} returns hash", c.query));
        CHECK(href(cs, c.query, "schema") == c.schema,
              std::format("{} schema == {}", c.query, c.schema));
        CHECK(href(cs, c.query, "active") == 1, std::format("{} active", c.query));
        const auto t0 = href(cs, c.query, "total");
        bump_standard(ev, c.bump_slug);
        CHECK(href(cs, c.query, "total") == t0 + 1, std::format("{} total bumps", c.query));
        CHECK(href(cs, c.query, "hits") >= 1, std::format("{} hits", c.query));
        CHECK(href(cs, c.query, "savings") >= 2, std::format("{} savings", c.query));
    }
}

void run_field_list_cases(CompilerService& cs) {
    std::println("\n=== Domain suite: field-list schemas ({}) ===", kFieldListCasesCount);
    for (std::size_t i = 0; i < kFieldListCasesCount; ++i) {
        const FieldListCase& c = kFieldListCases[i];
        auto h = cs.eval(aura::test::aura_call_expr(c.query));
        CHECK(h && is_hash(*h), std::format("{} returns hash", c.query));
        CHECK(href(cs, c.query, "schema") == c.schema,
              std::format("{} schema == {}", c.query, c.schema));
        for (std::size_t f = 0; f < c.n_fields; ++f) {
            const char* key = c.fields[f];
            auto v = cs.eval(
                std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(c.query), key));
            CHECK(v.has_value(), std::format("{} field '{}' present", c.query, key));
        }
    }
}

void run_production_cases(CompilerService& cs) {
    std::println("\n=== Domain suite: production field-list schemas ({}) ===", kProdCasesCount);
    for (std::size_t i = 0; i < kProdCasesCount; ++i) {
        const ProdCase& c = kProdCases[i];
        auto h = cs.eval(aura::test::aura_call_expr(c.query));
        CHECK(h && is_hash(*h), std::format("{} returns hash ({})", c.query, c.source_stem));
        // #1261–#1265 lineage may report schema 1625 or 1261.
        const auto got = href(cs, c.query, "schema");
        const bool schema_ok = (got == c.schema) ||
                               (c.schema == 1625 && (got == 1625 || got == 1261)) ||
                               (c.schema == 1261 && (got == 1625 || got == 1261));
        CHECK(schema_ok,
              std::format("{} schema == {} (got {}, {})", c.query, c.schema, got, c.source_stem));
        for (std::size_t f = 0; f < c.n_fields; ++f) {
            const char* key = c.fields[f];
            auto v = cs.eval(
                std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(c.query), key));
            CHECK(v.has_value(),
                  std::format("{} field '{}' present ({})", c.query, key, c.source_stem));
        }
    }
}

void run_light_primitives(CompilerService& cs) {
    std::println("\n=== Domain suite: light production primitives ===");
    auto buf = cs.eval("(terminal:create-buffer)");
    CHECK(buf && is_bool(*buf), "terminal:create-buffer");
    auto diff = cs.eval("(terminal:diff)");
    CHECK(diff && is_bool(*diff), "terminal:diff");
    auto alias = cs.eval("(primitives:alias \"q\" \"query:pattern\")");
    CHECK(alias && is_bool(*alias), "primitives:alias");
    auto alias_bad = cs.eval("(primitives:alias)");
    CHECK(alias_bad && is_bool(*alias_bad), "primitives:alias arity-fail");
}

void run_regression(CompilerService& cs) {
    std::println("\n=== Domain suite: classic eval regression ===");
    auto c = cs.eval("(+ 19 23)");
    CHECK(c && is_int(*c) && as_int(*c) == 42, "classic eval (+ 19 23) == 42");
}

void run_all(CompilerService& cs) {
    run_standard_cases(cs);
    run_field_list_cases(cs);
    run_production_cases(cs);
    run_light_primitives(cs);
    run_regression(cs);
}

} // namespace

int aura_issue_obs_schema_matrix_run() {
    aura::compiler::CompilerService cs;
    run_all(cs);
    return RUN_ALL_TESTS();
}

// Legacy alias (was test_domain_production_sweep).
int aura_issue_domain_production_sweep_run() {
    return aura_issue_obs_schema_matrix_run();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_obs_schema_matrix_run();
}
#endif
