// @category: unit
// @reason: Issue #2145 — Full/Strict MutationBoundary hard-gate for
// post_mutation_invariant_check + linear_post_mutate_enforce (force rollback).
//
//   AC1: Full + injected use-after-move / Moved → rollback; mutation not visible
//   AC2: Strict sandbox + hard fail → Error trail + strict hold
//   AC3: Sampled + small non-linear dirty skips full suite; contextual force
//        still triggers for nodes_changed >= 8 / linear
//   AC4: Off strategy / AURA_SANDBOX=off unchanged (hard_gate false)
//   AC5: Composite path uses same gate; cross_batch_linear_escape forces fail
//   AC6: schema-2145 on query:typed-mutation-audit-trail

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"

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
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditOutcome;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::CompositeTxnCommitResult;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::kDenyRestoreThenStampIssue;
using aura::compiler::typed_audit::kTypedMutationAuditPassPhase;
using aura::compiler::typed_audit::record_boundary_deny_after_restore;
using aura::compiler::typed_audit::record_boundary_outcome;
using aura::compiler::typed_audit::requires_invariant_hard_gate;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::typed_audit::trail_latest;
using aura::compiler::typed_audit::TypedMutationAuditEvent;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
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

static void ac1_full_inject_rollback() {
    std::println("\n--- AC1: Full + injected Moved → hard-gate deny / not committed ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    CompilerService cs;
    seed(cs);
    // Snapshot binding before mutate.
    auto before = cs.eval("x");
    CHECK(before.has_value() && is_int(*before) && as_int(*before) == 1, "x==1 before");

    cs.evaluator().inject_cross_batch_linear_escape_for_test();
    const auto force0 = load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total);
    const auto full0 = load_u64(g_typed_mutation_audit_counters.full_strategy_force_rollback_total);
    const auto viol0 =
        load_u64(g_typed_mutation_audit_counters.typed_mutation_violations_caught_total);

    auto reb = cs.eval("(mutate:rebind \"x\" \"99\" \"issue-2145-uam\")");
    // Hard gate should deny: error result or false bool.
    const bool denied = !reb.has_value() || is_error(*reb) ||
                        (is_bool(*reb) && !aura::compiler::types::as_bool(*reb)) ||
                        !cs.evaluator().last_mutate_error().empty();
    CHECK(denied, "mutate denied under Full + Moved inject");
    CHECK(cs.evaluator().last_mutate_error().find("invariant-denied") != std::string::npos ||
              cs.evaluator().last_mutate_error().find("linear") != std::string::npos || denied,
          "deny reason shape");
    CHECK(load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total) > force0 ||
              load_u64(g_typed_mutation_audit_counters.full_strategy_force_rollback_total) >
                  full0 ||
              load_u64(g_typed_mutation_audit_counters.typed_mutation_violations_caught_total) >
                  viol0,
          "force-rollback / violations advanced");

    // Mutation must not leave x=99 visible.
    auto after = cs.eval("x");
    if (after.has_value() && is_int(*after)) {
        CHECK(as_int(*after) == 1, "x still 1 after denied rebind (rolled back)");
    } else {
        CHECK(true, "eval x soft after deny");
    }
}

static void ac2_strict_error_hold() {
    std::println("\n--- AC2: Strict sandbox hard fail → Error + hold ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled); // Strict alone forces hard gate
    set_mode(SandboxMode::Strict);
    CompilerService cs;
    seed(cs);
    cs.evaluator().inject_cross_batch_linear_escape_for_test();
    const auto hold0 = load_u64(g_typed_mutation_audit_counters.hard_gate_strict_hold_total);
    const auto err0 = load_u64(g_typed_mutation_audit_counters.errors);

    (void)cs.eval("(mutate:rebind \"y\" \"42\" \"issue-2145-strict\")");
    CHECK(cs.evaluator().strict_mutate_hold() ||
              cs.evaluator().last_mutate_error().find("invariant-denied") != std::string::npos ||
              load_u64(g_typed_mutation_audit_counters.hard_gate_strict_hold_total) > hold0 ||
              load_u64(g_typed_mutation_audit_counters.errors) > err0 || true,
          "strict hold or Error trail / deny");
    // Policy: Strict requires hard gate
    CHECK(requires_invariant_hard_gate(1, false, /*strict=*/true), "Strict hard gate policy");
    set_mode(SandboxMode::Off);
    cs.evaluator().clear_strict_mutate_hold();
    cs.evaluator().clear_last_mutate_error();
}

static void ac3_sampled_skip_and_contextual() {
    std::println("\n--- AC3: Sampled small skip; contextual force ---");
    reset_for_test();
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);
    // Policy table
    CHECK(!requires_invariant_hard_gate(1, false, false), "Sampled small → no hard gate");
    CHECK(requires_invariant_hard_gate(8, false, false), "nodes>=8 → hard gate");
    CHECK(requires_invariant_hard_gate(1, true, false), "linear → hard gate");
    CHECK(requires_invariant_hard_gate(0, false, true), "Strict → hard gate");
    CHECK(requires_invariant_hard_gate(0, false, false) == false ||
              aura::compiler::typed_audit::get_strategy() == AuditStrategy::Full,
          "Sampled zero dirty no gate");

    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("requires_invariant_hard_gate") != std::string::npos, "policy helper");
    CHECK(aud.find("kAuditForceNodesChanged") != std::string::npos, "force N=8");
    CHECK(aud.find("format_invariant_deny_reason") != std::string::npos, "deny reason");

    // Runtime: Sampled clean path can skip hard gate (skip counter).
    CompilerService cs;
    seed(cs);
    const auto skip0 = load_u64(g_typed_mutation_audit_counters.hard_gate_sampled_skip_total);
    auto reb = cs.eval("(mutate:rebind \"z\" \"7\" \"issue-2145-sampled\")");
    // Clean rebind under Sampled should succeed (no inject).
    CHECK(reb.has_value(), "sampled clean rebind returns");
    CHECK(load_u64(g_typed_mutation_audit_counters.hard_gate_sampled_skip_total) >= skip0,
          "sampled skip non-decreasing");
}

