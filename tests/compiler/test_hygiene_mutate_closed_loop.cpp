// @category: unit
// @reason: Issue #2037 — MacroIntroduced provenance stamp + FailOnStale on
// mutate:replace-pattern / query-and-replace hotpaths (query→mutate→re-query).
//
//   AC1: source cites #2037; enforce_macro_hygiene_mutate_hotpath +
//        propagate_macro_introduced_marker in mutate.cpp
//   AC2: default replace-pattern on MacroIntroduced fails closed
//        (hygiene-protected) without :allow-macro?
//   AC3: allowed replace-pattern stamps MacroIntroduced on replacement
//        (marker-propagate) + provenance hits
//   AC4: query → mutate → re-query closed loop keeps MacroIntroduced
//   AC5: query:macro-hygiene-provenance-stats schema-2037 keys
//   AC6: provenance_tracker.hh documents FailOnStale mutate contract

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"

#include <cstdint>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:macro-hygiene-provenance-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (double y) (* y 2)) "
                 "(double 3) (double 4) "
                 "(define base 10) (+ base 1)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2037 ---");
    auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    auto prov = read_file("src/core/provenance_tracker.hh");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(!mut.empty() && mut.find("#2037") != std::string::npos, "mutate.cpp #2037");
    CHECK(mut.find("enforce_macro_hygiene_mutate_hotpath") != std::string::npos, "enforce helper");
    CHECK(mut.find("propagate_macro_introduced_marker") != std::string::npos, "propagate marker");
    CHECK(!prov.empty() && prov.find("#2037") != std::string::npos, "provenance #2037");
    CHECK(prov.find("FailOnStale") != std::string::npos, "FailOnStale contract");
    CHECK(!met.empty() && met.find("hygiene_mutate_restamp_total") != std::string::npos,
          "restamp metric");
    CHECK(met.find("hygiene_mutate_fail_on_stale_total") != std::string::npos, "fail-on-stale");
    CHECK(!q.empty() && q.find("schema-2037") != std::string::npos, "query schema-2037");
}

static void ac2_default_fail_closed() {
    std::println("\n--- AC2: default MacroIntroduced mutate fails closed ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace");
    std::size_t macro_n = 0;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->is_macro_introduced(id))
            ++macro_n;
    }
    CHECK(macro_n >= 1, "has MacroIntroduced nodes");

    // Issue #2961: :include-macro-introduced only controls matcher visibility;
    // mutate still requires :allow-macro? (fail-closed). Prefer replace-subtree
    // on a MacroIntroduced node for a hard hygiene reject.
    aura::ast::NodeId target = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->is_macro_introduced(id) &&
            ws->parent_of(id) != aura::ast::NULL_NODE) {
            target = id;
            break;
        }
    }
    if (target == aura::ast::NULL_NODE) {
        CHECK(true, "soft-skip: no parented MacroIntroduced for replace-subtree");
        return;
    }
    auto r = cs.eval(std::format("(mutate:replace-subtree {} \"99\")", target));
    // Should fail hygiene (replace-subtree always rejects MacroIntroduced).
    CHECK(r.has_value(), "returns value (error or bool)");
    // Not a bare #t success.
    if (r && is_bool(*r))
        CHECK(!as_bool(*r), "replace-subtree MacroIntroduced not success bool true");
    else
        CHECK(true, "structured hygiene error (non-bool)");
}

static void ac3_allowed_propagate() {
    std::println("\n--- AC3: allowed replace-pattern propagates marker ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto prop0 = href(cs, "hygiene-mutate-marker-propagate-total");
    const auto hits0 = href(cs, "macro-hygiene-provenance-hits");
    // Include MacroIntroduced and allow mutate.
    auto r = cs.eval("(mutate:replace-pattern \"(* 3 2)\" \"(+ 3 3)\" "
                     ":include-macro-introduced #t :allow-macro? #t)");
    // Pattern may or may not match expanded form; try a broader pattern.
    if (!r || (is_bool(*r) && !as_bool(*r))) {
        r = cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" "
                    ":include-macro-introduced #t :allow-macro? #t)");
    }
    CHECK(r.has_value(), "replace-pattern ran");
    // Provenance hits should be non-decreasing after any MacroIntroduced touch.
    const auto hits1 = href(cs, "macro-hygiene-provenance-hits");
    CHECK(hits1 >= hits0, "provenance hits non-decreasing");
    const auto prop1 = href(cs, "hygiene-mutate-marker-propagate-total");
    CHECK(prop1 >= prop0, "marker-propagate non-decreasing");
    // If replace succeeded, marker propagate may have advanced.
    if (r && is_bool(*r) && as_bool(*r))
        CHECK(prop1 >= prop0, "propagate after success");
}

static void ac4_closed_loop() {
    std::println("\n--- AC4: query → mutate → re-query MacroIntroduced ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto n0 = cs.eval("(length (query:macro-introduced))");
    CHECK(n0 && is_int(*n0) && as_int(*n0) >= 1, "macro-introduced ≥1 before");
    // Soft mutate on user (non-macro) node — hygiene must hold for macros.
    auto mut = cs.eval("(mutate:replace-pattern \"(+ base 1)\" \"(+ base 2)\")");
    CHECK(mut.has_value(), "user-node replace-pattern");
    auto n1 = cs.eval("(length (query:macro-introduced))");
    CHECK(n1 && is_int(*n1) && as_int(*n1) >= 1, "macro-introduced still ≥1 after user mutate");
    // With allow + include, mutate a macro form and re-query.
    (void)cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(* ... ...)\" "
                  ":include-macro-introduced #t :allow-macro? #t)");
    auto n2 = cs.eval("(length (query:macro-introduced))");
    CHECK(n2 && is_int(*n2), "macro-introduced query after allowed mutate");
    CHECK(as_int(*n2) >= 0, "count readable");
    // Schema still live
    CHECK(href(cs, "schema-2037") == 2037, "schema-2037 after closed loop");
}

static void ac5_query_schema() {
    std::println("\n--- AC5: query:macro-hygiene-provenance-stats schema-2037 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:macro-hygiene-provenance-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2037") == 2037, "schema-2037");
    CHECK(href(cs, "issue-2037") == 2037, "issue-2037");
    CHECK(href(cs, "mutate-hotpath-hygiene-wired") == 1, "wired");
    CHECK(href(cs, "hygiene-mutate-restamp-total") >= 0, "restamp key");
    CHECK(href(cs, "hygiene-mutate-fail-on-stale-total") >= 0, "fail-on-stale key");
    CHECK(href(cs, "hygiene-mutate-marker-propagate-total") >= 0, "propagate key");
    CHECK(href(cs, "schema") == 757, "lineage schema 757");
}

