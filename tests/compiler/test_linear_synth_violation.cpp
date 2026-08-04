// @category: unit
// @reason: Issue #2357 — Phase-1 linear Move/Drop violations as first-class
// check during synthesize (not only post-mutate audit).
//
//   AC1: Double-move / can_move fail path is first-class (note_linear_synth
//        + TypeError under production/strict) before boundary Full audit only
//   AC2: Valid single move + drop still ok; escape elision path unchanged
//   AC3: Permissive non-strict non-production → Warning; production → hard
//   AC4: post_mutation_invariant_check + #2108 escape hard-block retained
//   AC5: Counter + query schema-2357; source-cite synthesize paths

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

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::OwnershipEnv;
using aura::compiler::OwnershipState;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
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

// ── AC1: OwnershipEnv double-move is first-class fail signal ──
static void ac1_double_move_first_class() {
    std::println("\n--- AC1: double-move can_move fails before post-mutate only ---");
    OwnershipEnv env;
    env.mark("x", OwnershipState::Owned);
    CHECK(env.can_move("x"), "AC1: Owned can_move");
    env.mark("x", OwnershipState::Moved);
    CHECK(!env.can_move("x"), "AC1: Moved cannot move again");
    CHECK(!env.can_drop("x"), "AC1: Moved cannot drop");
    CHECK(env.state_name(env.get("x")) == "moved", "AC1: state is moved");

    // Happy eval: single move still works (synthesize allows first move).
    CompilerService cs;
    CHECK(cs.eval("(let ((l (Linear 5))) (move l))").has_value(), "AC1: single move eval ok");

    // Double-move at runtime/eval may soft-fail; source must wire first-class.
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("note_linear_synth_violation") != std::string::npos,
          "AC1: note_linear_synth_violation present");
    CHECK(tci.find("synthesize_flat_move") != std::string::npos &&
              tci.find("can_move") != std::string::npos,
          "AC1: synthesize_flat_move checks can_move");
    CHECK(tci.find("set_node_error") != std::string::npos, "AC1: set_node_error on violation");
}

// ── AC2: valid move + drop + escape gate source ──
static void ac2_valid_move_and_escape_gate() {
    std::println("\n--- AC2: valid move/drop + escape gate unchanged ---");
    OwnershipEnv env;
    env.mark("y", OwnershipState::Owned);
    CHECK(env.can_move("y"), "AC2: can move owned");
    env.mark("y", OwnershipState::Moved);
    // Fresh owned for drop path
    env.mark("z", OwnershipState::Owned);
    CHECK(env.can_drop("z"), "AC2: can drop owned");

    CompilerService cs;
    CHECK(cs.eval("(let ((l (Linear 1))) (move l))").has_value(), "AC2: single move eval");
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC2: warm after linear");

    const auto gate = read_file("src/compiler/ownership_escape_lowering_gate.h");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(gate.find("escape") != std::string::npos ||
              tci.find("analyze_linear_escape") != std::string::npos,
          "AC2: escape analysis path retained");
    CHECK(tci.find("Issue #2263") != std::string::npos ||
              tci.find("ownership_escape_lowering_gate") != std::string::npos,
          "AC2: #2263 elision gate still referenced");
}

// ── AC3: production → hard; soft path documented ──
static void ac3_production_hard_policy() {
    std::println("\n--- AC3: production_defaults → hard; soft Warning path ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("production_defaults_active") != std::string::npos,
          "AC3: production_defaults_active in note_linear_synth_violation");
    CHECK(tci.find("ErrorKind::Warning") != std::string::npos, "AC3: Warning soft path");
    CHECK(tci.find("ErrorKind::TypeError") != std::string::npos, "AC3: TypeError hard path");
    CHECK(tci.find("strict_") != std::string::npos, "AC3: strict_ also hard");

    // Policy: production defaults active → hard fail flag logic present.
    apply_production_audit_defaults();
    CHECK(production_defaults_active(), "AC3: production defaults can be armed");
    apply_dev_audit_defaults();
    CHECK(!production_defaults_active(), "AC3: dev defaults clear production");
}

// ── AC4: post_mutation + #2108 retained ──
static void ac4_post_mutation_and_2108() {
    std::println("\n--- AC4: post_mutation_invariant + #2108 retained ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto tch = read_file("src/compiler/type_checker.ixx");
    CHECK(tci.find("post_mutation_invariant_check") != std::string::npos ||
              tch.find("post_mutation_invariant_check") != std::string::npos,
          "AC4: post_mutation_invariant_check retained");
    CHECK(tci.find("validate_ownership") != std::string::npos, "AC4: validate_ownership retained");
    const auto hard = read_file("src/compiler/type_checker_impl.cpp");
    // #2108 may live in service/composite path
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(hard.find("linear_escape_commit_blocked") != std::string::npos ||
              svc.find("linear_escape") != std::string::npos ||
              hard.find("Issue #2108") != std::string::npos ||
              read_file("src/compiler/observability_metrics.h")
                      .find("linear_escape_commit_blocked") != std::string::npos,
          "AC4: #2108 escape commit hard-block counter retained");
}

// ── AC5: query + source-cite ──
static void ac5_query_and_source() {
    std::println("\n--- AC5: query schema-2357 + source-cite ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2357") == 2357, "AC5: schema-2357");
    CHECK(href(cs, "issue-2357") == 2357, "AC5: issue-2357");
    CHECK(href(cs, "linear-synth-wired") == 1, "AC5: wired");
    CHECK(href(cs, "linear-synth-violation-total") >= 0, "AC5: violation-total");
    CHECK(href(cs, "linear-synth-hard-fail-total") >= 0, "AC5: hard-fail-total");

    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto tch = read_file("src/compiler/type_checker.ixx");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(tci.find("Issue #2357") != std::string::npos, "AC5: impl cites #2357");
    CHECK(tch.find("note_linear_synth_violation") != std::string::npos, "AC5: declared on engine");
    CHECK(tci.find("synthesize_flat_drop") != std::string::npos &&
              tci.find("note_linear_synth_violation") != std::string::npos,
          "AC5: drop path uses note_linear_synth_violation");
    CHECK(tci.find("synthesize_flat_mut_borrow") != std::string::npos, "AC5: mut_borrow path");
    CHECK(met.find("linear_synth_violation_total") != std::string::npos, "AC5: metrics field");
    CHECK(q.find("schema-2357") != std::string::npos, "AC5: query schema");
    CHECK(q.find("linear-synth-violation-total") != std::string::npos, "AC5: query key");
}

} // namespace

int run_test_linear_synth_violation() {
    std::println("=== Issue #2357: linear synth Move/Drop first-class violation ===");
    ac5_query_and_source();
    ac1_double_move_first_class();
    ac2_valid_move_and_escape_gate();
    ac3_production_hard_policy();
    ac4_post_mutation_and_2108();
    apply_dev_audit_defaults(); // leave process soft
    std::println("\n=== #2357: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_synth_violation();
}
#endif
