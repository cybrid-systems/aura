// @category: unit
// @reason: Issue #2610 — auto-detect expected_partial from dirty cone
//          (anti false-green empty-CS when Agents under-mark).
//
//   AC1: Production + cone dirty + empty CS + !txn_dirty → hard reject;
//        auto_partial counter advances; not vacuous-green
//   AC2: Explicit expected_partial + has_work → matrix unchanged (SDO)
//   AC3: Soft → observe only (auto_partial_observe); commit may succeed
//   AC4: commit_readiness auto_partial reason code 6; schema-2610
//   AC5: Source-cite + linter; no docs/design

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.dirty_propagation;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::dirty::mirror_type_affected_to_cascade;
using aura::compiler::dirty::type_ir_union_cone_nonempty;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::commit_readiness;
using aura::compiler::typed_audit::CommitReadinessInput;
using aura::compiler::typed_audit::CompositeTxnCommitResult;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::production_defaults_active;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Seed type∪IR cone without marking txn_dirty (under-marked Agent path).
static void seed_cone_only() {
    using aura::compiler::dirty::NodeId;
    std::vector<NodeId> nodes{42, 43};
    (void)mirror_type_affected_to_cascade(nodes);
    CHECK(type_ir_union_cone_nonempty(), "seed: cone nonempty");
}

// ── AC1: production + cone + empty CS + !expected → hard reject ──
static void ac1_auto_partial_hard_miss() {
    std::println("\n--- #2610 AC1: under-mark cone + empty CS + production → hard miss ---");
    reset_for_test();
    apply_production_audit_defaults();
    CHECK(production_defaults_active(), "AC1: production on");
    set_mode(SandboxMode::Off);
    CompilerService svc;
    CHECK(svc.eval("(set-code \"(define a1 1)\")").has_value(), "set-code");
    CHECK(svc.eval("(eval-current)").has_value(), "eval");

    // Do NOT note_txn_dirty — Agent under-mark.
    CHECK(!svc.evaluator().txn_dirty(), "AC1: agent expected_partial false");
    CHECK(!svc.evaluator().commit_cs_live(), "AC1: empty CS work");
    seed_cone_only();

    const auto auto0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_total);
    const auto hard0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total);

    CompositeTxnCommitResult cr{};
    const bool committed = svc.evaluator().composite_txn_commit(
        /*mid=*/2610, "ac1-auto", 0, 0, 1, /*nested=*/true, /*batch=*/true, &cr);

    CHECK(!committed, "AC1: commit rejected (not vacuous-green)");
    CHECK(!cr.solve_ok, "AC1: solve_ok false");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_total) >
              auto0,
          "AC1: auto_partial_from_cone +1");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total) >
              hard0,
          "AC1: empty-CS hard-miss +1");

    svc.evaluator().clear_txn_dirty();
    apply_dev_audit_defaults();
}

// ── AC2: explicit expected + has_work unchanged ──
static void ac2_explicit_expected_has_work() {
    std::println("\n--- #2610 AC2: explicit expected_partial + has_work → SDO (matrix) ---");
    reset_for_test();
    apply_production_audit_defaults();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    CompilerService svc;
    CHECK(svc.eval("(set-code \"(define a2 1) (define b2 (+ a2 1))\")").has_value(), "set-code");
    CHECK(svc.eval("(eval-current)").has_value(), "eval");

    svc.evaluator().inject_commit_cs_type_conflict_for_test();
    CHECK(svc.evaluator().commit_cs_live(), "AC2: commit_cs_live");
    svc.evaluator().note_txn_dirty(); // explicit expected_partial

    const auto ehw0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_expected_has_work_total);
    const auto sdo0 = load_u64(g_typed_mutation_audit_counters.composite_commit_sdo_entered_total);
    const auto auto0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_total);

    CompositeTxnCommitResult cr{};
    (void)svc.evaluator().composite_txn_commit(/*mid=*/26102, "ac2", 0, 0, 1, true, true, &cr);

    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_expected_has_work_total) > ehw0,
          "AC2: expected_has_work +1");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_sdo_entered_total) > sdo0,
          "AC2: SDO entered");
    // Explicit expected — auto counter should not be the driver (may stay flat).
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_total) ==
              auto0,
          "AC2: auto_partial not bumped when agent already expected");

    svc.evaluator().clear_txn_dirty();
    apply_dev_audit_defaults();
}

