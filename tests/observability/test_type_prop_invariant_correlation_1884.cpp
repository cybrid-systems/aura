// @category: integration
// @reason: Issue #1884 — TypePropagation/DCE/memo ↔ TypedMutationAudit correlation
//
// AC1: query:type-propagation-invariant-stats schema 1884
// AC2: note_type_propagation_pass + invariant audit raises correlation
// AC3: pass-with-evidence rises when narrow hits + invariant ok
// AC4: evidence-lost rises when type fail after narrow evidence
// AC5: 1000 synthetic correlate loops — correlation-total == 1000

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::correlate_invariant_with_type_system;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::InvariantAuditResult;
using aura::compiler::typed_audit::note_dce_narrow_hits;
using aura::compiler::typed_audit::note_predicate_memo_eviction;
using aura::compiler::typed_audit::note_type_propagation_pass;
using aura::compiler::typed_audit::record_invariant_audit_result;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(const std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-propagation-invariant-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_query_schema(CompilerService& cs) {
    std::println("\n--- AC1: type-propagation-invariant-stats schema ---");
    auto h = cs.eval("(engine:metrics \"query:type-propagation-invariant-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1884, "schema 1884");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "correlation-total") >= 0, "correlation-total");
}

void ac2_correlation_bumps() {
    std::println("\n--- AC2: TypeProp note + invariant audit correlates ---");
    reset_for_test();
    note_type_propagation_pass(/*fixpoint=*/4, /*narrow=*/3, /*extended=*/2);
    note_dce_narrow_hits(5);
    InvariantAuditResult ok;
    ok.type_ok = true;
    ok.linear_ok = true;
    ok.provenance_ok = true;
    record_invariant_audit_result(1, "structural", ok, 0, 1);
    CHECK(load_u64(g_typed_mutation_audit_counters.type_prop_invariant_correlation_total) == 1,
          "correlation 1");
    CHECK(load_u64(g_typed_mutation_audit_counters.type_prop_invariant_pass_with_evidence_total) ==
              1,
          "pass-with-evidence 1");
}

void ac3_pass_with_evidence() {
    std::println("\n--- AC3: pass-with-evidence after narrow hits ---");
    reset_for_test();
    note_type_propagation_pass(2, 10, 1);
    InvariantAuditResult ok;
    record_invariant_audit_result(2, "structural", ok, 0, 1);
    CHECK(load_u64(g_typed_mutation_audit_counters.type_prop_invariant_pass_with_evidence_total) >=
              1,
          "pass-with-evidence");
    CHECK(load_u64(g_typed_mutation_audit_counters.type_prop_evidence_lost_total) == 0,
          "no evidence-lost on pass");
}

void ac4_evidence_lost_on_type_fail() {
    std::println("\n--- AC4: evidence-lost when type invariant fails with narrow ---");
    reset_for_test();
    note_type_propagation_pass(1, 7, 0);
    note_dce_narrow_hits(2);
    InvariantAuditResult bad;
    bad.type_ok = false;
    bad.linear_ok = true;
    bad.provenance_ok = true;
    record_invariant_audit_result(3, "structural", bad, 0, 1);
    CHECK(load_u64(g_typed_mutation_audit_counters.type_prop_invariant_fail_with_evidence_total) >=
              1,
          "fail-with-evidence");
    CHECK(load_u64(g_typed_mutation_audit_counters.type_prop_evidence_lost_total) >= 1,
          "evidence-lost");
    // memo eviction under fail correlates
    note_predicate_memo_eviction(3);
    CHECK(load_u64(g_typed_mutation_audit_counters.predicate_memo_evict_correlated_total) >= 3,
          "memo-evict-correlated");
}

void ac5_high_frequency_loop() {
    std::println("\n--- AC5: 1000 correlate loops ---");
    reset_for_test();
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        note_type_propagation_pass(1 + (i % 4), 1 + (i % 8), i % 3);
        note_dce_narrow_hits(i % 5);
        InvariantAuditResult r;
        r.type_ok = (i % 11) != 0;
        r.linear_ok = true;
        r.provenance_ok = true;
        correlate_invariant_with_type_system(r);
        if ((i % 17) == 0)
            note_predicate_memo_eviction(1);
    }
    CHECK(load_u64(g_typed_mutation_audit_counters.type_prop_invariant_correlation_total) ==
              static_cast<std::uint64_t>(N),
          "correlation-total 1000");
    const auto pass_ev =
        load_u64(g_typed_mutation_audit_counters.type_prop_invariant_pass_with_evidence_total);
    const auto fail_ev =
        load_u64(g_typed_mutation_audit_counters.type_prop_invariant_fail_with_evidence_total);
    CHECK(pass_ev + fail_ev == static_cast<std::uint64_t>(N), "pass+fail evidence partitions N");
    CHECK(pass_ev > fail_ev, "majority pass-with-evidence under mostly-ok loop");
}

void ac6_query_reflects_counters(CompilerService& cs) {
    std::println("\n--- AC6: query surfaces correlation ---");
    // Leave counters from ac5
    auto h = cs.eval("(engine:metrics \"query:type-propagation-invariant-stats\")");
    CHECK(h && is_hash(*h), "hash after loop");
    CHECK(href(cs, "correlation-total") == 1000, "query correlation-total");
    CHECK(href(cs, "pass-with-evidence") > 0, "query pass-with-evidence");
    CHECK(href(cs, "evidence-pass-correlation-bp") >= 0, "bp");
}

} // namespace

int main() {
    std::println("=== Issue #1884: TypeProp/memo ↔ invariant correlation ===");
    CompilerService cs;
    ac1_query_schema(cs);
    ac2_correlation_bumps();
    ac3_pass_with_evidence();
    ac4_evidence_lost_on_type_fail();
    ac5_high_frequency_loop();
    ac6_query_reflects_counters(cs);
    std::println("\n=== #1884: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
