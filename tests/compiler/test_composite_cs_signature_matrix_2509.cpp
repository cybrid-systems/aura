// @category: unit
// @reason: Issue #2509 — symmetric expected_partial ↔ commit_cs_has_work
//          hard-gate matrix at composite_txn_commit (anti false-green /
//          false-empty). Extends #2345 empty-CS lineage.
//
//   AC1: expected_partial + empty CS → hard-miss under production (#2345)
//   AC2: expected_partial + has_work → SDO entered; no vacuous SOLVED skip
//   AC3: !expected + !has_work → structural commit allowed (no type solve req)
//   AC4: !expected + has_work → unexpected_cs_work observe; SDO still runs
//   AC5: source-cite + schema-2509; all 4 matrix cells covered

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
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
using aura::compiler::typed_audit::AuditStrategy;
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

// ── AC1: expected + empty → hard-miss (no regression #2345) ──
static void ac1_expected_empty_hard_miss() {
    std::println("\n--- #2509 AC1: expected_partial + empty CS → hard-miss ---");
    reset_for_test();
    apply_production_audit_defaults();
    CHECK(production_defaults_active(), "production on");
    set_mode(SandboxMode::Off);
    CompilerService svc;
    CHECK(svc.eval("(set-code \"(define a1 1)\")").has_value(), "set-code");
    CHECK(svc.eval("(eval-current)").has_value(), "eval");

    svc.evaluator().note_txn_dirty(); // expected_partial
    CHECK(svc.evaluator().txn_dirty(), "AC1: expected_partial");
    CHECK(!svc.evaluator().commit_cs_live(), "AC1: no live CS work");

    const auto hard0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total);
    const auto sdo0 = load_u64(g_typed_mutation_audit_counters.composite_commit_sdo_entered_total);

    CompositeTxnCommitResult cr{};
    const bool committed = svc.evaluator().composite_txn_commit(
        /*mid=*/2509, "ac1-empty", 0, 0, 1, /*nested=*/true, /*batch=*/true, &cr);

    CHECK(!committed, "AC1: commit rejected");
    CHECK(!cr.solve_ok, "AC1: solve_ok false (anti false-green)");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total) >
              hard0,
          "AC1: hard-miss +1");
    // Empty greenfield still runs SDO on scratch CS, then hard-miss flips solve_ok.
    (void)sdo0;
    svc.evaluator().clear_txn_dirty();
    apply_dev_audit_defaults();
}

// ── AC2: expected + has_work → SDO entered ──
static void ac2_expected_has_work_sdo() {
    std::println("\n--- #2509 AC2: expected_partial + has_work → SDO entered ---");
    reset_for_test();
    apply_production_audit_defaults();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    CompilerService svc;
    CHECK(svc.eval("(set-code \"(define a2 1) (define b2 (+ a2 1))\")").has_value(), "set-code");
    CHECK(svc.eval("(eval-current)").has_value(), "eval");

    // Seed live commit CS via conflict inject (has_work + commit_cs_live).
    svc.evaluator().inject_commit_cs_type_conflict_for_test();
    CHECK(svc.evaluator().commit_cs_live(), "AC2: commit_cs_live");
    svc.evaluator().note_txn_dirty(); // expected_partial

    const auto ehw0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_expected_has_work_total);
    const auto sdo0 = load_u64(g_typed_mutation_audit_counters.composite_commit_sdo_entered_total);
    const auto reuse0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_solve_reuse_hit_total);
    const auto hard0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total);

    CompositeTxnCommitResult cr{};
    (void)svc.evaluator().composite_txn_commit(
        /*mid=*/25092, "ac2-has-work", 0, 0, 1, true, true, &cr);

    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_expected_has_work_total) > ehw0,
          "AC2: expected_has_work cell +1");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_sdo_entered_total) > sdo0,
          "AC2: SDO entered (no vacuous skip)");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_solve_reuse_hit_total) > reuse0,
          "AC2: reuse path (live CS, not greenfield empty)");
    // Conflict inject → solve may fail; empty hard-miss must NOT fire (has_work).
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total) ==
              hard0,
          "AC2: empty hard-miss not used when has_work");
    svc.evaluator().clear_txn_dirty();
    apply_dev_audit_defaults();
}

// ── AC3: !expected + !has_work → structural OK ──
static void ac3_structural_vacuous() {
    std::println("\n--- #2509 AC3: !expected + !has_work → structural OK ---");
    reset_for_test();
    apply_production_audit_defaults();
    CompilerService svc;
    CHECK(svc.eval("(set-code \"(define a3 0)\")").has_value(), "set-code");
    CHECK(svc.eval("(eval-current)").has_value(), "eval");
    CHECK(!svc.evaluator().txn_dirty(), "AC3: !expected_partial");
    CHECK(!svc.evaluator().commit_cs_live(), "AC3: !has_work");

    const auto hard0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total);
    const auto unexp0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_unexpected_cs_work_total);
    const auto ehw0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_expected_has_work_total);

    CompositeTxnCommitResult cr{};
    const bool committed = svc.evaluator().composite_txn_commit(
        /*mid=*/25093, "ac3-structural", 0, 0, 1, true, true, &cr);

    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_empty_cs_hard_miss_total) ==
              hard0,
          "AC3: no hard-miss without expected_partial");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_unexpected_cs_work_total) ==
              unexp0,
          "AC3: no unexpected_cs_work without has_work");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_expected_has_work_total) ==
              ehw0,
          "AC3: no expected_has_work cell");
    if (committed)
        CHECK(cr.solve_ok, "AC3: structural commit solve_ok");
    else
        CHECK(!cr.solve_ok || !cr.linear_ok || !cr.blame_ok || !cr.audit_ok,
              "AC3: reject cites non-matrix reason if any");
    apply_dev_audit_defaults();
}