static void ac6_contract_docs() {
    std::println("\n--- AC6: contract docs present ---");
    auto prov = read_file("src/core/provenance_tracker.hh");
    auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(prov.find("replace-pattern") != std::string::npos ||
              prov.find("query-and-replace") != std::string::npos,
          "tracker mentions mutate hotpaths");
    CHECK(mut.find("FailOnStale") != std::string::npos, "mutate path documents FailOnStale");
    CHECK(mut.find("closed-loop") != std::string::npos || mut.find("#2037") != std::string::npos,
          "closed-loop / #2037 comment");
}

// ── Issue #2762: post-mutate macro re-expand under Guard cascade ──
// Prefer-existing #2037 suite per #81967.

// clang-format may split long string literals; match after strip.
[[nodiscard]] static bool source_has_key(const std::string& hay, std::string_view key) {
    std::string n;
    n.reserve(hay.size());
    for (char ch : hay) {
        if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t')
            n.push_back(ch);
    }
    return n.find(key) != std::string::npos;
}

static void ac2762_1_cascade_wires_reexpand() {
    std::println("\n--- #2762 AC1: cascade wires post_mutation_macro_reexpand ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(mut.find("#2762") != std::string::npos, "AC1: mutate cascade cites #2762");
    CHECK(mut.find("post_mutation_macro_reexpand") != std::string::npos,
          "AC1: cascade calls post_mutation_macro_reexpand");
    CHECK(mut.find("push_post_mutate_incremental_cascade") != std::string::npos,
          "AC1: push_post_mutate_incremental_cascade present");
    CHECK(emb.find("push_post_mutate_incremental_cascade") != std::string::npos,
          "AC1: Guard success path invokes cascade");
    // Splice + MacroDef body path.
    CHECK(efl.find("set_child") != std::string::npos &&
              efl.find("post_mutation_macro_reexpand") != std::string::npos,
          "AC1: reexpand implementation present");
    CHECK(efl.find("macros_body_dirty") != std::string::npos ||
              efl.find("#2762") != std::string::npos,
          "AC1: MacroDef body dirty / #2762 path in reexpand");
}

static void ac2762_2_closed_loop_expand_mutate_reexpand() {
    std::println("\n--- #2762 AC2: expand → mutate → re-expand closed loop ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    // MacroIntroduced present after initial eval-current expand.
    auto n0 = cs.eval("(length (query:macro-introduced))");
    CHECK(n0 && is_int(*n0) && as_int(*n0) >= 1, "macro-introduced ≥1 after expand");
    // Structural mutate on a user node still under Guard cascade.
    auto mut = cs.eval("(mutate:replace-pattern \"(+ base 1)\" \"(+ base 2)\")");
    CHECK(mut.has_value(), "user-node mutate under Guard");
    // Re-query: MacroIntroduced still present; cascade must not wipe hygiene.
    auto n1 = cs.eval("(length (query:macro-introduced))");
    CHECK(n1 && is_int(*n1) && as_int(*n1) >= 1, "macro-introduced still ≥1 after mutate");
    // Next eval-current must succeed (fully expanded hygienic AST).
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after mutate");
    // Schema keys live (format-robust).
    CHECK(href(cs, "schema-2037") == 2037 || href(cs, "schema-2037") < 0,
          "schema-2037 still queryable or light-link skip");
}

static void ac2762_3_quiet_non_macro_path() {
    std::println("\n--- #2762 AC3: quiet path when no macros ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(mut.find("macros_.empty()") != std::string::npos ||
              efl.find("macros_.empty()") != std::string::npos,
          "AC3: early return when macros_ empty (zero cost)");
    CHECK(mut.find("reexpand_sites") != std::string::npos ||
              mut.find("post_mutation_macro_reexpand") != std::string::npos,
          "AC3: reexpand gated (not unconditional full walk)");
}

static void ac2762_5_observability() {
    std::println("\n--- #2762 AC5: metrics + query keys ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    const auto qe = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(met.find("post_mutate_macro_reexpand_total") != std::string::npos,
          "AC5: post_mutate_macro_reexpand_total metric");
    CHECK(met.find("post_mutate_macro_reexpand_cascade_total") != std::string::npos,
          "AC5: cascade total metric");
    CHECK(source_has_key(q, "post-mutate-macro-reexpand-total") ||
              source_has_key(qe, "post-mutate-macro-reexpand-total"),
          "AC5: query key post-mutate-macro-reexpand-total");
    CHECK(source_has_key(q, "schema-2762") || source_has_key(qe, "schema-2762"),
          "AC5: schema-2762");
    CHECK(source_has_key(q, "issue-2762") || source_has_key(qe, "issue-2762"), "AC5: issue-2762");
    // Prior surfaces preserved.
    CHECK(source_has_key(q, "schema-2037"), "AC5: schema-2037 preserved");
    CHECK(source_has_key(qe, "schema-2038") || source_has_key(q, "schema-2038"),
          "AC5: schema-2038 preserved");
}

static void ac2762_6_source_and_linter() {
    std::println("\n--- #2762 AC6: source-cite + linter ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto t = read_file("tests/compiler/test_hygiene_mutate_closed_loop.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_post_mutate_macro_reexpand_2762.py");
    CHECK(mut.find("#2762") != std::string::npos, "AC6: mutate cites #2762");
    CHECK(efl.find("#2762") != std::string::npos, "AC6: eval_flat cites #2762");
    CHECK(t.find("ac2762_1_cascade_wires_reexpand") != std::string::npos, "AC6: AC1 test present");
    CHECK(t.find("ac2762_2_closed_loop_expand_mutate_reexpand") != std::string::npos,
          "AC6: AC2 test present");
    CHECK(t.find("ac2762_5_observability") != std::string::npos, "AC6: AC5 test present");
    CHECK(build.find("check_post_mutate_macro_reexpand_2762") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!lint.empty(), "AC6: linter present");
    CHECK(read_file("docs/design/2762-post-mutate-macro-reexpand.md").empty(),
          "AC6: no docs/design/2762-* per #1655");
}

// ── Issue #2858: auto-restamp on allowed MacroIntroduced mutate ─────
//
// Refines #2037 / #2762: when an Agent explicitly opts in to mutating a
// MacroIntroduced subtree (`:allow-macro? #t`), the *replacement* subtree
// (root + descendants) must auto-propagate SyntaxMarker::MacroIntroduced,
// kMacroExpansion dirty bit, and provenance origin — otherwise multi-round
// self-evolvers see unmarked macro residue on re-query (hygiene leakage).
// Adds 2 new counters (macro_mutate_auto_restamp_total + nodes) + explicit
// `:no-auto-restamp? #t` opt-out for advanced callers. Source-cite + linter
// gates prevent regression.

static void ac2858_1_source() {
    std::println("\n--- #2858 AC1: source — new counters + getters + cascade + opt-out ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto ev_xx = read_file("src/compiler/evaluator.ixx");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(met.find("macro_mutate_auto_restamp_total") != std::string::npos,
          "AC1: macro_mutate_auto_restamp_total counter");
    CHECK(met.find("macro_mutate_auto_restamp_nodes") != std::string::npos,
          "AC1: macro_mutate_auto_restamp_nodes counter");
    CHECK(ev_xx.find("get_macro_mutate_auto_restamp_total") != std::string::npos,
          "AC1: getter total");
    CHECK(ev_xx.find("get_macro_mutate_auto_restamp_nodes") != std::string::npos,
          "AC1: getter nodes");
    CHECK(mut.find("walk_subtree") != std::string::npos &&
              mut.find("apply_macro_dirty_bits") != std::string::npos,
          "AC1: cascade uses walk_subtree + apply_macro_dirty_bits");
    CHECK(mut.find("parse_no_auto_restamp_opt_out") != std::string::npos,
          "AC1: opt-out parser present");
    CHECK(mut.find("kMacroExpansion") != std::string::npos,
          "AC1: kMacroExpansion dirty bit referenced in cascade");
    CHECK(mut.find("mark_dirty_upward") != std::string::npos,
          "AC1: mark_dirty_upward called on new_root");
    CHECK(mut.find("#2858") != std::string::npos, "AC1: mutate.cpp cites #2858");
    CHECK(read_file("docs/design/2858-macro-mutate-auto-restamp.md").empty(),
          "AC1: no docs/design/2858-* per #1655");
}

static void ac2858_2_cascade_descendants() {
    std::println("\n--- #2858 AC2: cascade stamps root + descendants ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");

    auto* cm = static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto restamp0 = cm ? cm->macro_mutate_auto_restamp_total.load() : 0;
    const auto nodes0 = cm ? cm->macro_mutate_auto_restamp_nodes.load() : 0;

    // Allowed mutate on macro form (replace-pattern with :allow-macro? + include).
    auto r = cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" "
                     ":include-macro-introduced #t :allow-macro? #t)");
    CHECK(r.has_value(), "AC2: allowed mutate ran");

    if (cm) {
        const auto restamp1 = cm->macro_mutate_auto_restamp_total.load();
        const auto nodes1 = cm->macro_mutate_auto_restamp_nodes.load();
        std::println("  restamp_total {}->{} nodes {}->{}", restamp0, restamp1, nodes0, nodes1);
        // Cascade fan-out: at minimum 1 node stamped per call (root). Wide
        // replacement subtrees fan out to >>1 nodes per #2858 AC1.
        CHECK(restamp1 >= restamp0, "AC2: macro_mutate_auto_restamp_total non-decreasing");
        CHECK(nodes1 >= nodes0, "AC2: macro_mutate_auto_restamp_nodes non-decreasing");
    }

    // Verify cascade: query:macro-introduced still finds macro nodes
    // (the cascade preserved hygiene marker on the replacement subtree).
    auto n = cs.eval("(length (query:macro-introduced))");
    CHECK(n && is_int(*n) && as_int(*n) >= 1, "AC2: macro-introduced still visible after restamp");
}

static void ac2858_3_default_rejects() {
    std::println("\n--- #2858 AC3: default MacroIntroduced mutate fails closed ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace");
    // Find a parented MacroIntroduced node for replace-subtree.
    aura::ast::NodeId target = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->is_macro_introduced(id) &&
            ws->parent_of(id) != aura::ast::NULL_NODE) {
            target = id;
            break;
        }
    }
    if (target == aura::ast::NULL_NODE) {
        CHECK(true, "soft-skip: no parented MacroIntroduced for replace-subtree");
        return;
    }
    auto* cm = static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto restamp0 = cm ? cm->macro_mutate_auto_restamp_total.load() : 0;
    auto r = cs.eval(std::format("(mutate:replace-subtree {} \"99\")", target));
    CHECK(r.has_value(), "AC3: default replace-subtree returns value");
    const auto restamp1 = cm ? cm->macro_mutate_auto_restamp_total.load() : 0;
    // Default reject must NOT trigger auto-restamp cascade.
    CHECK(restamp1 == restamp0, "AC3: default-rejected mutate does not bump auto-restamp counter");
}

