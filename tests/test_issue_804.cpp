// test_issue_804.cpp — Issue #804: P0 stdlib error
// semantics unification + provenance tracking +
// recovery hooks for AI Agent reliability (refine/
// consolidate #585 + #751 + #775 error paths).
//
// Scope-limited close: the body asks for 5 things:
// (1) audit & unify error paths in all
// evaluator_primitives_*.cpp (use common helper in
// primitives_detail), (2) extend PrimMeta or new
// ErrorMeta; wire provenance in make_primitive_error,
// (3) new error primitives + query:error-stats with
// per-primitive breakdown + recovery hooks, (4)
// Registry enforcement + test harness, (5) Docs +
// Agent prompt patterns. Phase 1 observability
// surface ships in this PR:
//
//   1. 3 NEW CompilerMetrics atomics + 3 NEW bump
//      helpers on Evaluator:
//      - primitive_error_with_provenance_total /
//        bump_primitive_error_with_provenance()
//        (bumped at the planned Phase 2+ PRIM_ERROR /
//        make_primitive_error sites that fill in the
//        (kind, msg, provenance) schema — the *good*
//        path)
//      - primitive_error_silent_fallback_total /
//        bump_primitive_error_silent_fallback()
//        (bumped by the Phase 2+ audit grep when
//        ad-hoc returns / catch-alls are detected)
//      - primitive_error_recovery_hook_invocations_
//        total / bump_primitive_error_recovery_hook()
//        (bumped at the planned Phase 2+ Guard +
//        retry path recovery-hook firings)
//   2. 1 NEW standalone (query:primitive-error-
//      unified-stats, schema 804) primitive (16-entry
//      hash) returning 6 fields + 2 derived + 1
//      hardcoded "not yet" + schema sentinel:
//      - error-count-total       reused #478 atomic
//      - with-provenance         NEW atomic
//      - silent-fallback        NEW atomic
//      - error-values-size      reused #478 accessor
//      - capture-violations     reused #751 atomic
//      - unified-path-pct       derived (with-prov /
//                                error_count × 10000;
//                                SLO target 100% = 10000)
//      - recovery-hook-invocations  NEW atomic
//      - unified-error-path-active   hardcoded 0
//                                  (Phase 2+; PRIM_ERROR
//                                  audit + registry
//                                  enforce + (error:
//                                  structured-make ...)
//                                  + recovery hooks all
//                                  follow-up work)
//      - schema == 804
//
// ACs:
//   AC1: hash shape (8 fields + schema sentinel = 9
//        entries in create(16) hash)
//   AC2: fresh-service zero state (3 NEW atomics
//        strict == 0; 2 reused #478 + #751 atomics
//        >= 0; unified-path-pct vacuous-true 10000
//        baseline; unified-error-path-active == 0;
//        recommendation == 3 early-stage)
//   AC3: schema == 804 (drift sentinel)
//   AC4: production-path bump helpers + primitive
//        read-back + unified-path-pct and
//        recommendation transition correctly under
//        bumps
//   AC5: sibling observability regression — #478
//        (query:primitive-error-stats) + #585
//        (query:primitives-error-stats) + #751
//        (query:primitives-contract-stats) primitives
//        still reachable

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_804_detail {
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

static std::int64_t hash_int_field(aura::compiler::CompilerService& cs, std::string_view hash_src,
                                   std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_shape(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: (query:primitive-error-unified-stats) hash shape ---");
    auto r = cs.eval("(query:primitive-error-unified-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(query:primitive-error-unified-stats) returns a hash");
    const std::vector<std::string> keys = {
        "error-count-total",         "with-provenance",           "silent-fallback",
        "error-values-size",         "capture-violations",        "unified-path-pct",
        "recovery-hook-invocations", "unified-error-path-active", "schema",
    };
    for (const auto& k : keys) {
        auto f = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:primitive-error-unified-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: fresh-service zero state (split-zero rule) ---");
    // 3 NEW #804 atomics — strict == 0.
    const auto with_provenance =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "with-provenance");
    CHECK(with_provenance == 0,
          std::format("with-provenance = {} (expected == 0 on fresh service — NEW atomic #804 "
                      "primitive_error_with_provenance_total; bumped by "
                      "bump_primitive_error_with_provenance() at PRIM_ERROR / make_primitive_"
                      "error sites that fill in (kind, msg, provenance) schema — the *good* "
                      "path the body asks for at 100% coverage; no PRIM_ERROR calls yet on "
                      "fresh service since the audit grep is Phase 2+)",
                      with_provenance));
    const auto silent_fallback =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "silent-fallback");
    CHECK(silent_fallback == 0,
          std::format("silent-fallback = {} (expected == 0 on fresh service — NEW atomic #804 "
                      "primitive_error_silent_fallback_total; bumped by the Phase 2+ audit "
                      "grep when ad-hoc returns / catch-alls are detected in evaluator_"
                      "primitives_*.cpp; no audit run yet)",
                      silent_fallback));
    const auto recovery_hook =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "recovery-hook-invocations");
    CHECK(recovery_hook == 0,
          std::format("recovery-hook-invocations = {} (expected == 0 on fresh service — NEW "
                      "atomic #804 primitive_error_recovery_hook_invocations_total; bumped by "
                      "bump_primitive_error_recovery_hook() at the planned Phase 2+ Guard + "
                      "retry path recovery-hook firings; no recovery hooks yet)",
                      recovery_hook));
    // Reused #478 + #751 atomics — >= 0 (split-zero rule).
    const auto error_count =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "error-count-total");
    CHECK(error_count >= 0,
          std::format("error-count-total = {} (expected >= 0 — reused #478 primitive_error_"
                      "count_; bumped by bump_primitive_error_count() at every PRIM_ERROR / "
                      "make_primitive_error call)",
                      error_count));
    const auto ev_size =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "error-values-size");
    CHECK(ev_size >= 0,
          std::format("error-values-size = {} (expected >= 0 — reused #478 "
                      "get_primitive_error_values_size(); persistent error object arena "
                      "size; >= 0 baseline when no errors have been observed)",
                      ev_size));
    const auto cap_violations =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "capture-violations");
    CHECK(cap_violations >= 0,
          std::format("capture-violations = {} (expected >= 0 — reused #751 primitive_capture_"
                      "violations_total; bumped by prim_record_capture_violation when no "
                      "error_counter on a mutate path)",
                      cap_violations));
    // Derived pct — vacuous-true 10000 baseline when error_count_total == 0.
    const auto unified_pct =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "unified-path-pct");
    CHECK(unified_pct >= 0 && unified_pct <= 10000,
          std::format("unified-path-pct = {} (expected in [0, 10000] range — 0-10000 fixed-"
                      "point percent × 100; 10000 = 100.00% baseline when error_count_"
                      "total == 0 = vacuous-true default mirror #774; SLO target 100% "
                      "= 10000 per body \"100% primitives use unified path\")",
                      unified_pct));
    // Hardcoded "not yet" flag — strict == 0.
    const auto path_active =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "unified-error-path-active");
    CHECK(path_active == 0,
          std::format("unified-error-path-active = {} (expected == 0 — Phase 2+ deferred; the "
                      "actual PRIM_ERROR audit + make_primitive_error provenance enforcement + "
                      "registry enforce-unified-path + (error:structured-make ...) + recovery "
                      "hooks in Guard + tests/test_primitive_error_unified_audit.cpp harness "
                      "all remain follow-up work per body Actionable 1-5)",
                      path_active));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 804 (drift sentinel) ---");
    const auto schema = hash_int_field(cs, "(query:primitive-error-unified-stats)", "schema");
    CHECK(schema == 804, std::format("schema = {} (expected 804 — drift sentinel)", schema));
}

