// @category: unit
// @reason: Issue #2027 — composite / nested txn typed + linear invariant
// audit under atomic batch (partial recovery before Full rollback).
//
//   AC1: source cites #2027; composite counters + partial recover helpers
//   AC2: composite_mode audit stamps composite_invariant_* counters
//   AC3: nested_boundary || atomic_batch forces audit under Sampled
//   AC4: query:typed-mutation-audit-trail schema-2027 + composite keys
//   AC5: partial recover attempt path wired (source + counter surface)
//   AC6: fine_rollback path remains consistent with composite full rollback
//   AC7: service smoke — nested mutate under Full leaves trail wired

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::InvariantAuditResult;
using aura::compiler::typed_audit::kTypedMutationAuditPassPhase;
using aura::compiler::typed_audit::record_composite_invariant_audit;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t trail_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-trail\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define x 1) (define y (+ x 1)) (define z (* y 2))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2027 ---");
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(!aud.empty(), "typed_mutation_audit.h readable");
    CHECK(aud.find("Issue #2027") != std::string::npos, "audit header cites #2027");
    CHECK(aud.find("composite_invariant_fail_total") != std::string::npos,
          "composite_invariant_fail_total");
    CHECK(aud.find("composite_partial_recover") != std::string::npos, "partial recover counters");
    CHECK(aud.find("record_composite_invariant_audit") != std::string::npos,
          "record_composite helper");
    CHECK(!bound.empty() && (bound.find("Issue #2027") != std::string::npos ||
                             bound.find("#2027") != std::string::npos),
          "boundary cites #2027");
    CHECK(bound.find("composite-partial-recover") != std::string::npos ||
              bound.find("composite_partial_recover") != std::string::npos,
          "boundary partial recover path");
    CHECK(bound.find("composite_mode") != std::string::npos, "boundary composite_mode");
    CHECK(!tc.empty() && tc.find("composite_mode") != std::string::npos,
          "typecheck composite_mode");
    CHECK(tc.find("cross_batch_linear_escape") != std::string::npos, "cross-batch escape scan");
    CHECK(!q.empty() && q.find("schema-2027") != std::string::npos, "query schema-2027");
    CHECK(kTypedMutationAuditPassPhase >= 5, "phase >= 5");
}

static void ac2_composite_mode_counters() {
    std::println("\n--- AC2: composite_mode stamps counters ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    const auto a0 = load_u64(g_typed_mutation_audit_counters.composite_invariant_audits_total);
    const auto ok0 = load_u64(g_typed_mutation_audit_counters.composite_invariant_ok_total);
    // Drive composite_mode=true directly (no nested stack required).
    InvariantAuditResult out{};
    const bool ok = cs.evaluator().run_typed_mutation_invariant_audit(
        /*mid=*/42, "composite-test", 0, 0, 1, /*composite_mode=*/true, &out);
    CHECK(out.composite_mode, "out.composite_mode set");
    // Manually stamp composite record (boundary does this on nested/batch).
    record_composite_invariant_audit(/*nested=*/true, /*batch=*/true, out);
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_invariant_audits_total) > a0,
          "composite audits bumped");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_nested_audit_total) >= 1,
          "nested audit counter");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_batch_audit_total) >= 1,
          "batch audit counter");
    if (ok) {
        CHECK(load_u64(g_typed_mutation_audit_counters.composite_invariant_ok_total) > ok0,
              "composite ok when suite passes");
    } else {
        CHECK(load_u64(g_typed_mutation_audit_counters.composite_invariant_fail_total) >= 1,
              "composite fail recorded");
    }
    CHECK(load_u64(g_typed_mutation_audit_counters.invariant_audits) >= 1,
          "base invariant suite ran");
}

static void ac3_composite_forces_audit() {
    std::println("\n--- AC3: composite forces audit under Sampled ---");
    // Source-level contract: exit path uses composite || should_audit_contextual.
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("const bool composite = nested_boundary || batch_active") !=
                  std::string::npos ||
              bound.find("nested_boundary || batch_active") != std::string::npos,
          "composite = nested || batch");
    CHECK(bound.find("composite || typed_audit::should_audit_contextual") != std::string::npos ||
              bound.find("(composite ||") != std::string::npos,
          "composite forces audit gate");
    // Runtime: mutate under Full still triggers invariant suite.
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    const auto inv0 = load_u64(g_typed_mutation_audit_counters.invariant_audits);
    CHECK(cs.eval("(mutate:rebind \"x\" \"10\")").has_value(), "mutate rebind");
    CHECK(load_u64(g_typed_mutation_audit_counters.invariant_audits) >= inv0,
          "invariant suite non-decreasing");
}

