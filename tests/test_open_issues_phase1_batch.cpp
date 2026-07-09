// test_open_issues_phase1_batch.cpp — legacy alias for the domain suite.
//
// Prefer: tests/domain/test_obs_schema_matrix.cpp + domain/cases/obs_schema_cases.hpp
// This file remains so late-bundle membership / old docs still resolve.
// Implementation is the shared domain matrix (included below).

#include "test_harness.hpp"
#include "domain/cases/obs_schema_cases.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

// Reuse the same runner logic as test_obs_schema_matrix by defining
// the entry point name expected by jit_late3 bundle membership.
// (Duplicate TU is intentional for back-compat; new cases go only in
// obs_schema_cases.hpp + test_obs_schema_matrix.cpp.)

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

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) '{}')", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void bump_standard(Evaluator& ev, const char* slug) {
    if (!slug || !*slug)
        return;
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

void run_all(CompilerService& cs) {
    auto& ev = cs.evaluator();
    for (std::size_t i = 0; i < kStandardCasesCount; ++i) {
        const StandardCase& c = kStandardCases[i];
        auto h = cs.eval(std::format("({})", c.query));
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
    for (std::size_t i = 0; i < kFieldListCasesCount; ++i) {
        const FieldListCase& c = kFieldListCases[i];
        auto h = cs.eval(std::format("({})", c.query));
        CHECK(h && is_hash(*h), std::format("{} returns hash", c.query));
        CHECK(href(cs, c.query, "schema") == c.schema,
              std::format("{} schema == {}", c.query, c.schema));
        for (std::size_t f = 0; f < c.n_fields; ++f) {
            auto v = cs.eval(std::format("(hash-ref ({}) '{}')", c.query, c.fields[f]));
            CHECK(v.has_value(), std::format("{} field present", c.query));
        }
    }
    (void)cs.eval("(terminal:create-buffer)");
    (void)cs.eval("(terminal:diff)");
    (void)cs.eval("(primitives:alias \"q\" \"query:pattern\")");
    auto c = cs.eval("(+ 19 23)");
    CHECK(c && is_int(*c) && as_int(*c) == 42, "classic eval");
}

} // namespace

int aura_issue_open_issues_phase1_batch_run() {
    aura::compiler::CompilerService cs;
    run_all(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_open_issues_phase1_batch_run();
}
#endif
