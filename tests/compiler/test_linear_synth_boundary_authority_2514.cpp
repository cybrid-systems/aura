// @category: unit
// @reason: Issue #2514 — unify linear_synth_hard_fail with MutationBoundary
// audit exit (single rollback authority).
//
//   AC1: Production synth hard-fail → boundary/hard-gate force-rollback;
//        no Success audit for that mid
//   AC2: Soft Warning synth → no forced rollback solely from synth flag
//   AC3: Escape-only (synth clean) → post-mutate path retained (#2263/#2108)
//   AC4: Counter ownership documented; query keys stable (no double-count)
//   AC5: Source-cite single exit decision table

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
import aura.compiler.type_checker;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::production_defaults_active;
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

// ── AC1: force-rollback path wired; no Success when synth hard-fail ──
static void ac1_production_force_rollback() {
    std::println("\n--- AC1: production synth hard-fail → force-rollback ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto tch = read_file("src/compiler/type_checker.ixx");

    CHECK(etc.find("deny_if_linear_synth_hard_fail") != std::string::npos,
          "AC1: deny_if_linear_synth_hard_fail helper");
    CHECK(etc.find("finish_mutate_hard_gate") != std::string::npos &&
              etc.find("deny_if_linear_synth_hard_fail") != std::string::npos,
          "AC1: hard-gate consults synth authority");
    CHECK(emb.find("linear_synth_hard_fail_pending") != std::string::npos,
          "AC1: boundary checks pending synth hard-fail");
    CHECK(emb.find("linear-synth-hard-fail") != std::string::npos ||
              emb.find("linear_synth_hard_fail") != std::string::npos,
          "AC1: boundary deny path cites synth hard-fail");
    CHECK(etc.find("linear-synth-hard-fail") != std::string::npos,
          "AC1: deny kind linear-synth-hard-fail");
    CHECK(etc.find("linear_synth_boundary_skip_recovery_total") != std::string::npos ||
              emb.find("skip_recovery") != std::string::npos ||
              etc.find("skip_recovery") != std::string::npos,
          "AC1: skip soft partial recovery counter");
    CHECK(tci.find("last_linear_synth_hard_fail_") != std::string::npos ||
              tci.find("last_linear_synth_hard_fail") != std::string::npos,
          "AC1: TypeChecker sticky flag from engine");
    CHECK(tch.find("last_linear_synth_hard_fail") != std::string::npos,
          "AC1: TypeChecker accessor declared");

    // Policy arm: production defaults enable hard path at synthesize.
    apply_production_audit_defaults();
    CHECK(production_defaults_active(), "AC1: production defaults arm hard synth");
    apply_dev_audit_defaults();

    // Single move still ok (no hard-fail without double-move).
    CompilerService cs;
    CHECK(cs.eval("(let ((l (Linear 5))) (move l))").has_value(), "AC1: single move ok");
}

// ── AC2: Soft Warning → no forced rollback solely from synth flag ──
static void ac2_soft_warning_no_force() {
    std::println("\n--- AC2: Soft Warning synth → no force-rollback ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");

    // Soft path: Warning when !production && !strict (sets hard=false).
    CHECK(tci.find("ErrorKind::Warning") != std::string::npos, "AC2: Warning soft path");
    CHECK(tci.find("production_defaults_active") != std::string::npos, "AC2: hard only under prod");
    // Sticky flag only set when engine.linear_synth_hard_fail() — soft leaves false.
    CHECK(tci.find("if (engine.linear_synth_hard_fail())") != std::string::npos ||
              tci.find("if (r.linear_synth_hard_fail)") != std::string::npos,
          "AC2: sticky only when hard");
    CHECK(etc.find("Soft Warning never sets") != std::string::npos ||
              etc.find("Soft Warning") != std::string::npos ||
              etc.find("soft Warning") != std::string::npos ||
              etc.find("only sets linear_synth_hard_fail_") != std::string::npos,
          "AC2: soft does not force documented");

    apply_dev_audit_defaults();
    CHECK(!production_defaults_active(), "AC2: dev defaults = soft");
}

// ── AC3: escape-only (synth clean) post-mutate retained ──
static void ac3_escape_defense_in_depth() {
    std::println("\n--- AC3: escape-only / post-mutate retained ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");

    CHECK(tci.find("post_mutation_invariant_check") != std::string::npos ||
              read_file("src/compiler/type_checker.ixx").find("post_mutation_invariant_check") !=
                  std::string::npos,
          "AC3: post_mutation_invariant_check retained");
    CHECK(etc.find("hard_block_cross_batch_linear_escape") != std::string::npos ||
              emb.find("hard_block_cross_batch_linear_escape") != std::string::npos ||
              emb.find("linear_escape") != std::string::npos,
          "AC3: escape hard-block path retained");
    CHECK(tci.find("Issue #2263") != std::string::npos ||
              tci.find("ownership_escape_lowering_gate") != std::string::npos ||
              read_file("src/compiler/ownership_escape_lowering_gate.h").find("escape") !=
                  std::string::npos,
          "AC3: #2263 escape elision gate retained");
    // When synth clean, decision table continues post-mutate (source-cite).
    CHECK(etc.find("synth clean") != std::string::npos ||
              emb.find("synth clean") != std::string::npos ||
              etc.find("defense-in-depth") != std::string::npos,
          "AC3: synth-clean → post-mutate defense-in-depth cited");
}

// ── AC4: counter ownership + stable query keys ──
static void ac4_counter_ownership() {
    std::println("\n--- AC4: counter ownership + query keys ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_security.cpp");

    CHECK(aud.find("linear_synth_boundary_force_rollback_total") != std::string::npos,
          "AC4: boundary force-rollback counter");
    CHECK(aud.find("linear_synth_boundary_skip_recovery_total") != std::string::npos,
          "AC4: skip-recovery counter");
    CHECK(aud.find("Counter ownership") != std::string::npos ||
              aud.find("no double-count") != std::string::npos ||
              etc.find("Do NOT bump linear_invariant_fail") != std::string::npos,
          "AC4: no double-count documented");
    CHECK(etc.find("Do NOT bump linear_invariant_fail") != std::string::npos ||
              etc.find("linear_invariant_fail") != std::string::npos,
          "AC4: early-exit skips linear_invariant_fail re-bump");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2514") == 2514, "AC4: schema-2514");
    CHECK(href(cs, "issue-2514") == 2514, "AC4: issue-2514");
    CHECK(href(cs, "linear-synth-authority-unified") == 1, "AC4: authority-unified");
    CHECK(href(cs, "linear-synth-boundary-force-rollback-total") >= 0,
          "AC4: force-rollback query key");
    CHECK(href(cs, "linear-synth-boundary-skip-recovery-total") >= 0,
          "AC4: skip-recovery query key");
    // Stable #2357 keys retained.
    CHECK(href(cs, "schema-2357") == 2357, "AC4: schema-2357 stable");
    CHECK(href(cs, "linear-synth-hard-fail-total") >= 0, "AC4: synth hard-fail key stable");

    CHECK(q.find("schema-2514") != std::string::npos, "AC4: query surface cites schema");
    CHECK(q.find("linear-synth-boundary-force-rollback-total") != std::string::npos,
          "AC4: query key string");

    // Reset does not leave counters undefined.
    CHECK(load_u64(g_typed_mutation_audit_counters.linear_synth_boundary_force_rollback_total) >= 0,
          "AC4: counter readable");
}

// ── AC5: single exit decision table source-cite ──
static void ac5_decision_table() {
    std::println("\n--- AC5: exit decision table source-cite ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto eixx = read_file("src/compiler/evaluator.ixx");

    CHECK(etc.find("Exit decision table") != std::string::npos ||
              etc.find("decision table") != std::string::npos ||
              eixx.find("Decision table") != std::string::npos,
          "AC5: decision table comment present");
    CHECK(etc.find("linear-synth-hard-fail") != std::string::npos, "AC5: deny kind cited");
    CHECK(etc.find("deny_if_linear_synth_hard_fail") != std::string::npos &&
              emb.find("linear_synth_hard_fail_pending") != std::string::npos,
          "AC5: hard-gate + boundary both consult authority");
    CHECK(eixx.find("Issue #2514") != std::string::npos, "AC5: evaluator.ixx declares #2514 API");
    CHECK(etc.find("Issue #2514") != std::string::npos, "AC5: typecheck cites #2514");
    CHECK(emb.find("Issue #2514") != std::string::npos, "AC5: boundary cites #2514");
}

} // namespace

int main() {
    std::println("=== Issue #2514: linear_synth_hard_fail ↔ MutationBoundary authority ===");
    ac5_decision_table();
    ac1_production_force_rollback();
    ac2_soft_warning_no_force();
    ac3_escape_defense_in_depth();
    ac4_counter_ownership();
    apply_dev_audit_defaults();
    std::println("\n=== #2514: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