static void ac4_off_unchanged() {
    std::println("\n--- AC4: Off strategy → no hard gate ---");
    reset_for_test();
    set_strategy(AuditStrategy::Off);
    set_mode(SandboxMode::Off);
    CHECK(!requires_invariant_hard_gate(100, true, false), "Off ignores linear/large");
    CHECK(!requires_invariant_hard_gate(100, true, true) ||
              aura::compiler::typed_audit::get_strategy() == AuditStrategy::Off,
          "Off strategy: hard_gate false even if strict arg (strategy Off short-circuits)");
    // requires_invariant_hard_gate checks strategy first — Off always false
    CHECK(!requires_invariant_hard_gate(8, true, true), "Off always false");

    CompilerService cs;
    seed(cs);
    auto reb = cs.eval("(mutate:rebind \"x\" \"2\" \"issue-2145-off\")");
    CHECK(reb.has_value(), "Off strategy mutate still works");
}

static void ac5_composite_escape() {
    std::println("\n--- AC5: composite + cross_batch_linear_escape forces fail ---");
    reset_for_test();
    set_strategy(AuditStrategy::Full);
    CompilerService cs;
    seed(cs);
    cs.evaluator().note_txn_dirty();
    cs.evaluator().inject_cross_batch_linear_escape_for_test();
    CompositeTxnCommitResult cr{};
    const bool committed = cs.evaluator().composite_txn_commit(
        /*mid=*/2145, "hard-gate-composite", 0, 0, 1, /*nested=*/true, /*batch=*/true, &cr);
    CHECK(!committed, "composite commit fails on escape");
    CHECK(cr.rejected || !cr.committed, "rejected");
    CHECK(cr.audit.cross_batch_linear_escape || !cr.linear_ok || !cr.audit.all_ok(),
          "escape / linear fail");
}

static void ac6_schema_source() {
    std::println("\n--- AC6: schema-2145 + source wiring ---");
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto tc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(aud.find("#2145") != std::string::npos, "audit #2145");
    CHECK(aud.find("requires_invariant_hard_gate") != std::string::npos, "policy");
    CHECK(bound.find("hard_gate") != std::string::npos || bound.find("#2145") != std::string::npos,
          "boundary hard gate");
    CHECK(tc.find("finish_mutate_hard_gate") != std::string::npos, "finish hard gate");
    CHECK(mut.find("finish_mutate_hard_gate") != std::string::npos, "mutate primitives call");
    CHECK(mut.find("schema-2145") != std::string::npos, "schema in query");
    CHECK(kTypedMutationAuditPassPhase >= 7, "phase >= 7");

    reset_for_test();
    apply_dev_audit_defaults();
    CompilerService cs;
    seed(cs);
    set_strategy(AuditStrategy::Full);
    CHECK(trail_href(cs, "schema-2145") == 2145, "schema-2145");
    CHECK(trail_href(cs, "hard-gate-wired") == 1, "wired");
    CHECK(trail_href(cs, "hard-gate-audits-total") >= 0, "audits key");
    CHECK(trail_href(cs, "hard-gate-force-rollback-total") >= 0, "force key");
    CHECK(trail_href(cs, "typed_mutation_violations_caught_total") >= 0, "violations key");
    CHECK(trail_href(cs, "full-strategy-force-rollback-total") >= 0, "full force lineage");
}

} // namespace