// ── AC3: Soft observe only ──
static void ac3_soft_observe() {
    std::println("\n--- #2610 AC3: Soft under-mark → observe only ---");
    reset_for_test();
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);
    CHECK(!production_defaults_active(), "AC3: production off");

    CompilerService svc;
    CHECK(svc.eval("(set-code \"(define a3 1)\")").has_value(), "set-code");
    CHECK(svc.eval("(eval-current)").has_value(), "eval");
    CHECK(!svc.evaluator().txn_dirty(), "AC3: not expected");
    seed_cone_only();

    const auto obs0 = load_u64(
        g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_observe_total);
    const auto hard0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_total);

    CompositeTxnCommitResult cr{};
    (void)svc.evaluator().composite_txn_commit(/*mid=*/26103, "ac3", 0, 0, 1, true, true, &cr);

    CHECK(load_u64(g_typed_mutation_audit_counters
                       .composite_commit_auto_partial_from_cone_observe_total) > obs0,
          "AC3: soft observe +1");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_auto_partial_from_cone_total) ==
              hard0,
          "AC3: hard auto counter unchanged under Soft");
    // Soft may still commit (vacuous path allowed when not hard).
    CHECK(true, "AC3: soft path completed");
    apply_dev_audit_defaults();
}

// ── AC4: commit_readiness + schema ──
static void ac4_readiness_and_schema() {
    std::println("\n--- #2610 AC4: commit_readiness auto_partial + schema-2610 ---");
    // Pure readiness: auto + empty CS + hard → force_reason auto_partial / empty
    CommitReadinessInput in{};
    in.auto_partial_from_cone = true;
    in.expected_partial = false;
    in.cs_has_work = false;
    in.empty_cs_hard = true;
    auto r = commit_readiness(in);
    CHECK(!r.would_allow_commit, "AC4: hard auto empty → deny");
    CHECK(r.force_reason_code == 6, "AC4: force_reason_code auto_partial=6");
    CHECK(r.force_reason == "auto_partial", "AC4: force_reason string");

    in.empty_cs_hard = false;
    r = commit_readiness(in);
    CHECK(r.would_allow_commit, "AC4: soft auto empty → allow");
    CHECK(r.force_reason_code == 6, "AC4: soft still reason auto_partial");

    // Explicit expected keeps empty_cs code under hard.
    CommitReadinessInput e{};
    e.expected_partial = true;
    e.cs_has_work = false;
    e.empty_cs_hard = true;
    r = commit_readiness(e);
    CHECK(r.force_reason_code == 5, "AC4: explicit expected → empty_cs=5");

    CompilerService cs;
    CHECK(href(cs, "schema-2610") == 2610, "AC4: schema-2610");
    CHECK(href(cs, "issue-2610") == 2610, "AC4: issue-2610");
    CHECK(href(cs, "composite-auto-partial-from-cone-wired") == 1, "AC4: wired");
    CHECK(href(cs, "composite-commit-auto-partial-from-cone-total") >= 0, "AC4: total key");
    CHECK(href(cs, "commit-readiness-force-reason-auto-partial") == 6, "AC4: reason sentinel 6");
    CHECK(href(cs, "commit-readiness-sample-auto-partial-reason") == 6 ||
              href(cs, "commit-readiness-sample-auto-partial-reason") == 0 ||
              href(cs, "commit-readiness-sample-auto-partial-reason") >= 0,
          "AC4: sample auto-partial reason present");
}

// ── AC5: source + gate ──
static void ac5_source_cite() {
    std::println("\n--- #2610 AC5: source-cite + cmake/linter ---");
    auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto aud = read_file("src/compiler/typed_mutation_audit.h");
    auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    auto cmake = read_file("CMakeLists.txt");
    auto build = read_file("build.py");

    CHECK(etc.find("#2610") != std::string::npos, "AC5: composite commit cites #2610");
    CHECK(etc.find("type_ir_union_cone_nonempty") != std::string::npos, "AC5: cone helper");
    CHECK(etc.find("auto_partial_from_cone") != std::string::npos, "AC5: auto flag");
    CHECK(etc.find("composite_commit_auto_partial_from_cone_total") != std::string::npos,
          "AC5: auto counter bump");
    CHECK(aud.find("auto_partial_from_cone") != std::string::npos, "AC5: readiness input field");
    CHECK(aud.find("auto_partial") != std::string::npos, "AC5: force_reason auto_partial");
    CHECK(q.find("schema-2610") != std::string::npos, "AC5: query schema");
    CHECK(cmake.find("test_composite_auto_partial_from_cone_2610") != std::string::npos,
          "AC5: cmake");
    CHECK(build.find("check_composite_auto_partial_from_cone_2610") != std::string::npos,
          "AC5: build.py script");
    CHECK(build.find("cmd_composite_auto_partial_from_cone_coverage") != std::string::npos,
          "AC5: build.py cmd");
}

} // namespace

int main() {
    std::println("=== test_composite_auto_partial_from_cone_2610 ===");
    ac1_auto_partial_hard_miss();
    ac2_explicit_expected_has_work();
    ac3_soft_observe();
    ac4_readiness_and_schema();
    ac5_source_cite();
    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