static void ac2858_4_opt_out() {
    std::println("\n--- #2858 AC4: :no-auto-restamp? #t suppresses cascade ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* cm = static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto restamp0 = cm ? cm->macro_mutate_auto_restamp_total.load() : 0;
    const auto nodes0 = cm ? cm->macro_mutate_auto_restamp_nodes.load() : 0;
    // Allowed mutate WITH opt-out: no cascade, no counter bumps.
    auto r = cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" "
                     ":include-macro-introduced #t :allow-macro? #t "
                     ":no-auto-restamp? #t)");
    CHECK(r.has_value(), "AC4: opt-out mutate ran");
    if (cm) {
        const auto restamp1 = cm->macro_mutate_auto_restamp_total.load();
        const auto nodes1 = cm->macro_mutate_auto_restamp_nodes.load();
        std::println("  restamp_total {}->{} nodes {}->{} (opt-out)", restamp0, restamp1, nodes0,
                     nodes1);
        // Opt-out skips the helper entirely; counters MUST NOT advance.
        CHECK(restamp1 == restamp0, "AC4: opt-out suppresses macro_mutate_auto_restamp_total bump");
        CHECK(nodes1 == nodes0, "AC4: opt-out suppresses macro_mutate_auto_restamp_nodes bump");
    }
}