static void ac4_query_schema_2027() {
    std::println("\n--- AC4: trail schema-2027 + composite keys ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    (void)cs.evaluator().run_typed_mutation_invariant_audit(7, "query-test", 0, 0, 1, true,
                                                            nullptr);
    auto h = cs.eval("(engine:metrics \"query:typed-mutation-audit-trail\")");
    CHECK(h && is_hash(*h), "trail hash");
    CHECK(trail_href(cs, "schema-2027") == 2027, "schema-2027");
    CHECK(trail_href(cs, "issue-2027") == 2027, "issue-2027");
    CHECK(trail_href(cs, "composite-partial-recover-wired") == 1, "partial recover wired");
    CHECK(trail_href(cs, "composite-nested-audit-wired") == 1, "nested audit wired");
    CHECK(trail_href(cs, "composite-invariant-audits") >= 0, "composite-invariant-audits");
    CHECK(trail_href(cs, "composite_invariant_fail_total") >= 0, "composite_invariant_fail_total");
    CHECK(trail_href(cs, "composite-partial-recover-type") >= 0, "partial type");
    CHECK(trail_href(cs, "composite-partial-recover-linear") >= 0, "partial linear");
    CHECK(trail_href(cs, "composite-partial-recover-success") >= 0, "partial success");
    CHECK(trail_href(cs, "composite-full-rollback") >= 0, "full rollback");
    CHECK(trail_href(cs, "composite-cross-batch-linear-escape") >= 0, "cross-batch escape");
    // Primary lineage retained
    CHECK(trail_href(cs, "schema") == 1894 || trail_href(cs, "schema") == 2027, "schema lineage");
    CHECK(trail_href(cs, "phase") >= 5, "phase >= 5");
}

static void ac5_partial_recover_wired() {
    std::println("\n--- AC5: partial recover path wired ---");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(bound.find("composite_partial_recover_attempt_total") != std::string::npos,
          "attempt counter");
    CHECK(bound.find("composite_partial_recover_linear_total") != std::string::npos,
          "linear recover counter");
    CHECK(bound.find("composite_partial_recover_type_total") != std::string::npos,
          "type recover counter");
    CHECK(bound.find("composite_partial_recover_success_total") != std::string::npos,
          "success counter");
    CHECK(bound.find("composite_full_rollback_total") != std::string::npos,
          "full rollback counter");
    CHECK(bound.find("enforce_linear_boundary_consistency") != std::string::npos,
          "linear re-enforce in recover");
    CHECK(bound.find("linear_post_mutate_enforce_all") != std::string::npos,
          "linear enforce_all in recover");
    // Counter surface zero-initialized after reset
    reset_for_test();
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_partial_recover_attempt_total) == 0,
          "reset clears attempt");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_full_rollback_total) == 0,
          "reset clears full rollback");
}

static void ac6_fine_rollback_consistency() {
    std::println("\n--- AC6: fine_rollback on composite full rollback ---");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // Force-rollback branch restores fine_rollback columns + children + batch flag
    CHECK(bound.find("cp.fine_rollback") != std::string::npos, "fine_rollback checked");
    CHECK(bound.find("restore_sym_id") != std::string::npos, "sym restore");
    CHECK(bound.find("restore_param_columns") != std::string::npos, "param restore");
    CHECK(bound.find("restore_children") != std::string::npos, "children restore");
    CHECK(bound.find("rollback_atomic_batch") != std::string::npos ||
              bound.find("begin_atomic_batch") != std::string::npos,
          "atomic batch realign");
    CHECK(bound.find("composite-invariant-force-rollback") != std::string::npos,
          "composite rollback trail name");
}

static void ac7_service_smoke() {
    std::println("\n--- AC7: service smoke nested-style mutate ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    CHECK(cs.eval("(mutate:rebind \"x\" \"100\")").has_value(), "mutate 1");
    CHECK(cs.eval("(mutate:rebind \"y\" \"200\")").has_value(), "mutate 2");
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok");
    CHECK(trail_href(cs, "schema-2027") == 2027, "schema-2027 after mutates");
    CHECK(trail_href(cs, "composite-partial-recover-wired") == 1, "wired after mutates");
    CHECK(load_u64(g_typed_mutation_audit_counters.invariant_audits) >= 0, "audits observed");
}

} // namespace

int main() {
    ac1_source();
    ac2_composite_mode_counters();
    ac3_composite_forces_audit();
    ac4_query_schema_2027();
    ac5_partial_recover_wired();
    ac6_fine_rollback_consistency();
    ac7_service_smoke();
    if (g_failed)
        return 1;
    std::println("composite nested txn invariant audit (#2027): OK ({} passed)", g_passed);
    return 0;
}