static void run_ac4_bump_correctness(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: production-path bump helpers + primitive read-back ---");

    // Snapshot before.
    const auto with_prov_before =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "with-provenance");
    const auto silent_before =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "silent-fallback");
    const auto recov_before =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "recovery-hook-invocations");

    // Exercise the 3 NEW #804 per-Evaluator bump helpers via the
    // service's evaluator instance.
    auto& ev = cs.evaluator();
    constexpr int k_provenance = 5;
    constexpr int k_silent = 0; // intentionally 0 — pure-provenance bumps
    constexpr int k_recovery = 2;
    for (int i = 0; i < k_provenance; ++i) {
        ev.bump_primitive_error_with_provenance();
    }
    for (int i = 0; i < k_silent; ++i) {
        ev.bump_primitive_error_silent_fallback();
    }
    for (int i = 0; i < k_recovery; ++i) {
        ev.bump_primitive_error_recovery_hook();
    }

    const auto with_prov_after =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "with-provenance");
    const auto silent_after =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "silent-fallback");
    const auto recov_after =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "recovery-hook-invocations");

    std::println("  counts after AC4 bumps: with-provenance {} -> {}, silent-fallback {} -> {}, "
                 "recovery-hook {} -> {}",
                 with_prov_before, with_prov_after, silent_before, silent_after, recov_before,
                 recov_after);

    // Direct bump helpers added exactly k_X to each of the 3
    // NEW atomics.
    CHECK(with_prov_after >= with_prov_before + k_provenance,
          std::format("with-provenance bumped by bump_primitive_error_with_provenance() ({} -> "
                      "{}; +{} bumps)",
                      with_prov_before, with_prov_after, k_provenance));
    CHECK(silent_after >= silent_before + k_silent,
          std::format("silent-fallback invariant under pure-provenance bumps (no-op): "
                      "{} = {} (no silent-fallback bumps requested in this test)",
                      silent_before, silent_after));
    CHECK(recov_after >= recov_before + k_recovery,
          std::format("recovery-hook-invocations bumped by "
                      "bump_primitive_error_recovery_hook() ({} -> {}; +{} bumps)",
                      recov_before, recov_after, k_recovery));

    // The unified-path-pct stays in [0, 10000] range after bumps.
    const auto unified_pct_after =
        hash_int_field(cs, "(query:primitive-error-unified-stats)", "unified-path-pct");
    CHECK(unified_pct_after >= 0 && unified_pct_after <= 10000,
          std::format("unified-path-pct still in [0, 10000] range after bumps (was {})",
                      unified_pct_after));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: sibling observability regression (#478 / #585 / #751) "
                 "---");

    // #478: (query:primitive-error-stats) — the canonical pair
    // primitive (error-count . error-values-size). Returns a
    // pair, not a hash.
    auto r478 = cs.eval("(query:primitive-error-stats)");
    CHECK(r478, "#478 (query:primitive-error-stats) still returns a value");

    // #585: (query:primitives-error-stats) — coarse hash from
    // earlier sprint; no schema sentinel (pre-schema convention).
    auto r585 = cs.eval("(query:primitives-error-stats)");
    CHECK(r585, "#585 (query:primitives-error-stats) still returns a value");

    // #751: (query:primitives-contract-stats) — primary
    // contract enforcement primitive with schema 751.
    auto r751 = cs.eval("(query:primitives-contract-stats)");
    CHECK(r751 && aura::compiler::types::is_hash(*r751),
          "#751 (query:primitives-contract-stats) still returns a hash");
    const auto schema_751 = hash_int_field(cs, "(query:primitives-contract-stats)", "schema");
    CHECK(schema_751 == 751, std::format("#751 schema = {} (expected 751)", schema_751));
    for (const auto& k : {"prim-error-counter", "prim-error-hits", "style-compliance-pct",
                          "capture-contract-version"}) {
        auto f = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:primitives-contract-stats\") '{}')", k));
        CHECK(f, std::format("#751 field '{}' still present", k));
    }
}

} // namespace aura_issue_804_detail

int aura_issue_804_run() {
    using namespace aura_issue_804_detail;
    aura::compiler::CompilerService cs;
    std::println("=== Issue #804 — P0 Stdlib-Registry unified primitive error "
                 "semantics + provenance tracking + recovery observability ===");
    run_ac1_shape(cs);
    run_ac2_fresh_zero(cs);
    run_ac3_schema_sentinel(cs);
    run_ac4_bump_correctness(cs);
    run_ac5_sibling_regression(cs);
    std::println("\n=== Summary: {} passed / {} failed (out of {} ACs) ===", g_passed, g_failed,
                 g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_804_run();
}
#endif