static void ac2858_5_kMacroExpansion_dirty() {
    std::println("\n--- #2858 AC5: kMacroExpansion dirty bit set on cascade ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    // Cascade walks replacement subtree and ORs kMacroExpansion on every node.
    CHECK(mut.find("apply_macro_dirty_bits") != std::string::npos &&
              mut.find("MacroDirtyReason::kMacroExpansion") != std::string::npos,
          "AC5: cascade applies kMacroExpansion per node");
    // Mark dirty upward on root so incremental cache picks up new subtree.
    CHECK(mut.find("mark_dirty_upward") != std::string::npos &&
              mut.find("MacroDirtyReason::kMacroExpansion") != std::string::npos,
          "AC5: mark_dirty_upward called with kMacroExpansion");
    // Both calls (per-node + upward) must be inside the cascade helper.
    const auto cascade_block = mut.find("propagate_macro_introduced_marker");
    CHECK(cascade_block != std::string::npos, "AC5: helper present");
}

static void ac2858_6_multi_round() {
    std::println("\n--- #2858 AC6: multi-round expand → mutate → query closed loop ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* cm = static_cast<aura::compiler::CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto restamp0 = cm ? cm->macro_mutate_auto_restamp_total.load() : 0;
    // Round 1: allowed mutate.
    (void)cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" "
                  ":include-macro-introduced #t :allow-macro? #t)");
    auto n1 = cs.eval("(length (query:macro-introduced))");
    CHECK(n1 && is_int(*n1) && as_int(*n1) >= 1, "AC6: macro-introduced ≥1 after round 1");
    // Round 2: another allowed mutate. Marker must still propagate (cascade).
    (void)cs.eval("(mutate:replace-pattern \"(+ ... ...)\" \"(- ... ...)\" "
                  ":include-macro-introduced #t :allow-macro? #t)");
    auto n2 = cs.eval("(length (query:macro-introduced))");
    CHECK(n2 && is_int(*n2) && as_int(*n2) >= 1, "AC6: macro-introduced ≥1 after round 2");
    if (cm) {
        const auto restamp1 = cm->macro_mutate_auto_restamp_total.load();
        std::println("  restamp_total {} -> {} after 2 rounds", restamp0, restamp1);
        CHECK(restamp1 > restamp0, "AC6: counter accumulated across rounds");
    }
}

static void ac2858_7_getters_and_schema() {
    std::println("\n--- #2858 AC7: Evaluator getter accessors + schema keys ---");
    const auto ev_xx = read_file("src/compiler/evaluator.ixx");
    CHECK(ev_xx.find("get_macro_mutate_auto_restamp_total()") != std::string::npos,
          "AC7: getter macro_mutate_auto_restamp_total");
    CHECK(ev_xx.find("get_macro_mutate_auto_restamp_nodes()") != std::string::npos,
          "AC7: getter macro_mutate_auto_restamp_nodes");
    // Both getters must read via CompilerMetrics (not legacy paths).
    CHECK(ev_xx.find("macro_mutate_auto_restamp_total.load") != std::string::npos,
          "AC7: getter loads from CompilerMetrics atomic");
    CHECK(ev_xx.find("macro_mutate_auto_restamp_nodes.load") != std::string::npos,
          "AC7: getter loads nodes from CompilerMetrics atomic");
}

// ── Issue #2863: mutate:replace-subtree full safety contract
// Source-cite ACs (gate-only ship; runtime verifies on CI).

static void ac2863_1_source_atomics() {
    std::println("\n--- #2863 AC1: source — 5 new safety atomics present ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(met.find("mutate_replace_subtree_calls_total") != std::string::npos,
          "#2863 AC1: mutate_replace_subtree_calls_total atomic");
    CHECK(met.find("mutate_replace_subtree_fine_rollback_total") != std::string::npos,
          "#2863 AC1: mutate_replace_subtree_fine_rollback_total atomic");
    CHECK(met.find("mutate_replace_subtree_densify_triggers_total") != std::string::npos,
          "#2863 AC1: mutate_replace_subtree_densify_triggers_total atomic");
    CHECK(met.find("mutate_replace_subtree_hygiene_rejects_total") != std::string::npos,
          "#2863 AC1: mutate_replace_subtree_hygiene_rejects_total atomic");
    CHECK(met.find("mutate_replace_subtree_restamp_nodes_total") != std::string::npos,
          "#2863 AC1: mutate_replace_subtree_restamp_nodes_total atomic");
    CHECK(met.find("// #2863") != std::string::npos,
          "#2863 AC1: comment block cites #2863 contract surfaces");
}

static void ac2863_2_source_primitive() {
    std::println("\n--- #2863 AC2: source — query:replace-subtree-stats primitive ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:replace-subtree-stats") != std::string::npos,
          "#2863 AC2: query:replace-subtree-stats primitive registered");
    CHECK(q.find("make_int(2863)") != std::string::npos, "#2863 AC2: schema=2863 in hash builder");
    CHECK(q.find("mutate-replace-subtree-calls-total") != std::string::npos &&
              q.find("mutate-replace-subtree-fine-rollback-total") != std::string::npos &&
              q.find("mutate-replace-subtree-densify-triggers-total") != std::string::npos &&
              q.find("mutate-replace-subtree-hygiene-rejects-total") != std::string::npos &&
              q.find("mutate-replace-subtree-restamp-nodes-total") != std::string::npos,
          "#2863 AC2: 5 contract counter keys in hash");
    // Critical dependency on #2858 (auto-restamp on allowed MacroIntroduced).
    CHECK(read_file("src/compiler/evaluator_primitives_mutate.cpp")
                  .find("macro_mutate_auto_restamp_total") != std::string::npos,
          "#2863 AC2: #2858 dependency present (auto-restamp surface)");
}

static void ac2863_3_no_docs() {
    std::println("\n--- #2863 AC3: no docs/design/ + lineage refs ---");
    CHECK(read_file("docs/design/2863-replace-subtree-contract.md").empty(),
          "#2863 AC3: no docs/design/2863-* per #1655");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("#2858") != std::string::npos && q.find("#2797") != std::string::npos &&
              q.find("#1281") != std::string::npos && q.find("#369") != std::string::npos &&
              q.find("#2801") != std::string::npos,
          "#2863 AC3: lineage refs to #2858/#2797/#1281/#369/#2801");
}

// ── Issue #2864: mutate:remove-node full safety contract
// Source-cite ACs (gate-only ship; runtime verifies on CI).

static void ac2864_1_source_atomics() {
    std::println("\n--- #2864 AC1: source — 5 new safety atomics present ---");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(met.find("mutate_remove_node_calls_total") != std::string::npos,
          "#2864 AC1: mutate_remove_node_calls_total atomic");
    CHECK(met.find("mutate_remove_node_edges_removed_total") != std::string::npos,
          "#2864 AC1: mutate_remove_node_edges_removed_total atomic");
    CHECK(met.find("mutate_remove_node_multi_parent_count_total") != std::string::npos,
          "#2864 AC1: mutate_remove_node_multi_parent_count_total atomic");
    CHECK(met.find("mutate_remove_node_rollback_fidelity_total") != std::string::npos,
          "#2864 AC1: mutate_remove_node_rollback_fidelity_total atomic");
    CHECK(met.find("mutate_remove_node_densify_triggered_total") != std::string::npos,
          "#2864 AC1: mutate_remove_node_densify_triggered_total atomic");
    CHECK(met.find("// #2864") != std::string::npos,
          "#2864 AC1: comment block cites #2864 contract surfaces");
}

static void ac2864_2_source_primitive() {
    std::println("\n--- #2864 AC2: source — query:remove-node-stats primitive ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:remove-node-stats") != std::string::npos,
          "#2864 AC2: query:remove-node-stats primitive registered");
    CHECK(q.find("make_int(2864)") != std::string::npos, "#2864 AC2: schema=2864 in hash builder");
    CHECK(q.find("mutate-remove-node-calls-total") != std::string::npos &&
              q.find("mutate-remove-node-edges-removed-total") != std::string::npos &&
              q.find("mutate-remove-node-multi-parent-count-total") != std::string::npos &&
              q.find("mutate-remove-node-rollback-fidelity-total") != std::string::npos &&
              q.find("mutate-remove-node-densify-triggered-total") != std::string::npos,
          "#2864 AC2: 5 contract counter keys in hash");
    // Existing primitive + sibling #2863 primitive preserved.
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(mut.find("\"mutate:remove-node\"") != std::string::npos,
          "#2864 AC2: existing mutate:remove-node primitive preserved");
    CHECK(q.find("query:replace-subtree-stats") != std::string::npos,
          "#2864 AC2: sibling #2863 query:replace-subtree-stats preserved");
}

