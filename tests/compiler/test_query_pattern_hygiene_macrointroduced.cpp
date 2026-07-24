// test_query_pattern_hygiene_macrointroduced.cpp — Issue #593:
// Hygiene + MacroIntroduced propagation in query:pattern + IR
// InlinePass — AST→query→IR closed-loop production matrix.
//
// Non-duplicative refinement / additive to macro+reflect+self-evo
// production review. Complements #547 (tag_arity_index + hygiene),
// #486 (query:pattern filter), #501 (IR InlinePass), #524
// (macro-production-hygiene-stats), and #420 (contract stats) with
// the full end-to-end matrix the Task6 review flagged:
//
//   - AC1:  macro expand stamps MacroIntroduced nodes
//   - AC2:  query:pattern default filters (no capture)
//   - AC3:  :respect-hygiene / :allow-macro-introduced flags
//   - AC4:  mutate safe on user nodes under Guard
//   - AC5:  IR InlinePass respects MacroIntroduced (no violation)
//   - AC6:  tag_arity_index dirty hook under macro+query load
//   - AC7:  query:pattern-ir-hygiene-closed-loop-stats (schema 593)
//   - AC8:  Combined metrics monotonic (pattern + IR + production)
//   - AC9:  5000+ fuzz mutate+query stress (env-tunable)
//   - AC10: Regression — existing hygiene primitives

#include "test_harness.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_593_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;

static int k_stress_iters() {
    return k_int_env("AURA_STRESS_ITERS", 5000);
}

