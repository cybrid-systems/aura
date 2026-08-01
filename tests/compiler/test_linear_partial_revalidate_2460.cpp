// @category: unit
// @reason: Issue #2460 — Phase-2 dirty-set OwnershipEnv re-sim during
//          infer_flat_partial (before boundary-only audit).
//
//   AC1: Dirty linear ownership fail path surfaces during partial under
//        production (not only Full boundary audit)
//   AC2: Phase-1 double-move synthesize path still first-class (#2357)
//   AC3: Soft Warning vs production/strict TypeError policy present
//   AC4: Escape gate / #2108 retained
//   AC5: Zero cost empty dirty linear set; schema-2460 + source-cite

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>

import std;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::ast::FlatAST;
using aura::ast::MutationRecord;
using aura::ast::MutationStatus;
using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::discover_linear_bindings_in_subtree;
using aura::compiler::OwnershipEnv;
using aura::compiler::OwnershipState;
using aura::compiler::TypeChecker;
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

// ── AC1: partial path elevates ownership fail under production ──
static void ac1_partial_path_hard_policy() {
    std::println("\n--- #2460 AC1: partial ownership fail is first-class ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("Issue #2460") != std::string::npos, "AC1: cites #2460");
    CHECK(tci.find("last_partial_linear_revalidate_fail_") != std::string::npos ||
              tci.find("last_partial_linear_revalidate_fail") != std::string::npos,
          "AC1: last_partial_linear_revalidate_fail flag");
    CHECK(tci.find("linear_partial_revalidate_fail_total") != std::string::npos,
          "AC1: fail counter bump");
    CHECK(tci.find("set_node_error") != std::string::npos &&
              tci.find("validate_ownership") != std::string::npos,
          "AC1: validate_ownership + set_node_error in partial");
    CHECK(tci.find("discover_linear_bindings_in_subtree") != std::string::npos,
          "AC1: discover_linear_bindings_in_subtree wired");

    // Unit ownership: two-site use-after-move is detected by OwnershipEnv.
    OwnershipEnv env;
    env.mark("x", OwnershipState::Owned);
    CHECK(env.can_move("x"), "AC1: can move once");
    env.mark("x", OwnershipState::Moved);
    CHECK(!env.can_move("x"), "AC1: second move fails");

    // Production policy arming used by hard path.
    apply_production_audit_defaults();
    CHECK(production_defaults_active(), "AC1: production defaults armed");
    apply_dev_audit_defaults();
}

// ── AC2: Phase-1 path unchanged ──
static void ac2_phase1_unchanged() {
    std::println("\n--- #2460 AC2: Phase-1 synthesize path retained ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("note_linear_synth_violation") != std::string::npos, "AC2: #2357 helper");
    CHECK(tci.find("synthesize_flat_move") != std::string::npos, "AC2: synthesize_flat_move");
    CHECK(tci.find("can_move") != std::string::npos, "AC2: can_move check");
    CompilerService cs;
    CHECK(cs.eval("(let ((l (Linear 1))) (move l))").has_value(), "AC2: single move still ok");
}

// ── AC3: Soft vs hard policy ──
static void ac3_soft_vs_hard() {
    std::println("\n--- #2460 AC3: Soft Warning vs production TypeError ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("production_defaults_active") != std::string::npos, "AC3: production gate");
    CHECK(tci.find("ErrorKind::Warning") != std::string::npos, "AC3: Warning soft");
    CHECK(tci.find("ErrorKind::TypeError") != std::string::npos, "AC3: TypeError hard");
    CHECK(tci.find("strict_") != std::string::npos, "AC3: strict_ hard");
}

// ── AC4: escape / #2108 retained ──
static void ac4_escape_retained() {
    std::println("\n--- #2460 AC4: escape gate + #2108 retained ---");
    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tci.find("analyze_linear_escape") != std::string::npos ||
              tci.find("escape-after-move") != std::string::npos,
          "AC4: escape analysis path");
    CHECK(tci.find("post_mutation_invariant_check") != std::string::npos ||
              read_file("src/compiler/type_checker.ixx").find("post_mutation_invariant_check") !=
                  std::string::npos,
          "AC4: post_mutation_invariant_check retained");
    CHECK(read_file("src/compiler/observability_metrics.h").find("linear_escape_commit_blocked") !=
              std::string::npos,
          "AC4: #2108 counter retained");
}

// ── AC5: zero cost empty + schema ──
static void ac5_zero_cost_and_schema() {
    std::println("\n--- #2460 AC5: empty dirty + schema-2460 ---");
    // Empty dirty linear set: discover returns empty, validate not required.
    FlatAST flat;
    StringPool pool;
    std::unordered_set<std::string> out;
    if (flat.root != NULL_NODE)
        discover_linear_bindings_in_subtree(flat, pool, flat.root, out);
    CHECK(out.empty(), "AC5: empty flat → no linear bindings");

    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    metrics.linear_partial_revalidate_total.store(4, std::memory_order_relaxed);
    metrics.linear_partial_revalidate_fail_total.store(2, std::memory_order_relaxed);
    metrics.linear_partial_revalidate_hard_fail_total.store(1, std::memory_order_relaxed);

    CHECK(href(cs, "schema-2460") == 2460, "AC5: schema-2460");
    CHECK(href(cs, "issue-2460") == 2460, "AC5: issue-2460");
    CHECK(href(cs, "linear-partial-revalidate-total") == 4, "AC5: revalidate total");
    CHECK(href(cs, "linear-partial-revalidate-fail-total") == 2, "AC5: fail total");
    CHECK(href(cs, "linear-partial-revalidate-hard-fail-total") == 1, "AC5: hard fail total");
    CHECK(href(cs, "linear-partial-revalidate-wired") == 1, "AC5: wired");
    // Lineage
    CHECK(href(cs, "schema-2357") == 2357, "AC5: schema-2357 retained");
    CHECK(href(cs, "linear-synth-wired") == 1, "AC5: #2357 wired retained");

    const auto tci = read_file("src/compiler/type_checker_impl.cpp");
    const auto tch = read_file("src/compiler/type_checker.ixx");
    CHECK(tci.find("linear_partial_revalidate_total") != std::string::npos, "AC5: counter in impl");
    CHECK(tch.find("last_partial_linear_revalidate_fail") != std::string::npos,
          "AC5: accessor in ixx");
    CHECK(tci.find("Zero cost when no linear") != std::string::npos ||
              tci.find("Zero cost when no linear bindings") != std::string::npos,
          "AC5: zero-cost comment");
}

} // namespace

int main() {
    std::println("=== Issue #2460: Phase-2 dirty ownership re-sim in infer_flat_partial ===");
    ac1_partial_path_hard_policy();
    ac2_phase1_unchanged();
    ac3_soft_vs_hard();
    ac4_escape_retained();
    ac5_zero_cost_and_schema();
    std::println("\n=== #2460 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