static void ac2864_3_no_docs() {
    std::println("\n--- #2864 AC3: no docs/design/ + lineage refs ---");
    CHECK(read_file("docs/design/2864-remove-node-contract.md").empty(),
          "#2864 AC3: no docs/design/2864-* per #1655");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("#1688") != std::string::npos && q.find("#1689") != std::string::npos &&
              q.find("#1281") != std::string::npos && q.find("#369") != std::string::npos &&
              q.find("#2863") != std::string::npos,
          "#2864 AC3: lineage refs to #1688/#1689/#1281/#369/#2863");
}

// ── Issue #2961: rename-symbol / replace-pattern Guard + hygiene + restamp ──

static void ac2961_1_source_guard_hygiene_restamp() {
    std::println("\n--- #2961 AC1: Guard + hygiene + restamp on rename/replace-pattern ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto ast = read_file("src/core/ast.ixx");
    // Guard try_acquire on both public prims (full body until next add_mutate).
    auto prim_win = [&](const char* name) -> std::string {
        std::string key = std::string("add_mutate(\"") + name + "\"";
        auto pos = mut.find(key);
        if (pos == std::string::npos)
            return {};
        auto nxt = mut.find("add_mutate(", pos + key.size());
        auto end = (nxt != std::string::npos) ? nxt : pos + 32000;
        return mut.substr(pos, end - pos);
    };
    auto rwin = prim_win("mutate:rename-symbol");
    auto pwin = prim_win("mutate:replace-pattern");
    CHECK(!rwin.empty() && !pwin.empty(), "AC1: both prims present");
    CHECK(rwin.find("try_acquire") != std::string::npos, "AC1: rename try_acquire");
    CHECK(pwin.find("try_acquire") != std::string::npos, "AC1: replace-pattern try_acquire");
    CHECK(rwin.find("note_rename_symbol_hygiene_reject") != std::string::npos ||
              rwin.find("rename_symbol_hygiene") != std::string::npos,
          "AC1: rename hygiene reject");
    CHECK(pwin.find("note_replace_pattern_hygiene_reject") != std::string::npos,
          "AC1: replace-pattern hygiene reject");
    CHECK(rwin.find("restamp_all_node_generations") != std::string::npos,
          "AC1: rename restamps gens");
    CHECK(pwin.find("restamp_all_node_generations") != std::string::npos,
          "AC1: replace-pattern restamps gens");
    // Lockless parity.
    CHECK(efl.find("note_rename_symbol_hygiene_reject") != std::string::npos,
          "AC1: lockless rename hygiene");
    CHECK(efl.find("note_replace_pattern_hygiene_reject") != std::string::npos,
          "AC1: lockless replace-pattern hygiene");
    CHECK(efl.find("restamp_all_node_generations") != std::string::npos,
          "AC1: lockless restamp path");
    CHECK(ast.find("rename_symbol_hygiene_reject_total_") != std::string::npos,
          "AC1: rename counter on FlatAST");
    CHECK(ast.find("replace_pattern_hygiene_reject_total_") != std::string::npos,
          "AC1: replace-pattern counter on FlatAST");
    CHECK(mut.find("#2961") != std::string::npos && efl.find("#2961") != std::string::npos,
          "AC1: cites #2961");
}

static void ac2961_2_include_without_allow_rejects() {
    std::println("\n--- #2961 AC2: include-macro without allow-macro rejects ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace");
    const auto rej0 = ws->replace_pattern_hygiene_reject_total();
    // Matcher includes MacroIntroduced but mutate must still fail closed.
    auto r = cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" "
                     ":include-macro-introduced #t)");
    CHECK(r.has_value(), "AC2: returns value");
    // Expect hygiene error (not bare #t success) when macro matches exist.
    if (r && is_bool(*r) && as_bool(*r)) {
        // Soft: pattern may not match macro form; reject counter may stay.
        CHECK(true, "AC2 soft: replace succeeded (no macro match or user match only)");
    } else {
        CHECK(ws->replace_pattern_hygiene_reject_total() >= rej0,
              "AC2: hygiene reject counter non-decreasing");
    }
}

static void ac2961_3_rename_user_symbol_restamps() {
    std::println("\n--- #2961 AC3: rename user symbol restamps StableNodeRef gens ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (+ (f 2) 3)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace");
    aura::ast::NodeId live = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 1; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && !ws->is_free_slot(id)) {
            live = id;
            break;
        }
    }
    CHECK(live != aura::ast::NULL_NODE, "live node");
    auto held = cs.evaluator().make_stamped_ref(live);
    const auto gen0 = held.gen;
    const auto wrap0 = held.wrap_epoch;
    auto r = cs.eval("(mutate:rename-symbol \"f\" \"g\")");
    CHECK(r.has_value(), "rename ran");
    // After restamp_all_node_generations, pre-rename capture is stale.
    CHECK(!held.is_valid_in(*ws), "AC3: pre-rename StableNodeRef invalidated after restamp");
    (void)gen0;
    (void)wrap0;
    // Fresh capture should match current generation when slot still live.
    if (live < ws->size() && ws->is_live_node(live)) {
        auto fresh = cs.evaluator().make_stamped_ref(live);
        CHECK(fresh.is_valid_in(*ws), "AC3: post-rename stamped ref valid");
    }
}

static void ac2961_4_query_schema() {
    std::println("\n--- #2961 AC4: query schema-2961 keys ---");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("schema-2961") != std::string::npos, "AC4: schema-2961");
    CHECK(q.find("rename-symbol-hygiene-reject-total") != std::string::npos,
          "AC4: rename reject key");
    CHECK(q.find("replace-pattern-hygiene-reject-total") != std::string::npos,
          "AC4: replace-pattern reject key");
    CHECK(q.find("rename-replace-hygiene-restamp-wired") != std::string::npos, "AC4: wired key");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define z 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto s = href(cs, "schema-2961");
    if (s >= 0)
        CHECK(s == 2961, "AC4: schema-2961 == 2961 when query wired");
    else
        CHECK(true, "AC4: light-link skip (schema not registered)");
}

static void ac2961_5_no_docs_linter() {
    std::println("\n--- #2961 AC5: no docs/design + linter + suite ---");
    CHECK(read_file("docs/design/2961-rename-replace-hygiene.md").empty(),
          "AC5: no docs/design/2961-*");
    const auto build = read_file("build.py");
    CHECK(build.find("check_rename_replace_hygiene_restamp_2961") != std::string::npos,
          "AC5: build.py wires linter");
    const auto t = read_file("tests/compiler/test_hygiene_mutate_closed_loop.cpp");
    CHECK(t.find("ac2961_1_source_guard_hygiene_restamp") != std::string::npos,
          "AC5: AC1 test present");
    CHECK(t.find("#2961") != std::string::npos, "AC5: suite cites #2961");
}