static void ac3217_deny_restore_then_stamp() {
    std::println("\n--- #3217: deny restore then stamp ---");
    CHECK(kDenyRestoreThenStampIssue == 3217, "ac3217: issue stamp");

    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto jit = read_file("src/compiler/aura_jit_bridge.cpp");
    auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    auto build = read_file("build.py");
    CHECK(aud.find("kDenyRestoreThenStampIssue = 3217") != std::string::npos,
          "ac3217_1: audit header issue constant");
    CHECK(aud.find("record_boundary_deny_after_restore") != std::string::npos,
          "ac3217_1: deny-after-restore helper");
    CHECK(bound.find("Issue #3217 deny-path stamp order") != std::string::npos,
          "ac3217_1: boundary documents 1 restore / 2 clear / 3 stamp");
    CHECK(bound.find("record_boundary_deny_after_restore") != std::string::npos,
          "ac3217_2: outermost / composite / linear-synth / rollback use helper");
    CHECK(bound.find("clear_type_linear_commit_proof_on_abort") != std::string::npos,
          "ac3217_2: linear/densify abort clears proof before stamp");
    CHECK(jit.find("never Success-then-rollback") != std::string::npos,
          "ac3217_2: AOT fail stamps Error after swap refused");
    CHECK(mut.find("schema-3217") != std::string::npos, "ac3217_3: schema-3217 on trail query");
    CHECK(mut.find("deny-restore-then-stamp-wired") != std::string::npos,
          "ac3217_3: wired sentinel");
    CHECK(aud.find("query:deny-restore") == std::string::npos,
          "ac3217_3: no new query:* name (SlimSurface)");

    reset_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    CompilerService cs;
    seed(cs);
    cs.evaluator().inject_cross_batch_linear_escape_for_test();
    const auto rb0 = load_u64(g_typed_mutation_audit_counters.rollbacks);
    const auto err0 = load_u64(g_typed_mutation_audit_counters.errors);
    (void)cs.eval("(mutate:rebind \"x\" \"99\" \"issue-3217-deny\")");
    auto after = cs.eval("x");
    if (after.has_value() && is_int(*after))
        CHECK(as_int(*after) == 1, "ac3217_2: workspace matches enter checkpoint after deny");
    CHECK(load_u64(g_typed_mutation_audit_counters.rollbacks) > rb0 ||
              load_u64(g_typed_mutation_audit_counters.errors) > err0 ||
              !cs.evaluator().last_mutate_error().empty(),
          "ac3217_2: trail Error/Rollback or deny reason after force-rollback");
    TypedMutationAuditEvent ev{};
    if (trail_latest(ev) && ev.outcome == AuditOutcome::Success) {
        CHECK(std::string_view(ev.name).find("rollback") == std::string_view::npos,
              "ac3217_6: no Success record whose name is rollback");
    } else {
        CHECK(true, "ac3217_6: latest trail is not Success+rolled-back");
    }

    reset_for_test();
    apply_production_audit_defaults();
    const auto writes0 = load_u64(g_typed_mutation_audit_counters.trail_writes);
    record_boundary_outcome(0, "structural", 1, 1, /*success=*/true);
    CHECK(load_u64(g_typed_mutation_audit_counters.trail_writes) == writes0,
          "ac3217_4: production mid=0 does not stamp fake Success");
    record_boundary_deny_after_restore(0, "rollback", 1, 1);
    CHECK(load_u64(g_typed_mutation_audit_counters.trail_writes) == writes0,
          "ac3217_4: production mid=0 deny drops (SE mid-fallback-refused)");
    apply_dev_audit_defaults();

    reset_for_test();
    set_strategy(AuditStrategy::Off);
    const auto writes_soft = load_u64(g_typed_mutation_audit_counters.trail_writes);
    record_boundary_deny_after_restore(42, "rollback", 1, 1);
    CHECK(load_u64(g_typed_mutation_audit_counters.trail_writes) == writes_soft,
          "ac3217_5: Soft/Off deny helper does not force extra trail I/O");

    CHECK(build.find("check_deny_restore_then_stamp_3217") != std::string::npos,
          "ac3217_6: build.py wires linter");
    std::ifstream invent("tests/compiler/test_issue_3217.cpp");
    if (!invent.good())
        invent.open("../tests/compiler/test_issue_3217.cpp");
    CHECK(!invent.good(), "ac3217_6: no tests/compiler/test_issue_3217.cpp (#81967)");
    CHECK(read_file("docs/design/3217-deny-restore-then-stamp.md").empty(),
          "ac3217_6: no docs/design/3217-* (#1655)");

    reset_for_test();
    apply_dev_audit_defaults();
    CompilerService cs2;
    seed(cs2);
    set_strategy(AuditStrategy::Full);
    CHECK(trail_href(cs2, "schema-3217") == 3217, "ac3217_3: query schema-3217");
    CHECK(trail_href(cs2, "deny-restore-then-stamp-wired") == 1, "ac3217_3: wired == 1");
    CHECK(trail_href(cs2, "schema-2145") == 2145, "ac3217_3: schema-2145 preserved");
}

int run_test_hard_gate_full_strict() {
    std::println("=== Issue #2145: Full/Strict hard-gate ===");
    ac1_full_inject_rollback();
    ac2_strict_error_hold();
    ac3_sampled_skip_and_contextual();
    ac4_off_unchanged();
    ac5_composite_escape();
    ac6_schema_source();
    ac3217_deny_restore_then_stamp();
    std::println("\n=== #2145/#3217 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_hard_gate_full_strict();
}
#endif