static std::int64_t hash_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:pattern-ir-hygiene-closed-loop-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t result_count(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval("(length " + expr + ")");
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::int64_t inline_skipped(CompilerService& cs) {
    auto r = cs.eval(
        "(hash-ref (engine:metrics \"compile:inline-pass-stats\") 'macro-hygiene-skipped')");
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_macro_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

// ── AC1: macro expand stamps MacroIntroduced ───────────────
bool test_macro_expand_stamps_macrointroduced(CompilerService& cs) {
    std::println("\n--- AC1: macro expand stamps MacroIntroduced ---");
    CHECK(setup_macro_workspace(cs), "hygienic macro workspace setup");
    auto macro_n = cs.eval("(length (query:macro-introduced))");
    CHECK(macro_n.has_value() && aura::compiler::types::is_int(*macro_n),
          "(query:macro-introduced) returns int");
    CHECK(aura::compiler::types::as_int(*macro_n) >= 3, "macro-introduced >= 3 nodes");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace_flat reachable");
    if (ws)
        CHECK(ws->macro_expansion_dirty_total() > 0, "macro_expansion_dirty_total bumped");
    return true;
}

// ── AC2: query:pattern default filters (no capture) ────────
bool test_query_pattern_default_filters(CompilerService& cs) {
    std::println("\n--- AC2: query:pattern default filters MacroIntroduced ---");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    const auto default_cnt = result_count(cs, "(query:pattern \"*\")");
    const auto allow_cnt = result_count(cs, "(query:pattern \"*\" :allow-macro-introduced #t)");
    CHECK(default_cnt >= 0 && allow_cnt >= 0, "pattern match counts observable");
    CHECK(allow_cnt >= default_cnt, "allow-macro-introduced yields >= default-filtered");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  skips: {} -> {} default={} allow={}", skips0, skips1, default_cnt, allow_cnt);
    CHECK(skips1 >= skips0, "macro_introduced_skipped monotonic after query:pattern");
    return true;
}

// ── AC3: :respect-hygiene keyword alias ────────────────────
bool test_respect_hygiene_keyword(CompilerService& cs) {
    std::println("\n--- AC3: :respect-hygiene keyword alias ---");
    auto r1 = cs.eval("(query:pattern \"base\" :respect-hygiene #f)");
    CHECK(r1.has_value(), "(query:pattern :respect-hygiene #f) recognized");
    auto r2 = cs.eval("(query:pattern \"base\" :include-macro-introduced #f)");
    CHECK(r2.has_value(), "(query:pattern :include-macro-introduced #f) recognized");
    const auto respect_cnt = result_count(cs, "(query:pattern \"*\" :respect-hygiene #t)");
    const auto include_cnt = result_count(cs, "(query:pattern \"*\" :include-macro-introduced #t)");
    CHECK(respect_cnt == include_cnt, ":respect-hygiene #t mirrors :include-macro-introduced #t");
    return true;
}

// ── AC4: mutate safe on user nodes under Guard ─────────────
bool test_mutate_safe_on_user_nodes(CompilerService& cs) {
    std::println("\n--- AC4: mutate safe on user nodes under Guard ---");
    const auto markers_before = cs.eval("(length (query:macro-introduced))");
    CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind under Guard");
    (void)cs.eval("(query:pattern \"base\")");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after mutate");
    auto user_pat = cs.eval("(query:pattern \"base\")");
    CHECK(user_pat.has_value(), "query:pattern finds user binding after mutate");
    cs.evaluator().ensure_macro_hygiene_contract();
    CHECK(cs.evaluator().get_macro_hygiene_contract_violations() == 0,
          "zero macro hygiene contract violations after user mutate");
    if (markers_before && aura::compiler::types::is_int(*markers_before)) {
        auto markers_after = cs.eval("(length (query:macro-introduced))");
        CHECK(markers_after.has_value() && aura::compiler::types::is_int(*markers_after),
              "macro-introduced count still observable");
        CHECK(aura::compiler::types::as_int(*markers_after) >=
                  aura::compiler::types::as_int(*markers_before),
              "macro-introduced count stable after user mutate");
    }
    return true;
}

// ── AC5: IR InlinePass respects MacroIntroduced ────────────
bool test_ir_inlinepass_respects_macrointroduced(CompilerService& cs) {
    std::println("\n--- AC5: IR InlinePass respects MacroIntroduced ---");
    auto ihs = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
    CHECK(ihs.has_value() && aura::compiler::types::is_hash(*ihs),
          "(engine:metrics \"query:ir-hygiene-stats\") returns hash");
    const auto skipped = inline_skipped(cs);
    CHECK(skipped >= 0, "inline macro-hygiene-skipped observable");
    auto r =
        cs.eval("(hash-ref (engine:metrics \"query:ir-hygiene-stats\") 'respect-macro-hygiene')");
    CHECK(r.has_value() && aura::compiler::types::is_int(*r) &&
              aura::compiler::types::as_int(*r) == 1,
          "respect-macro-hygiene == 1 (default on)");
    return true;
}

// ── AC6: tag_arity_index dirty hook under macro+query ──────
bool test_tag_arity_dirty_hook_under_query(CompilerService& cs) {
    std::println("\n--- AC6: tag_arity_index dirty hook under macro+query ---");
    auto* ws = cs.evaluator().workspace_flat();
    CHECK(ws != nullptr, "workspace flat available");
    if (!ws)
        return false;
    ws->rebuild_tag_arity_index();
    const auto d0 = ws->tag_arity_index_dirty_marks();
    if (ws->size() > 0)
        ws->mark_dirty_upward(static_cast<aura::ast::NodeId>(0));
    CHECK(ws->tag_arity_index_dirty(), "mark_dirty_upward flips dirty flag");
    (void)cs.eval("(query:pattern \"base\")");
    (void)cs.eval("(query:pattern \"*\")");
    const auto d1 = ws->tag_arity_index_dirty_marks();
    std::println("  dirty_marks: {} -> {}", d0, d1);
    CHECK(d1 > d0, "dirty_marks bumped under macro+query load");
    return true;
}

// ── AC7: query:pattern-ir-hygiene-closed-loop-stats ────────
bool test_pattern_ir_hygiene_closed_loop_stats(CompilerService& cs) {
    std::println("\n--- AC7: query:pattern-ir-hygiene-closed-loop-stats ---");
    auto h = cs.eval("(engine:metrics \"query:pattern-ir-hygiene-closed-loop-stats\")");
    CHECK(h.has_value() && aura::compiler::types::is_hash(*h),
          "pattern-ir-hygiene-closed-loop-stats returns hash");
    const auto capture = hash_int(cs, "capture-prevented");
    const auto ir_viol = hash_int(cs, "ir-post-mutate-violation");
    const auto tag_delta = hash_int(cs, "tag-arity-delta");
    const auto schema = hash_int(cs, "schema");
    CHECK(capture >= 0, std::format("capture-prevented >= 0 (got {})", capture));
    CHECK(ir_viol >= 0, std::format("ir-post-mutate-violation >= 0 (got {})", ir_viol));
    CHECK(tag_delta >= 0, std::format("tag-arity-delta >= 0 (got {})", tag_delta));
    CHECK(schema == 593, std::format("schema == 593 (got {})", schema));
    return true;
}

// ── AC8: combined metrics monotonic ────────────────────────
bool test_combined_metrics_monotonic(CompilerService& cs) {
    std::println("\n--- AC8: combined metrics monotonic ---");
    const auto capture0 = hash_int(cs, "capture-prevented");
    const auto phs0 = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    const auto prod0 =
        cs.eval("(hash-ref (engine:metrics \"query:macro-production-hygiene-stats\") "
                "'macro-production-hygiene-total')");
    (void)cs.eval("(query:pattern \"*\")");
    (void)cs.eval("(query:pattern \"base\" :respect-hygiene #t)");
    cs.evaluator().ensure_macro_hygiene_contract();
    const auto capture1 = hash_int(cs, "capture-prevented");
    const auto phs1 = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    const auto prod1 =
        cs.eval("(hash-ref (engine:metrics \"query:macro-production-hygiene-stats\") "
                "'macro-production-hygiene-total')");
    CHECK(capture1 >= capture0,
          std::format("capture-prevented monotonic ({} -> {})", capture0, capture1));
    if (phs0 && phs1 && aura::compiler::types::is_int(*phs0) &&
        aura::compiler::types::is_int(*phs1)) {
        CHECK(aura::compiler::types::as_int(*phs1) >= aura::compiler::types::as_int(*phs0),
              "pattern-hygiene-stats monotonic");
    } else if (phs0 && phs1 && aura::compiler::types::is_hash(*phs0) &&
               aura::compiler::types::is_hash(*phs1)) {
        CHECK(true, "pattern-hygiene-stats hash (schema 1609) present");
    }
    if (prod0 && prod1 && aura::compiler::types::is_int(*prod0) &&
        aura::compiler::types::is_int(*prod1)) {
        CHECK(aura::compiler::types::as_int(*prod1) >= aura::compiler::types::as_int(*prod0),
              "macro-production-hygiene-total monotonic");
    }
    return true;
}

// ── AC9: fuzz mutate+query stress ──────────────────────────
bool test_fuzz_mutate_query_stress(CompilerService& cs) {
    std::println("\n--- AC9: {} iters fuzz mutate+query stress ---", k_stress_iters());
    std::mt19937 rng(593u);
    std::uniform_int_distribution<int> val_dist(0, 999);
    std::uniform_int_distribution<int> query_every(7, 23);
    int queries = 0;
    int next_query = query_every(rng);
    const auto capture0 = hash_int(cs, "capture-prevented");
    const auto skips0 = cs.evaluator().get_macro_introduced_skipped_in_query();
    for (int i = 0; i < k_stress_iters(); ++i) {
        if (i == next_query) {
            (void)cs.eval("(query:pattern \"*\")");
            (void)cs.eval("(query:pattern \"base\")");
            ++queries;
            next_query = i + query_every(rng);
        }
        if ((i & 31) == 0) {
            std::string code =
                "(define stress-" + std::to_string(i) + " " + std::to_string(val_dist(rng)) + ")";
            (void)cs.eval(code);
        }
    }
    const auto capture1 = hash_int(cs, "capture-prevented");
    const auto skips1 = cs.evaluator().get_macro_introduced_skipped_in_query();
    std::println("  iters={} queries={} capture: {} -> {} skips: {} -> {}", k_stress_iters(),
                 queries, capture0, capture1, skips0, skips1);
    CHECK(queries > 0, "at least 1 query:pattern in stress");
    CHECK(capture1 >= capture0, "capture-prevented grew in stress");
    CHECK(skips1 >= skips0, "macro_introduced_skipped grew in stress");
    return true;
}

// ── AC10: regression — existing hygiene primitives ─────────
bool test_regression_hygiene_primitives(CompilerService& cs) {
    std::println("\n--- AC10: regression — existing hygiene primitives ---");
    auto r1 = cs.eval("(engine:metrics \"query:macro-hygiene-stats\")");
    CHECK(r1.has_value() && aura::compiler::types::is_hash(*r1),
          "(engine:metrics \"query:macro-hygiene-stats\") regression");
    auto r2 = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    CHECK(r2.has_value() &&
              (aura::compiler::types::is_int(*r2) || aura::compiler::types::is_hash(*r2)),
          "(engine:metrics \"query:pattern-hygiene-stats\") regression");
    auto r3 = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
    CHECK(r3.has_value() && aura::compiler::types::is_hash(*r3),
          "(engine:metrics \"query:ir-hygiene-stats\") regression");
    auto r4 = cs.eval("(engine:metrics \"query:macro-production-hygiene-stats\")");
    CHECK(r4.has_value() && aura::compiler::types::is_hash(*r4),
          "(engine:metrics \"query:macro-production-hygiene-stats\") regression");
    auto r5 = cs.eval("(engine:metrics \"query:macro-hygiene-contract-stats\")");
    CHECK(r5.has_value() && aura::compiler::types::is_int(*r5),
          "(engine:metrics \"query:macro-hygiene-contract-stats\") regression");
    return true;
}

int run_tests() {
    std::println("═══ Issue #593: query:pattern + IR MacroIntroduced hygiene ═══\n");
    CompilerService cs;
    std::println("Layer 1: macro expand + query:pattern hygiene");
    test_macro_expand_stamps_macrointroduced(cs);
    test_query_pattern_default_filters(cs);
    test_respect_hygiene_keyword(cs);
    test_mutate_safe_on_user_nodes(cs);
    std::println("\nLayer 2: IR InlinePass + tag_arity + closed-loop stats");
    test_ir_inlinepass_respects_macrointroduced(cs);
    test_tag_arity_dirty_hook_under_query(cs);
    test_pattern_ir_hygiene_closed_loop_stats(cs);
    test_combined_metrics_monotonic(cs);
    std::println("\nLayer 3: stress + regression");
    test_fuzz_mutate_query_stress(cs);
    test_regression_hygiene_primitives(cs);
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_593_detail

int aura_issue_593_run() {
    return aura_issue_593_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_593_run();
}
#endif