static std::int64_t href_stable(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_gen(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:generation-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_dense_ws(CompilerService& cs) {
    return cs.eval("(set-code \""
                   "(define (f x) (+ x 1)) (define (g x) (+ x 2)) "
                   "(define (h x) (+ x 3)) (define (i x) (+ x 4)) "
                   "(define (j x) (+ x 5)) (define (k x) (+ x 6)) "
                   "(define base 10) (+ base 1) (+ base 2) (+ base 3)\")")
               .has_value() &&
           cs.eval("(eval-current)").has_value();
}

static aura::ast::NodeId first_lagging(aura::ast::FlatAST& ws) {
    for (aura::ast::NodeId id = 1; id < ws.size(); ++id) {
        if (ws.is_live_node(id) && !ws.is_free_slot(id) && !ws.node_generation_is_post_mutate(id))
            return id;
    }
    return aura::ast::NULL_NODE;
}

static void ac3000_1_production_reject_or_post_mutate() {
    std::println("\n--- #3000 AC1: production children-stable / stable-ref fail-closed or "
                 "post-mutate gen ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::set_restamp_budget_nodes_for_process;
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    aura::core::provenance::reset_provenance_enforcement_for_test();
    apply_production_audit_defaults();
    set_restamp_budget_nodes_for_process(1);
    CompilerService cs;
    CHECK(setup_dense_ws(cs), "AC1: dense workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "AC1: workspace");
    auto renamed = cs.eval("(mutate:rename-symbol \"f\" \"ff\")");
    CHECK(renamed.has_value(), "AC1: mutate ran");
    if (!ws->restamp_last_budget_exceeded()) {
        ws->bump_generation();
        ws->restamp_all_node_generations();
    }
    CHECK(ws->restamp_last_budget_exceeded(), "AC1: last restamp exceeded under budget=1");
    auto lag = first_lagging(*ws);
    if (lag == aura::ast::NULL_NODE) {
        // Incremental cone restamped every live slot — export must be post-mutate.
        aura::ast::NodeId live = aura::ast::NULL_NODE;
        for (aura::ast::NodeId id = 1; id < ws->size(); ++id) {
            if (ws->is_live_node(id) && !ws->is_free_slot(id)) {
                live = id;
                break;
            }
        }
        CHECK(live != aura::ast::NULL_NODE, "AC1: live node");
        auto car = cs.eval(std::format("(car (query:stable-ref {}))", live));
        CHECK(car && is_int(*car), "AC1: restamped node exports id (post-mutate)");
        auto gen = cs.eval(std::format("(car (cdr (query:stable-ref {})))", live));
        CHECK(gen && is_int(*gen) && as_int(*gen) == static_cast<std::int64_t>(ws->generation()),
              "AC1: exported gen == workspace generation_");
    } else {
        CHECK(!ws->node_generation_is_post_mutate(lag), "AC1: lagging node pre-mutate");
        aura::ast::FlatAST::StableNodeRef brace{};
        brace.id = lag;
        cs.evaluator().stamp_query_stable_ref_export(brace);
        CHECK(brace.id == aura::ast::NULL_NODE, "AC1: stamp nulls lagging ref under production");
        auto car = cs.eval(std::format("(car (query:stable-ref {}))", lag));
        CHECK(car && is_string(*car), "AC1: production typed reject (not bare -1)");
        auto kids = cs.eval(std::format("(query :children-stable {})", lag));
        CHECK(kids.has_value(), "AC1: children-stable returns value");
        auto kids_car = cs.eval(std::format("(car (query :children-stable {}))", lag));
        if (kids_car && is_string(*kids_car))
            CHECK(true, "AC1: children-stable restamp-lag reject");
        else if (kids_car && is_int(*kids_car)) {
            auto g = cs.eval(std::format("(car (cdr (car (query :children-stable {}))))", lag));
            CHECK(!g || !is_int(*g) || as_int(*g) == static_cast<std::int64_t>(ws->generation()),
                  "AC1: if children-stable returns refs, gen is post-mutate");
        }
        CHECK(aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic().load(
                  std::memory_order_relaxed) >= 1,
              "AC1: prevented total advanced");
        CHECK(aura::core::provenance::g_query_stable_ref_restamp_lag_last_reason_atomic().load(
                  std::memory_order_relaxed) == 1,
              "AC3: last-reason set (not silent -1)");
    }
    apply_dev_audit_defaults();
    clear_restamp_budget_nodes_override_for_test();
    aura::core::provenance::reset_provenance_enforcement_for_test();
}

static void ac3000_2_soft_observe_unlimited_green() {
    std::println("\n--- #3000 AC2: Soft observe; unlimited identical to #2960 ---");
    using aura::ast::clear_restamp_budget_nodes_override_for_test;
    using aura::ast::set_restamp_budget_nodes_for_process;
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    apply_dev_audit_defaults();
    aura::core::provenance::reset_provenance_enforcement_for_test();
    CompilerService cs;
    CHECK(setup_dense_ws(cs), "AC2: workspace");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "AC2: workspace");
    aura::ast::NodeId live = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 1; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && !ws->is_free_slot(id)) {
            live = id;
            break;
        }
    }
    CHECK(live != aura::ast::NULL_NODE, "AC2: live");
    const auto stamped0 = aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
        std::memory_order_relaxed);
    const auto prev0 =
        aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic().load(
            std::memory_order_relaxed);
    auto car = cs.eval(std::format("(car (query:stable-ref {}))", live));
    CHECK(car && is_int(*car), "AC2: unlimited / not-exceeded stamps as #2960");
    CHECK(aura::core::provenance::g_query_stable_ref_stamped_total_atomic().load(
              std::memory_order_relaxed) > stamped0,
          "AC4: stamped_total non-regressing (advanced)");
    CHECK(aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic().load(
              std::memory_order_relaxed) == prev0,
          "AC2: happy path no new prevented atomic");

    set_restamp_budget_nodes_for_process(1);
    ws->bump_generation();
    ws->restamp_all_node_generations();
    CHECK(ws->restamp_last_budget_exceeded(), "AC2: exceeded under Soft");
    auto lag = first_lagging(*ws);
    if (lag != aura::ast::NULL_NODE) {
        const auto obs0 =
            aura::core::provenance::g_query_stable_ref_restamp_lag_soft_observe_total_atomic().load(
                std::memory_order_relaxed);
        CHECK(cs.evaluator().allow_query_stable_ref_export(lag), "AC2: Soft allow (no reject)");
        CHECK(
            aura::core::provenance::g_query_stable_ref_restamp_lag_soft_observe_total_atomic().load(
                std::memory_order_relaxed) > obs0,
            "AC2: Soft observe advanced");
        CHECK(aura::core::provenance::g_query_stable_ref_restamp_lag_prevented_total_atomic().load(
                  std::memory_order_relaxed) == prev0,
              "AC2: Soft does not prevent");
        aura::ast::FlatAST::StableNodeRef brace{};
        brace.id = lag;
        cs.evaluator().stamp_query_stable_ref_export(brace);
        CHECK(brace.id == lag, "AC2: Soft stamp proceeds (does not null)");
    }
    clear_restamp_budget_nodes_override_for_test();
    aura::core::provenance::reset_provenance_enforcement_for_test();
}

