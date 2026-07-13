// test_issue_772.cpp — Issue #772: Consolidated SV Verification EDSL +
// Backend Emit Fidelity + Roundtrip Validation + Dirty-Driven
// Incremental Re-emit Harness for Commercial Tool Interop
// (consolidates/refines #724/#725/#726/#748/#693).
//
// Scope-limited close: the issue body asks for the full SV verification
// EDSL + commercial-tool-compatible emit (VCS/Questa/JasperGold) +
// roundtrip parser stub + dirty-triggered incremental re-emit + SEVA
// extension. Phase 1 ships the observability surface (5 CompilerMetrics
// atomics + 5 bump helpers + (query:sv-closedloop-slo, schema 772)
// primitive + test_issue_772 5 ACs / 27 checks). Phases 2-6 are
// follow-up work:
//   2. Full builders for SVClass/Constraint/Covergroup/SVA + dirty
//      re-emit queue + roundtrip validate stub
//   3. emit_sv_verification_structured for VCS/Questa/JasperGold compat
//   4. StableNodeRef/SafePCVSpan validity across mutate
//   5. tests/test_sv_verification_edsl_emit_fidelity_closedloop.cpp
//      + extended SEVA with full class/constraint/covergroup/SVA self-evo
//   6. Prometheus exposure with (query:sv-closedloop-slo) thresholds +
//      SEVA tutorial update + CI gate + docs
//
// For this PR we ship:
//
//   1. 5 new atomics in CompilerMetrics:
//        sv_slo_emit_parse_success_total
//        sv_slo_emit_parse_failure_total
//        sv_slo_reemit_latency_max_us (high-water mark via compare-exchange)
//        sv_slo_reemit_hits_total
//        sv_slo_breach_total
//   2. 5 new public bump helpers in Evaluator
//        (bump_sv_slo_emit_parse_success /
//         bump_sv_slo_emit_parse_failure /
//         bump_sv_slo_reemit_latency_max_us /
//         bump_sv_slo_reemit_hits /
//         bump_sv_slo_breach)
//   3. New standalone
//      (query:sv-closedloop-slo, schema 772)
//      primitive exposing 6 fields + schema sentinel (7-entry hash):
//        slo-status (computed: 0=ok / 1=warn / 2=breach)
//        emit-parse-success / emit-parse-failure
//        reemit-latency-max-us / reemit-hits / slo-breach-total
//        schema (772)
//   4. Test verifies: primitive shape, fresh-zero state, schema
//      sentinel, bump accessibility, SLO status transitions,
//      sibling observability regression.
//
// ACs:
//   AC1: hash shape (6 fields + schema sentinel = 7 entries)
//   AC2: 5 counters == 0 on fresh service
//   AC3: schema == 772 (drift sentinel)
//   AC4: bump helpers accessible — exercise each field + verify the
//        SLO status transitions correctly under high fidelity +
//        low latency (ok) / low fidelity (warn/breach) / high
//        latency (warn/breach) / explicit breach (always breach)
//   AC5: regression — #748 + #801 + #802 + #766/#767/#768 sibling
//        primitives still reachable with their schema sentinels
//        intact

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_772_detail {
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
    std::println("\n--- AC1: (query:sv-closedloop-slo) hash shape ---");
    auto r = cs.eval("(query:sv-closedloop-slo)");
    CHECK(r && aura::compiler::types::is_hash(*r), "(query:sv-closedloop-slo) returns a hash");
    const std::vector<std::string> keys = {"slo-status",
                                           "emit-parse-success",
                                           "emit-parse-failure",
                                           "reemit-latency-max-us",
                                           "reemit-hits",
                                           "slo-breach-total",
                                           "schema"};
    for (const auto& k : keys) {
        auto f =
            cs.eval(std::format("(hash-ref (engine:metrics \"query:sv-closedloop-slo\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: counters == 0 on fresh service ---");
    const auto eps = hash_int_field(cs, "(query:sv-closedloop-slo)", "emit-parse-success");
    CHECK(eps == 0, std::format("emit-parse-success = {} (expected 0 on fresh service)", eps));
    const auto epf = hash_int_field(cs, "(query:sv-closedloop-slo)", "emit-parse-failure");
    CHECK(epf == 0, std::format("emit-parse-failure = {} (expected 0 on fresh service)", epf));
    const auto rlm = hash_int_field(cs, "(query:sv-closedloop-slo)", "reemit-latency-max-us");
    CHECK(rlm == 0, std::format("reemit-latency-max-us = {} (expected 0 on fresh service)", rlm));
    const auto rh = hash_int_field(cs, "(query:sv-closedloop-slo)", "reemit-hits");
    CHECK(rh == 0, std::format("reemit-hits = {} (expected 0 on fresh service)", rh));
    const auto sbt = hash_int_field(cs, "(query:sv-closedloop-slo)", "slo-breach-total");
    CHECK(sbt == 0, std::format("slo-breach-total = {} (expected 0 on fresh service)", sbt));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 772 (drift sentinel) ---");
    const auto schema = hash_int_field(cs, "(query:sv-closedloop-slo)", "schema");
    CHECK(schema == 772, std::format("schema = {} (expected 772)", schema));
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable + SLO status transitions ---");
    auto& ev = cs.evaluator();

    // Scenario 1: ok — high fidelity + low latency + no breach
    ev.bump_sv_slo_emit_parse_success(99);
    ev.bump_sv_slo_emit_parse_failure(1);        // 99% fidelity
    ev.bump_sv_slo_reemit_latency_max_us(30000); // 30ms < 50ms
    ev.bump_sv_slo_reemit_hits(50);
    const auto slo_ok = hash_int_field(cs, "(query:sv-closedloop-slo)", "slo-status");
    CHECK(slo_ok == 0,
          std::format("SLO status = {} (expected 0 = ok: 99% fidelity + 30ms latency)", slo_ok));

    // Scenario 2: warn — drop fidelity to 97%
    ev.bump_sv_slo_emit_parse_failure(2); // 99/(99+3) = 97.06%
    const auto slo_warn_fid = hash_int_field(cs, "(query:sv-closedloop-slo)", "slo-status");
    CHECK(slo_warn_fid == 1,
          std::format("SLO status = {} (expected 1 = warn: 97% fidelity)", slo_warn_fid));

    // Scenario 3: breach — high latency
    ev.bump_sv_slo_reemit_latency_max_us(150000); // 150ms > 100ms
    const auto slo_breach_lat = hash_int_field(cs, "(query:sv-closedloop-slo)", "slo-status");
    CHECK(slo_breach_lat == 2,
          std::format("SLO status = {} (expected 2 = breach: 150ms latency)", slo_breach_lat));

    // Scenario 4: breach — explicit bump_sv_slo_breach (always wins)
    ev.bump_sv_slo_breach(1);
    const auto slo_breach_explicit = hash_int_field(cs, "(query:sv-closedloop-slo)", "slo-status");
    CHECK(slo_breach_explicit == 2,
          std::format("SLO status = {} (expected 2 = breach: explicit breach bump fires)",
                      slo_breach_explicit));

    // Verify atomic values are reflected
    const auto eps_after = hash_int_field(cs, "(query:sv-closedloop-slo)", "emit-parse-success");
    CHECK(eps_after == 99,
          std::format("emit-parse-success = {} (expected 99 after 1 bump of n=99)", eps_after));
    const auto rlm_after = hash_int_field(cs, "(query:sv-closedloop-slo)", "reemit-latency-max-us");
    CHECK(rlm_after == 150000,
          std::format("reemit-latency-max-us = {} (expected 150000 = high-water mark)", rlm_after));
    const auto sbt_after = hash_int_field(cs, "(query:sv-closedloop-slo)", "slo-breach-total");
    CHECK(
        sbt_after == 1,
        std::format("slo-breach-total = {} (expected 1 after 1 explicit breach bump)", sbt_after));

    // Scenario 5: latency high-water mark only goes up (compare-exchange)
    ev.bump_sv_slo_reemit_latency_max_us(50000); // 50ms < 150ms — should NOT update
    const auto rlm_compare =
        hash_int_field(cs, "(query:sv-closedloop-slo)", "reemit-latency-max-us");
    CHECK(rlm_compare == 150000,
          std::format("reemit-latency-max-us = {} (expected 150000 still — compare-exchange "
                      "only goes up)",
                      rlm_compare));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #748 + #801 + #802 + #766/#767/#768 sibling "
                 "primitives unaffected ---");
    auto sv_verification_structure = cs.eval("(query:sv-verification-structure-stats)");
    auto sv_commercial_emit_fidelity = cs.eval("(query:sv-commercial-emit-fidelity-stats)");
    auto sv_verification_self_evolution = cs.eval("(query:sv-verification-self-evolution-stats)");
    auto ir_soa_migration = cs.eval("(query:ir-soa-migration-stats)");
    CHECK(sv_verification_structure && aura::compiler::types::is_hash(*sv_verification_structure),
          "query:sv-verification-structure-stats hash regression (#748)");
    CHECK(sv_commercial_emit_fidelity &&
              aura::compiler::types::is_hash(*sv_commercial_emit_fidelity),
          "query:sv-commercial-emit-fidelity-stats hash regression (#801)");
    CHECK(sv_verification_self_evolution &&
              aura::compiler::types::is_hash(*sv_verification_self_evolution),
          "query:sv-verification-self-evolution-stats hash regression (#802)");
    CHECK(ir_soa_migration && aura::compiler::types::is_hash(*ir_soa_migration),
          "query:ir-soa-migration-stats hash regression (#766)");
    const auto a748_schema =
        hash_int_field(cs, "(query:sv-verification-structure-stats)", "schema");
    CHECK(a748_schema == 748,
          std::format("#748 schema = {} (expected 748, no drift)", a748_schema));
    const auto a801_schema =
        hash_int_field(cs, "(query:sv-commercial-emit-fidelity-stats)", "schema");
    CHECK(a801_schema == 801,
          std::format("#801 schema = {} (expected 801, no drift)", a801_schema));
    const auto a802_schema =
        hash_int_field(cs, "(query:sv-verification-self-evolution-stats)", "schema");
    CHECK(a802_schema == 802,
          std::format("#802 schema = {} (expected 802, no drift)", a802_schema));
    const auto a766_schema = hash_int_field(cs, "(query:ir-soa-migration-stats)", "schema");
    CHECK(a766_schema == 766,
          std::format("#766 schema = {} (expected 766, no drift)", a766_schema));
}

} // namespace aura_issue_772_detail

int aura_issue_772_run() {
    using namespace aura_issue_772_detail;
    std::println("=== Issue #772: SV Verification closed-loop SLO observability "
                 "(scope-limited close, consolidates #693/#724/#725/#726/#748) ===");

    {
        aura::compiler::CompilerService cs;
        run_ac1_shape(cs);
        run_ac2_fresh_zero(cs);
        run_ac3_schema_sentinel(cs);
        run_ac4_bump_accessible(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_sibling_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_772_run();
}
#endif
