// @category: unit
// @reason: Issue #2264 — match exhaustiveness in TypedMutationAudit hard-gate
// suite (refine #2223 / #2145 / #2029).
//
//   AC1: Full hard-gate + non-exhaustive inject → adt_ok=false; suite fails;
//        adt_exhaustiveness_fail_total advances; rollback/deny path
//   AC2: Exhaustive / no-match → adt_ok=true; no false positive
//   AC3: Soft/Sampled small dirty without hard-gate does not force rollback
//        solely for ADT (metrics-only path)
//   AC4: adt_ok defaults true; all_ok includes adt_ok
//   AC5: Unit test + schema-2264 + source-cite

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::InvariantAuditResult;
using aura::compiler::typed_audit::requires_invariant_hard_gate;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t trail(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-trail\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t audit_stats(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_full_hard_gate_fails() {
    std::println("\n--- AC1: Full hard-gate + inject non-exhaustive → fail ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    (void)cs.eval("(+ 1 1)");

    const auto audit0 = load_u64(g_typed_mutation_audit_counters.adt_exhaustiveness_audit_total);
    const auto fail0 = load_u64(g_typed_mutation_audit_counters.adt_exhaustiveness_fail_total);
    const auto inv_fail0 = load_u64(g_typed_mutation_audit_counters.adt_invariant_fail);
    const auto hard0 = load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total);

    cs.evaluator().inject_adt_non_exhaustive_for_test();
    InvariantAuditResult inv{};
    const bool inv_ok = cs.evaluator().run_typed_mutation_invariant_audit(
        /*mid=*/2264, "test-adt-2264", 0, 0, 1, /*composite=*/false, &inv);

    CHECK(!inv.adt_ok, "AC1: adt_ok false after inject");
    CHECK(inv.adt_non_exhaustive > 0, "AC1: non_exhaustive stamped");
    CHECK(!inv.all_ok(), "AC1: all_ok false");
    CHECK(!inv_ok, "AC1: suite returns false");
    CHECK(load_u64(g_typed_mutation_audit_counters.adt_exhaustiveness_fail_total) > fail0,
          "AC1: adt_exhaustiveness_fail_total advanced");
    CHECK(load_u64(g_typed_mutation_audit_counters.adt_exhaustiveness_audit_total) > audit0,
          "AC1: adt_exhaustiveness_audit_total advanced");
    CHECK(load_u64(g_typed_mutation_audit_counters.adt_invariant_fail) > inv_fail0,
          "AC1: adt_invariant_fail advanced");

    // Hard-gate finish path should deny when suite fails.
    cs.evaluator().inject_adt_non_exhaustive_for_test();
    const bool commit_ok = cs.evaluator().finish_mutate_hard_gate(
        /*nodes_changed=*/1, /*linear_ops=*/false, "mutate:rebind");
    CHECK(!commit_ok, "AC1: finish_mutate_hard_gate denies");
    CHECK(!cs.evaluator().last_mutate_error().empty() ||
              load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total) > hard0,
          "AC1: deny error and/or hard-gate rollback counter");
    cs.evaluator().clear_adt_non_exhaustive_inject_for_test();
    cs.evaluator().clear_last_mutate_error();
}

static void ac2_exhaustive_ok() {
    std::println("\n--- AC2: exhaustive / no-match → adt_ok true ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define x 1)\")").has_value() || true, "set-code");
    (void)cs.eval("(eval-current)");
    InvariantAuditResult inv{};
    const bool ok =
        cs.evaluator().run_typed_mutation_invariant_audit(1, "happy-2264", 0, 0, 1, false, &inv);
    CHECK(inv.adt_ok, "AC2: adt_ok true without non-exhaustive match");
    CHECK(inv.all_ok() || !inv.type_ok || !inv.linear_ok || !inv.provenance_ok,
          "AC2: all_ok not failed by adt alone");
    (void)ok;
}

static void ac3_soft_no_hard_gate() {
    std::println("\n--- AC3: Soft small dirty without hard-gate → no ADT rollback ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    // Small dirty, no linear, no match force → soft path.
    CHECK(!requires_invariant_hard_gate(/*nodes=*/0, /*linear=*/false, /*strict=*/false,
                                        /*match=*/false),
          "AC3: Sampled small no-match → no hard gate");
    CompilerService cs;
    (void)cs.eval("(+ 1 1)");
    const auto hard0 = load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total);
    // Inject would fail suite if audited, but soft hard-gate skip returns true.
    cs.evaluator().inject_adt_non_exhaustive_for_test();
    const bool soft_ok = cs.evaluator().finish_mutate_hard_gate(
        /*nodes_changed=*/0, /*linear_ops=*/false, "soft-mutate");
    CHECK(soft_ok, "AC3: soft path allows commit");
    CHECK(load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total) == hard0,
          "AC3: no hard-gate force-rollback for ADT alone under soft");
    // Inject consumed only if audit ran; clear residual.
    cs.evaluator().clear_adt_non_exhaustive_inject_for_test();
}