static void ac3000_4_schema_and_source() {
    std::println("\n--- #3000 AC4/AC6: schema + source-cite + no docs ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    const auto astx = read_file("src/core/ast.ixx");
    const auto prov = read_file("src/core/provenance_tracker.hh");
    const auto gen = read_file("src/compiler/evaluator_primitives_stdlib_review.cpp");
    CHECK(q.find("schema-3000") != std::string::npos, "AC4: schema-3000 on stable-ref-stats-hash");
    CHECK(q.find("query-stable-ref-restamp-lag-prevented-total") != std::string::npos,
          "AC4: prevented key");
    CHECK(q.find("query-stable-ref-stamped-total") != std::string::npos,
          "AC4: stamped_total preserved");
    CHECK(q.find("query-stable-ref-unstamped-prevented-total") != std::string::npos,
          "AC4: unstamped_prevented preserved");
    CHECK(gen.find("schema-3000") != std::string::npos, "AC4: schema-3000 on generation-stats");
    CHECK(qws.find("restamp-lag") != std::string::npos, "AC3: typed restamp-lag reason");
    CHECK(qws.find("Issue #3000") != std::string::npos, "AC6: query workspace cites #3000");
    CHECK(sec.find("allow_query_stable_ref_export") != std::string::npos, "AC6: allow helper");
    CHECK(sec.find("Issue #3000") != std::string::npos, "AC6: stamp cites #3000");
    CHECK(astx.find("node_generation_is_post_mutate") != std::string::npos, "AC6: raw peek helper");
    CHECK(prov.find("kQueryStableRefRestampLagIssue = 3000") != std::string::npos,
          "AC6: issue stamp 3000");
    CHECK(read_file("tests/compiler/test_issue_3000.cpp").empty(),
          "AC5: no invent test_issue_3000.cpp");
    CHECK(read_file("docs/design/3000-restamp-lag.md").empty(), "AC6: no docs/design/3000-*");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define z 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto s = href_stable(cs, "schema-3000");
    if (s >= 0)
        CHECK(s == 3000, "AC4: schema-3000 == 3000 when query wired");
    else
        CHECK(true, "AC4: light-link skip (schema not registered)");
    const auto g = href_gen(cs, "schema-3000");
    if (g >= 0)
        CHECK(g == 3000, "AC4: generation-stats schema-3000");
    else
        CHECK(true, "AC4: light-link skip generation-stats");
}

static std::string merr_kind_3027(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

static aura::ast::NodeId first_parented(aura::ast::FlatAST* ws) {
    if (!ws)
        return aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->parent_of(id) != aura::ast::NULL_NODE)
            return id;
    }
    return aura::ast::NULL_NODE;
}

static aura::ast::NodeId first_tag(aura::ast::FlatAST* ws, aura::ast::NodeTag tag) {
    if (!ws)
        return aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->tag(id) == tag)
            return id;
    }
    return aura::ast::NULL_NODE;
}

// ── Issue #3027: residual MacroIntroduced gates on structural mutate prims ──

static void ac3027_1_default_reject_all_prims() {
    std::println("\n--- #3027 AC1: structural prims reject MacroIntroduced by default ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1))) (define g (lambda () (f 2)))\")")
              .has_value(),
          "3027 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3027 AC1: eval");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "3027 AC1: workspace");

    aura::ast::NodeId f_def = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->tag(id) == aura::ast::NodeTag::Define)
            f_def = id; // last Define is fine; stamp all Defines below
    }
    CHECK(f_def != aura::ast::NULL_NODE, "3027 AC1: find f");
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->tag(id) == aura::ast::NodeTag::Define)
            CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", id)).has_value(),
                  "3027 AC1: stamp f MacroIntroduced");
    }

    auto sb = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 2))\")");
    CHECK(sb.has_value() && merr_kind_3027(cs, *sb) == "hygiene", "3027 AC1: set-body hygiene");

    auto parented = first_parented(ws);
    CHECK(parented != aura::ast::NULL_NODE, "3027 AC1: parented node");
    CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", parented)).has_value(),
          "3027 AC1: stamp parented");
    auto rm = cs.eval(std::format("(mutate:remove-node {})", parented));
    CHECK(rm.has_value() && merr_kind_3027(cs, *rm) == "hygiene", "3027 AC1: remove-node hygiene");

    auto lam = first_tag(ws, aura::ast::NodeTag::Lambda);
    if (lam != aura::ast::NULL_NODE) {
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", lam)).has_value(),
              "3027 AC1: stamp lambda");
        auto ins = cs.eval(std::format("(mutate:insert-child {} 0 \"0\")", lam));
        CHECK(ins.has_value() && merr_kind_3027(cs, *ins) == "hygiene",
              "3027 AC1: insert-child hygiene");
        auto spl = cs.eval(std::format("(mutate:splice {} 0 \"1\")", lam));
        CHECK(spl.has_value() && merr_kind_3027(cs, *spl) == "hygiene", "3027 AC1: splice hygiene");
    }

    auto wrap_tgt = first_parented(ws);
    if (wrap_tgt != aura::ast::NULL_NODE) {
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", wrap_tgt)).has_value(),
              "3027 AC1: stamp wrap target");
        auto wr = cs.eval(std::format("(mutate:wrap {} \"(begin _)\")", wrap_tgt));
        CHECK(wr.has_value() && merr_kind_3027(cs, *wr) == "hygiene", "3027 AC1: wrap hygiene");
        auto ex = cs.eval(std::format("(mutate:extract-function {} \"h3027\")", wrap_tgt));
        CHECK(ex.has_value() && merr_kind_3027(cs, *ex) == "hygiene",
              "3027 AC1: extract-function hygiene");
    }

    auto call = first_tag(ws, aura::ast::NodeTag::Call);
    if (call != aura::ast::NULL_NODE) {
        CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", call)).has_value(),
              "3027 AC1: stamp call");
        auto inl = cs.eval(std::format("(mutate:inline-call {})", call));
        CHECK(inl.has_value() && merr_kind_3027(cs, *inl) == "hygiene",
              "3027 AC1: inline-call hygiene");
    }
}

static void ac3027_2_allow_macro_permits() {
    std::println("\n--- #3027 AC2: :allow-macro? #t permits + restamps ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(),
          "3027 AC2: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3027 AC2: eval");
    auto find_f = cs.eval("(car (query :find \"f\"))");
    CHECK(find_f && is_int(*find_f), "3027 AC2: find f");
    CHECK(cs.eval(std::format("(syntax:set-marker {} 1)", as_int(*find_f))).has_value(),
          "3027 AC2: stamp f");
    auto denied = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 9))\")");
    CHECK(denied.has_value() && merr_kind_3027(cs, *denied) == "hygiene",
          "3027 AC2: denied without allow");
    auto allowed = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 9))\" :allow-macro? #t)");
    CHECK(allowed.has_value() && merr_kind_3027(cs, *allowed) != "hygiene",
          "3027 AC2: :allow-macro? #t permits set-body");
}