// ── AC4: !expected + has_work → observe + SDO ──
static void ac4_unexpected_cs_work() {
    std::println("\n--- #2509 AC4: !expected + has_work → observe + SDO ---");
    reset_for_test();
    apply_production_audit_defaults();
    set_strategy(AuditStrategy::Full);
    CompilerService svc;
    CHECK(svc.eval("(set-code \"(define a4 1)\")").has_value(), "set-code");
    CHECK(svc.eval("(eval-current)").has_value(), "eval");

    // Live CS without txn_dirty (unexpected work cell).
    svc.evaluator().inject_commit_cs_type_conflict_for_test();
    CHECK(svc.evaluator().commit_cs_live(), "AC4: has_work");
    CHECK(!svc.evaluator().txn_dirty(), "AC4: !expected_partial");

    const auto unexp0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_unexpected_cs_work_total);
    const auto sdo0 = load_u64(g_typed_mutation_audit_counters.composite_commit_sdo_entered_total);
    const auto reuse0 =
        load_u64(g_typed_mutation_audit_counters.composite_commit_solve_reuse_hit_total);

    CompositeTxnCommitResult cr{};
    (void)svc.evaluator().composite_txn_commit(
        /*mid=*/25094, "ac4-unexpected", 0, 0, 1, true, true, &cr);

    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_unexpected_cs_work_total) >
              unexp0,
          "AC4: unexpected_cs_work +1");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_sdo_entered_total) > sdo0,
          "AC4: SDO still entered (never silent drop under Full)");
    CHECK(load_u64(g_typed_mutation_audit_counters.composite_commit_solve_reuse_hit_total) > reuse0,
          "AC4: reuse/solve path taken");
    apply_dev_audit_defaults();
}

// ── AC5: source-cite + schema ──
static void ac5_source_and_schema() {
    std::println("\n--- #2509 AC5: source-cite + schema ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto ixx = read_file("src/compiler/type_checker.ixx");

    CHECK(etc.find("Issue #2509") != std::string::npos, "AC5: #2509 in composite_txn_commit");
    CHECK(etc.find("expected_partial") != std::string::npos, "AC5: expected_partial");
    CHECK(etc.find("require_sdo") != std::string::npos ||
              etc.find("sdo_entered") != std::string::npos,
          "AC5: SDO gate");
    CHECK(etc.find("composite_commit_unexpected_cs_work_total") != std::string::npos,
          "AC5: unexpected work bump");
    CHECK(etc.find("composite_commit_expected_has_work_total") != std::string::npos,
          "AC5: expected has_work bump");
    CHECK(etc.find("composite_commit_sdo_entered_total") != std::string::npos,
          "AC5: sdo_entered bump");
    CHECK(aud.find("composite_commit_unexpected_cs_work_total") != std::string::npos,
          "AC5: audit counter");
    CHECK(aud.find("composite_cs_signature_matrix_wired") != std::string::npos, "AC5: wired");
    CHECK(ixx.find("commit_cs_has_work") != std::string::npos, "AC5: commit_cs_has_work");
    CHECK(q.find("schema-2509") != std::string::npos, "AC5: query schema-2509");
    CHECK(q.find("composite-commit-unexpected-cs-work-total") != std::string::npos,
          "AC5: query unexpected key");
    CHECK(mut.find("schema-2509") != std::string::npos, "AC5: mutate schema-2509");
    CHECK(cmake.find("test_composite_cs_signature_matrix_2509") != std::string::npos, "AC5: cmake");
    // #2345 lineage retained.
    CHECK(etc.find("composite_empty_cs_hard_reject_enabled") != std::string::npos ||
              etc.find("composite_commit_empty_cs_hard_miss_total") != std::string::npos,
          "AC5: #2345 empty hard-miss retained");
    CHECK(q.find("schema-2345") != std::string::npos, "AC5: schema-2345 retained");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC5: warm");
    CHECK(href(cs, "schema-2509") == 2509, "AC5: schema-2509 live");
    CHECK(href(cs, "issue-2509") == 2509, "AC5: issue-2509");
    CHECK(href(cs, "composite-cs-signature-matrix-wired") == 1, "AC5: wired live");
    CHECK(href(cs, "composite-commit-unexpected-cs-work-total") >= 0, "AC5: unexpected key");
    CHECK(href(cs, "composite-commit-expected-has-work-total") >= 0, "AC5: expected-has-work key");
    CHECK(href(cs, "composite-commit-sdo-entered-total") >= 0, "AC5: sdo-entered key");
    CHECK(href(cs, "schema-2345") == 2345, "AC5: #2345 lineage");
}

} // namespace

int main() {
    std::println("test_composite_cs_signature_matrix_2509");
    ac1_expected_empty_hard_miss();
    ac2_expected_has_work_sdo();
    ac3_structural_vacuous();
    ac4_unexpected_cs_work();
    ac5_source_and_schema();
    if (g_failed)
        return 1;
    std::println("composite CS signature matrix #2509: OK ({} passed)", g_passed);
    return 0;
}
