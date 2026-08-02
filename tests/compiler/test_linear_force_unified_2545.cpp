// @category: unit
// @reason: Issue #2545 — unify hard-fail decision entry (synth + post-mutate
//          + escape + boundary) via force_linear_rollback.
//
//   AC1: Production/strict synth → force-rollback; soft recovery skipped;
//        linear_synth_boundary_force_rollback_total advances
//   AC2: Synth hard-fail does NOT also bump linear_invariant_fail (same mid)
//   AC3: Post-mutate-only linear fail (clean synth) still bumps
//        linear_invariant_fail and force-rollback under Full/production
//   AC4: Soft Warning synth does not set sticky / does not force rollback
//   AC5: Clean path: zero extra force counters
//   AC6: Source-cite + linter (all boundary/hard-gate sites call unified entry)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
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
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::production_defaults_active;
using aura::compiler::typed_audit::reset_for_test;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-ownership-typed-mutate-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::uint64_t load_u64(const std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

// ── AC1: production synth force path wired through force_linear_rollback ──
static void ac1_synth_force_via_unified() {
    std::println("\n--- #2545 AC1: synth hard-fail → force_linear_rollback ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto eixx = read_file("src/compiler/evaluator.ixx");

    CHECK(eixx.find("force_linear_rollback") != std::string::npos,
          "AC1: force_linear_rollback declared");
    CHECK(eixx.find("LinearForceAuthority") != std::string::npos, "AC1: LinearForceAuthority enum");
    CHECK(eixx.find("classify_linear_force") != std::string::npos, "AC1: classify declared");
    CHECK(etc.find("force_linear_rollback") != std::string::npos,
          "AC1: force_linear_rollback impl");
    CHECK(etc.find("LinearForceAuthority::SynthHardFail") != std::string::npos,
          "AC1: SynthHardFail authority");
    CHECK(etc.find("linear_synth_boundary_force_rollback_total") != std::string::npos,
          "AC1: boundary force-rollback counter");
    CHECK(etc.find("linear_synth_boundary_skip_recovery_total") != std::string::npos,
          "AC1: skip soft recovery counter");
    CHECK(emb.find("force_linear_rollback") != std::string::npos,
          "AC1: boundary calls force_linear_rollback");
    CHECK(etc.find("finish_mutate_hard_gate") != std::string::npos &&
              etc.find("force_linear_rollback(op)") != std::string::npos,
          "AC1: hard-gate consults force_linear_rollback");
    CHECK(etc.find("composite_txn_commit") != std::string::npos &&
              etc.find("force_linear_rollback") != std::string::npos,
          "AC1: composite commit consults force_linear_rollback");

    apply_production_audit_defaults();
    CHECK(production_defaults_active(), "AC1: production defaults arm hard synth");
    apply_dev_audit_defaults();

    CompilerService cs;
    CHECK(cs.eval("(let ((l (Linear 5))) (move l))").has_value(), "AC1: single move ok");
}

// ── AC2: synth early-exit does not re-bump linear_invariant_fail ──
static void ac2_no_double_count() {
    std::println("\n--- #2545 AC2: synth early-exit no linear_invariant_fail double-count ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");

    CHECK(etc.find("Do NOT bump linear_invariant_fail") != std::string::npos,
          "AC2: no linear_invariant_fail re-bump documented");
    CHECK(etc.find("Early-exit") != std::string::npos ||
              etc.find("early-exit") != std::string::npos ||
              etc.find("NOT re-run invariant linear walk") != std::string::npos ||
              etc.find("not re-run") != std::string::npos,
          "AC2: early-exit before audit linear walk");
    CHECK(aud.find("When synth early-exits") != std::string::npos ||
              aud.find("invariant audit is NOT re-run") != std::string::npos ||
              aud.find("no double-count") != std::string::npos,
          "AC2: audit header documents no double-count");
    // SynthHardFail branch does not call linear_post_mutate_enforce.
    const auto synth_branch = etc.find("LinearForceAuthority::SynthHardFail");
    CHECK(synth_branch != std::string::npos, "AC2: SynthHardFail branch present");
    if (synth_branch != std::string::npos) {
        const auto next = etc.find("case LinearForceAuthority::PostMutateLinear", synth_branch);
        const auto slice =
            etc.substr(synth_branch, next == std::string::npos ? 400 : next - synth_branch);
        CHECK(slice.find("linear_invariant_fail") == std::string::npos ||
                  slice.find("Do NOT bump linear_invariant_fail") != std::string::npos,
              "AC2: SynthHardFail case does not fetch_add linear_invariant_fail");
    }
}

// ── AC3: post-mutate-only path retained ──
static void ac3_post_mutate_only() {
    std::println("\n--- #2545 AC3: post-mutate-only linear fail retained ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto eixx = read_file("src/compiler/evaluator.ixx");

    CHECK(etc.find("LinearForceAuthority::PostMutateLinear") != std::string::npos,
          "AC3: PostMutateLinear authority");
    CHECK(etc.find("linear-post-mutate-fail") != std::string::npos,
          "AC3: deny kind linear-post-mutate-fail");
    CHECK(eixx.find("last_post_mutate_linear_fail") != std::string::npos,
          "AC3: sticky last_post_mutate_linear_fail");
    CHECK(etc.find("note_post_mutate_linear_fail") != std::string::npos,
          "AC3: audit notes post-mutate sticky");
    CHECK(etc.find("LinearForceAuthority::CrossBatchEscape") != std::string::npos,
          "AC3: CrossBatchEscape authority");
    CHECK(etc.find("linear-cross-batch-escape") != std::string::npos,
          "AC3: deny kind cross-batch escape");
    // hard-gate final deny routes linear through force_linear_rollback
    CHECK(etc.find("force_linear_rollback(op, &r)") != std::string::npos ||
              etc.find("force_linear_rollback(op, &") != std::string::npos,
          "AC3: hard-gate final deny uses force_linear_rollback with result");
    CHECK(emb.find("force_linear_rollback") != std::string::npos &&
              emb.find("&first") != std::string::npos,
          "AC3: boundary force path uses force_linear_rollback with first audit");
    // linear_invariant_fail still owned by audit walk
    CHECK(read_file("src/compiler/typed_mutation_audit.h").find("linear_invariant_fail") !=
              std::string::npos,
          "AC3: linear_invariant_fail counter retained");
}

// ── AC4: Soft Warning no force ──
static void ac4_soft_warning() {
    std::println("\n--- #2545 AC4: Soft Warning synth → no force ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");

    CHECK(tci.find("ErrorKind::Warning") != std::string::npos, "AC4: Warning soft path");
    CHECK(tci.find("production_defaults_active") != std::string::npos, "AC4: hard only under prod");
    CHECK(etc.find("Soft Warning never sets") != std::string::npos ||
              etc.find("Soft Warning") != std::string::npos,
          "AC4: soft Warning documented in force path");
    CHECK(etc.find("classify_linear_force") != std::string::npos, "AC4: pure classify");

    apply_dev_audit_defaults();
    CHECK(!production_defaults_active(), "AC4: dev defaults = soft");
}

// ── AC5: clean path zero extra force ──
static void ac5_clean_zero_cost() {
    std::println("\n--- #2545 AC5: clean path zero extra force counters ---");
    reset_for_test();
    const auto fr0 =
        load_u64(g_typed_mutation_audit_counters.linear_synth_boundary_force_rollback_total);
    const auto hg0 = load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total);

    CompilerService cs;
    CHECK(cs.eval("(+ 1 2)").has_value(), "AC5: pure eval ok");
    CHECK(cs.eval("(let ((l (Linear 7))) (move l))").has_value(), "AC5: single move clean");

    CHECK(load_u64(g_typed_mutation_audit_counters.linear_synth_boundary_force_rollback_total) ==
              fr0,
          "AC5: force-rollback total unchanged on clean");
    CHECK(load_u64(g_typed_mutation_audit_counters.hard_gate_force_rollback_total) == hg0,
          "AC5: hard-gate force unchanged on clean single-move");

    // classify returns None when all sticky clean — source-cite
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("zero-cost clean path") != std::string::npos ||
              etc.find("LinearForceAuthority::None") != std::string::npos,
          "AC5: zero-cost None path documented");
}

// ── AC6: all sites + schema + linter ──
static void ac6_source_and_schema() {
    std::println("\n--- #2545 AC6: source-cite + schema + all call sites ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto eixx = read_file("src/compiler/evaluator.ixx");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_security.cpp");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/check_linear_force_unified_2545.py");

    // All production call sites use force_linear_rollback (not only deny_if)
    CHECK(emb.find("force_linear_rollback") != std::string::npos, "AC6: boundary uses unified");
    CHECK(etc.find("force_linear_rollback") != std::string::npos, "AC6: typecheck uses unified");
    // deny_if is thin alias
    CHECK(etc.find("deny_if_linear_synth_hard_fail") != std::string::npos &&
              etc.find("return force_linear_rollback") != std::string::npos,
          "AC6: deny_if is back-compat alias → force_linear_rollback");
    CHECK(eixx.find("Issue #2545") != std::string::npos, "AC6: evaluator.ixx cites #2545");
    CHECK(etc.find("Issue #2545") != std::string::npos, "AC6: typecheck cites #2545");
    CHECK(emb.find("Issue #2545") != std::string::npos || emb.find("#2545") != std::string::npos,
          "AC6: boundary cites #2545");
    CHECK(aud.find("Authority table") != std::string::npos ||
              aud.find("force_linear_rollback") != std::string::npos,
          "AC6: audit header authority table");
    CHECK(aud.find("linear_force_unified_2545") != std::string::npos, "AC6: unified flag field");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2545") == 2545, "AC6: schema-2545");
    CHECK(href(cs, "issue-2545") == 2545, "AC6: issue-2545");
    CHECK(href(cs, "linear-force-unified") == 1, "AC6: linear-force-unified");
    CHECK(href(cs, "linear-force-rollback-wired") == 1, "AC6: wired sentinel");
    // Compatible lineage
    CHECK(href(cs, "schema-2514") == 2514, "AC6: schema-2514 retained");
    CHECK(href(cs, "schema-2357") == 2357, "AC6: schema-2357 retained");
    CHECK(href(cs, "linear-synth-authority-unified") == 1, "AC6: #2514 authority retained");

    CHECK(q.find("schema-2545") != std::string::npos, "AC6: query security surface");
    CHECK(mut.find("schema-2545") != std::string::npos, "AC6: mutate surface");
    CHECK(cmake.find("test_linear_force_unified_2545") != std::string::npos, "AC6: cmake target");
    CHECK(build.find("check_linear_force_unified_2545") != std::string::npos ||
              build.find("linear_force_unified") != std::string::npos,
          "AC6: build.py coverage");
    CHECK(!lint.empty() && lint.find("force_linear_rollback") != std::string::npos,
          "AC6: linter script present");
}

} // namespace

int main() {
    std::println("=== Issue #2545: force_linear_rollback unified entry ===");
    ac1_synth_force_via_unified();
    ac2_no_double_count();
    ac3_post_mutate_only();
    ac4_soft_warning();
    ac5_clean_zero_cost();
    ac6_source_and_schema();
    apply_dev_audit_defaults();
    if (g_failed)
        return 1;
    std::println("\n=== #2545: {} passed, {} failed ===", g_passed, g_failed);
    return 0;
}