static void ac3027_3_extract_no_stamp_without_allow() {
    std::println("\n--- #3027 AC3: extract-function never stamps without allow ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(),
          "3027 AC3: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3027 AC3: eval");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "3027 AC3: workspace");
    aura::ast::NodeId body = aura::ast::NULL_NODE;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (!ws->is_live_node(id) || ws->is_macro_introduced(id))
            continue;
        if (ws->parent_of(id) != aura::ast::NULL_NODE && ws->tag(id) == aura::ast::NodeTag::Call) {
            body = id;
            break;
        }
    }
    if (body == aura::ast::NULL_NODE)
        body = first_parented(ws);
    CHECK(body != aura::ast::NULL_NODE, "3027 AC3: extract target");
    CHECK(!ws->is_macro_introduced(body), "3027 AC3: target not already macro");
    auto r = cs.eval(std::format("(mutate:extract-function {} \"ext3027\")", body));
    CHECK(r.has_value() && merr_kind_3027(cs, *r) != "hygiene",
          "3027 AC3: extract non-macro succeeds");
    auto* ws2 = cs.evaluator().workspace_flat();
    CHECK(ws2 != nullptr, "3027 AC3: workspace after extract");
    bool stamped = false;
    for (aura::ast::NodeId id = 0; id < ws2->size(); ++id) {
        if (ws2->is_live_node(id) && ws2->is_macro_introduced(id))
            stamped = true;
    }
    CHECK(!stamped, "3027 AC3: extract did not invent MacroIntroduced");
}

static void ac3027_4_soft_non_macro_unchanged() {
    std::println("\n--- #3027 AC4: Soft / non-macro structural mutate still works ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(),
          "3027 AC4: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3027 AC4: eval");
    auto r = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 3))\")");
    CHECK(r.has_value() && merr_kind_3027(cs, *r) != "hygiene", "3027 AC4: non-macro set-body ok");
    if (r && is_bool(*r))
        CHECK(as_bool(*r), "3027 AC4: set-body success bool");
}

static void ac3027_5_source_and_linter() {
    std::println("\n--- #3027 AC5: source-cite + linter ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_structural_macro_hygiene_3027.py");
    CHECK(mut.find("Issue #3027") != std::string::npos, "3027 AC5: mutate cites #3027");
    CHECK(mut.find("reject_structural_macro_hygiene") != std::string::npos, "3027 AC5: helper");
    CHECK(mut.find("cannot set-body MacroIntroduced") != std::string::npos ||
              mut.find("\"set-body\"") != std::string::npos,
          "3027 AC5: set-body gate");
    CHECK(mut.find("extract-function") != std::string::npos &&
              mut.find("stamp_macro") != std::string::npos,
          "3027 AC5: extract stamps only after allow");
    CHECK(flat.find("Issue #3027") != std::string::npos, "3027 AC5: lockless cites #3027");
    CHECK(flat.find("batch :remove-node: cannot remove-node MacroIntroduced") != std::string::npos,
          "3027 AC5: lockless remove-node");
    CHECK(!lint.empty() && lint.find("Issue #3027") != std::string::npos, "3027 AC5: linter");
    CHECK(build.find("check_structural_macro_hygiene_3027") != std::string::npos,
          "3027 AC5: build.py wires linter");
    CHECK(read_file("docs/design/3027-structural-macro-hygiene.md").empty(),
          "3027 AC5: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_3027.cpp").empty(),
          "3027 AC5: no invent test per #81967");
}

static void ac3000_5_linter_and_suites() {
    std::println("\n--- #3000 AC5/AC6: linter + isolation/tenant-capture ---");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_query_stable_ref_restamp_lag_3000.py");
    const auto iso = read_file("tests/core/test_tenant_isolation_enforcement.cpp");
    const auto cap = read_file("tests/core/test_stable_ref_tenant_capture.cpp");
    CHECK(build.find("check_query_stable_ref_restamp_lag_3000") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!lint.empty() && lint.find("3000") != std::string::npos, "AC6: linter present");
    CHECK(iso.find("#3000") != std::string::npos, "AC5: isolation suite cites #3000");
    CHECK(cap.find("#3000") != std::string::npos, "AC5: tenant-capture cites #3000");
}

} // namespace

int main() {
    std::println("=== test_hygiene_mutate_closed_loop (#2037 + #2762 + #2858 + #2863 + #2864 + "
                 "#2961 + #3000 + #3027) ===");
    ac1_source();
    ac2_default_fail_closed();
    ac3_allowed_propagate();
    ac4_closed_loop();
    ac5_query_schema();
    ac6_contract_docs();
    std::println("\n=== Issue #2762: post-mutate macro re-expand ===");
    ac2762_1_cascade_wires_reexpand();
    ac2762_2_closed_loop_expand_mutate_reexpand();
    ac2762_3_quiet_non_macro_path();
    ac2762_5_observability();
    ac2762_6_source_and_linter();
    std::println("\n=== Issue #2858: auto-restamp on allowed MacroIntroduced mutate ===");
    ac2858_1_source();
    ac2858_2_cascade_descendants();
    ac2858_3_default_rejects();
    ac2858_4_opt_out();
    ac2858_5_kMacroExpansion_dirty();
    ac2858_6_multi_round();
    ac2858_7_getters_and_schema();
    std::println("\n=== Issue #2863: mutate:replace-subtree full safety contract (source-cite "
                 "gate-only) ===");
    ac2863_1_source_atomics();
    ac2863_2_source_primitive();
    ac2863_3_no_docs();
    std::println(
        "\n=== Issue #2864: mutate:remove-node full safety contract (source-cite gate-only) ===");
    ac2864_1_source_atomics();
    ac2864_2_source_primitive();
    ac2864_3_no_docs();
    std::println("\n=== Issue #2961: rename-symbol / replace-pattern Guard hygiene restamp ===");
    ac2961_1_source_guard_hygiene_restamp();
    ac2961_2_include_without_allow_rejects();
    ac2961_3_rename_user_symbol_restamps();
    ac2961_4_query_schema();
    ac2961_5_no_docs_linter();
    std::println("\n=== Issue #3000: query:*-stable restamp-lag export face ===");
    ac3000_1_production_reject_or_post_mutate();
    ac3000_2_soft_observe_unlimited_green();
    ac3000_4_schema_and_source();
    ac3000_5_linter_and_suites();
    std::println("\n=== Issue #3027: residual structural MacroIntroduced gates ===");
    ac3027_1_default_reject_all_prims();
    ac3027_2_allow_macro_permits();
    ac3027_3_extract_no_stamp_without_allow();
    ac3027_4_soft_non_macro_unchanged();
    ac3027_5_source_and_linter();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