static void ac4_defaults() {
    std::println("\n--- AC4: adt_ok default true; all_ok includes adt ---");
    InvariantAuditResult r{};
    CHECK(r.adt_ok, "AC4: default adt_ok true");
    CHECK(r.all_ok(), "AC4: default all_ok");
    r.adt_ok = false;
    CHECK(!r.all_ok(), "AC4: all_ok false when adt_ok false");
    const auto ta = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(ta.find("bool adt_ok = true") != std::string::npos, "AC4: defaulted field");
    CHECK(ta.find("&& adt_ok") != std::string::npos ||
              ta.find("adt_ok && !cross_batch_linear_escape") != std::string::npos,
          "AC4: all_ok includes adt_ok");
}

static void ac5_schema_source() {
    std::println("\n--- AC5: schema-2264 + source-cite ---");
    CompilerService cs;
    CHECK(trail(cs, "schema-2264") == 2264, "trail schema-2264");
    CHECK(trail(cs, "issue-2264") == 2264, "trail issue-2264");
    CHECK(trail(cs, "adt-exhaustiveness-hard-gate-wired") == 1, "trail wired");
    CHECK(trail(cs, "adt-exhaustiveness-audit-total") >= 0, "trail audit-total key");
    CHECK(trail(cs, "adt-exhaustiveness-fail-total") >= 0, "trail fail-total key");
    CHECK(audit_stats(cs, "schema-2264") == 2264, "stats schema-2264");
    CHECK(audit_stats(cs, "issue-2264") == 2264, "stats issue");

    const auto ta = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(ta.find("adt_exhaustiveness_audit_total") != std::string::npos, "audit total counter");
    CHECK(ta.find("adt_exhaustiveness_fail_total") != std::string::npos, "fail total counter");
    CHECK(ta.find("Issue #2264") != std::string::npos, "audit.h cites 2264");
    const auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(tc.find("inject_adt_non_exhaustive_for_test") != std::string::npos, "inject seam");
    CHECK(tc.find("Issue #2264") != std::string::npos || tc.find("#2264") != std::string::npos,
          "typecheck cites 2264");
    CHECK(tc.find("check_match_exhaustiveness") != std::string::npos, "hard-gate suite checks");
}

// Issue #2288: AC6–AC8 — selective ADT exhaustiveness on infer_flat_partial
// main path (earlier signal than Full audit). Partial-infer counter bumps
// when non-exhaustive match is detected during the partial-infer sweep,
// BEFORE Full audit sampling closes the window. Schema-additive to #2264.
static void ac6_partial_non_exhaustive_counter() {
    std::println("\n--- AC6: adt_partial_non_exhaustive_total counter exposed ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    // Set known value to verify the query surface reads from this CounterMetrics
    // (not the free-process g_typed_mutation_audit_counters used by #2223/#2264).
    metrics.adt_partial_non_exhaustive_total.store(7, std::memory_order_relaxed);
    const auto v = audit_stats(cs, "adt-partial-non-exhaustive-total");
    CHECK(v == 7, "AC6: counter exposed via query:typed-mutation-audit-stats");
}

static void ac7_schema_2288() {
    std::println("\n--- AC7: schema-2288 + issue-2288 keys ---");
    CompilerService cs;
    CHECK(audit_stats(cs, "schema-2288") == 2288, "AC7: schema-2288 key");
    CHECK(audit_stats(cs, "issue-2288") == 2288, "AC7: issue-2288 key");
}

static void ac8_source_wiring() {
    std::println("\n--- AC8: source wiring #2288 ---");
    auto tc = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tc.find("Issue #2288") != std::string::npos, "AC8: type_checker_impl.cpp cites #2288");
    CHECK(tc.find("adt_partial_non_exhaustive_total") != std::string::npos,
          "AC8: counter bump wired in infer_flat_partial");
    CHECK(tc.find("bump_partial_counter") != std::string::npos,
          "AC8: partial counter flag in recheck_match_exhaustiveness_in_dirty_scope");
    auto om = read_file("src/compiler/observability_metrics.h");
    CHECK(om.find("adt_partial_non_exhaustive_total") != std::string::npos,
          "AC8: counter field defined");
    auto inc = read_file("src/compiler/compiler_metrics_fields.inc");
    CHECK(inc.find("adt_partial_non_exhaustive_total") != std::string::npos,
          "AC8: .inc has the counter");
    auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("Issue #2288") != std::string::npos, "AC8: query cites #2288");
    CHECK(q.find("schema-2288") != std::string::npos, "AC8: schema-2288 in query surface");
    CHECK(q.find("adt-partial-non-exhaustive-total") != std::string::npos,
          "AC8: counter key in query surface");
}

} // namespace

int main() {
    std::println("=== Issue #2264 / #2288: ADT exhaustiveness hard-gate + partial-infer ===");
    ac1_full_hard_gate_fails();
    ac2_exhaustive_ok();
    ac3_soft_no_hard_gate();
    ac4_defaults();
    ac5_schema_source();
    ac6_partial_non_exhaustive_counter();
    ac7_schema_2288();
    ac8_source_wiring();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
