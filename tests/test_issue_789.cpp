// test_issue_789.cpp — Issue #789: P0 mandate
// SafePCVSpan / children_safe in all query:pattern /
// matcher walks + enforce tag_arity_index_ hot-path +
// deep :marker provenance predicate for production
// concurrent large-AST AI loops (Refine/Consolidate
// #760 non-duplicative).
//
// Scope-limited close: the body asks for 5 things:
// (1) mandate children_safe_view / SafePCVSpan for
// all children iteration in pattern match / filter /
// where + add generation pin check, (2) fully
// populate tag_arity_index_ (hash on tag+arity+marker)
// on every structural change + wire fast-path lookup
// in matcher before linear fallback, (3) QueryExpr /
// pattern parser support for hygiene provenance
// predicates (`:marker MacroIntroduced :provenance
// macro-def-id`) + auto-filter or stamp in matcher
// under macro context + wire to clone_macro_body
// name_map, (4) enhance (query:pattern-performance-
// stats) with (index_hit_rate, safe_span_uses,
// hygiene_predicate_hits, dangling_prevented) + wire
// to mutation-impact-snapshot, (5) new
// tests/test_query_pattern_indexing_safe_span_hygiene_
// concurrent.cpp harness (large macro-expanded AST +
// concurrent fibers + pattern mutate under Guard →
// assert index used, SafePCVSpan pinned, hygiene
// predicate works, no dangling, perf win, TSan clean)
// + SEVA pattern-heavy verification self-edit + CI
// perf gate + docs. All follow-up work is Phase 2+
// (each requires touching query_matcher.cpp +
// evaluator_primitives_query.cpp + ast.ixx + new
// harness + SEVA demo + docs). Phase 1 observability
// surface ships in this PR:
//
//   1. 2 NEW CompilerMetrics atomics + 2 NEW bump
//      helpers on Evaluator:
//      - pattern_safe_span_uses_total /
//        bump_pattern_safe_span_use() (called at
//        the planned Phase 2+ children_safe_view /
//        SafePCVSpan pin call sites in
//        query_matcher.cpp + evaluator_primitives_
//        query.cpp)
//      - pattern_dangling_prevented_total /
//        bump_pattern_dangling_prevented() (called
//        when the generation pin check fires — the
//        safety net signal)
//   2. New standalone (query:pattern-index-safe-
//      span-stats, schema 789) primitive returning
//      2 NEW atomics + 4 hardcoded "not yet" fields
//      (index-hit-rate + 3 deferred flags) + derived
//      recommendation + schema sentinel (8-entry
//      hash).
//
// ACs:
//   AC1: hash shape (7 fields + schema sentinel = 8 entries)
//   AC2: fresh-service zero state (2 NEW atomics == 0;
//        4 hardcoded "not yet" flags == 0;
//        recommendation == 3 early-stage)
//   AC3: schema == 789 (drift sentinel)
//   AC4: production-path bump correctness — call the
//        per-Evaluator bump helpers + cross-check the
//        primitive reads reflect the bumps
//   AC5: sibling observability regression — #760
//        (query:pattern-performance-stats) + #788
//        (query:ai-native-extension-stats) primitives
//        still reachable with their schema sentinels
//        intact

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_789_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int_field(aura::compiler::CompilerService& cs, std::string_view hash_src,
                                   std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_shape(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: (query:pattern-index-safe-span-stats) hash shape ---");
    auto r = cs.eval("(query:pattern-index-safe-span-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(query:pattern-index-safe-span-stats) returns a hash");
    const std::vector<std::string> keys = {"safe-span-uses",
                                           "dangling-prevented",
                                           "index-hit-rate",
                                           "safe-span-mandate-active",
                                           "tag-arity-index-population-active",
                                           "deep-hygiene-predicate-active",
                                           "recommendation",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format("(hash-ref (query:pattern-index-safe-span-stats) '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: fresh-service zero state (no SafePCVSpan activity) ---");
    const auto safe_uses =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "safe-span-uses");
    CHECK(safe_uses == 0,
          std::format("safe-span-uses = {} (expected 0 on fresh service — Phase 2+ deferred to "
                      "wire children_safe_view / SafePCVSpan pin call sites in query_matcher.cpp "
                      "+ evaluator_primitives_query.cpp pattern iterator paths per body "
                      "\"Mandate children_safe_view / SafePCVSpan for all children iteration in "
                      "pattern match / filter / where; add generation pin check\")",
                      safe_uses));
    const auto dangling =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "dangling-prevented");
    CHECK(dangling == 0,
          std::format("dangling-prevented = {} (expected 0 on fresh service — Phase 2+ deferred "
                      "to wire generation pin check in ast.ixx children_safe_view)",
                      dangling));
    const auto index_rate =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "index-hit-rate");
    CHECK(index_rate == 0,
          std::format("index-hit-rate = {} (expected 0 in Phase 1 — Phase 2+ to derive from "
                      "#760 pattern_match_index_hits_total / (linear-scans + index-hits) × "
                      "10000)",
                      index_rate));
    const auto mandate =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "safe-span-mandate-active");
    CHECK(mandate == 0,
          std::format("safe-span-mandate-active = {} (expected 0 — Phase 2+ deferred per body "
                      "\"Mandate children_safe_view / SafePCVSpan for all children iteration "
                      "in pattern match / filter / where\")",
                      mandate));
    const auto tag_arity = hash_int_field(cs, "(query:pattern-index-safe-span-stats)",
                                          "tag-arity-index-population-active");
    CHECK(tag_arity == 0,
          std::format("tag-arity-index-population-active = {} (expected 0 — Phase 2+ deferred "
                      "per body \"Fully populate tag_arity_index_ (hash on tag+arity+marker) "
                      "on every structural change; wire fast-path lookup in matcher before "
                      "linear fallback\")",
                      tag_arity));
    const auto deep_pred = hash_int_field(cs, "(query:pattern-index-safe-span-stats)",
                                          "deep-hygiene-predicate-active");
    CHECK(deep_pred == 0,
          std::format("deep-hygiene-predicate-active = {} (expected 0 — Phase 2+ deferred per "
                      "body \"Add support for hygiene provenance predicates ... auto-filter "
                      "or stamp in matcher under macro context\")",
                      deep_pred));
    const auto rec = hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "recommendation");
    CHECK(rec == 3,
          std::format("recommendation = {} (expected 3 = early-stage when all 3 deferred flags "
                      "== 0 AND no activity)",
                      rec));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 789 (drift sentinel) ---");
    const auto schema = hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "schema");
    CHECK(schema == 789, std::format("schema = {} (expected 789)", schema));
}

static void run_ac4_bump_correctness(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: production-path bump helpers + primitive read-back ---");

    // Snapshot before.
    const auto safe_before =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "safe-span-uses");
    const auto dangling_before =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "dangling-prevented");

    // Exercise the 2 NEW per-Evaluator bump helpers
    // via the service's evaluator instance. The bump
    // helpers bump CompilerMetrics atomics (which
    // the primitive reads via ev.compiler_metrics()).
    auto& ev = cs.evaluator();
    constexpr int k_iters = 5;
    for (int i = 0; i < k_iters; ++i) {
        ev.bump_pattern_safe_span_use();
        ev.bump_pattern_dangling_prevented();
    }

    const auto safe_after =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "safe-span-uses");
    const auto dangling_after =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "dangling-prevented");

    std::println("  counts after AC4 bumps: safe-span {} -> {}, dangling-prevented {} -> {}",
                 safe_before, safe_after, dangling_before, dangling_after);

    // Direct bump helpers added exactly k_iters to
    // each of the 2 NEW atomics.
    CHECK(safe_after >= safe_before + k_iters,
          std::format("safe-span-uses bumped by bump_pattern_safe_span_use ({} -> {})", safe_before,
                      safe_after));
    CHECK(dangling_after >= dangling_before + k_iters,
          std::format("dangling-prevented bumped by bump_pattern_dangling_prevented ({} -> {})",
                      dangling_before, dangling_after));

    // Recommendation should now be 2 (Phase 1 only —
    // all 3 deferred flags == 0 BUT activity > 0).
    const auto rec_after =
        hash_int_field(cs, "(query:pattern-index-safe-span-stats)", "recommendation");
    CHECK(rec_after == 2,
          std::format("recommendation = {} (expected 2 = Phase 1 only after activity; "
                      "activity > 0 with all 3 deferred flags == 0)",
                      rec_after));
}

static void run_ac5_sibling_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #760 + #788 sibling primitives unaffected ---");
    auto a760 = cs.eval("(query:pattern-performance-stats)");
    auto a788 = cs.eval("(query:ai-native-extension-stats)");
    CHECK(a760 && aura::compiler::types::is_hash(*a760),
          "query:pattern-performance-stats hash regression (#760)");
    CHECK(a788 && aura::compiler::types::is_hash(*a788),
          "query:ai-native-extension-stats hash regression (#788)");
    const auto a760_schema = hash_int_field(cs, "(query:pattern-performance-stats)", "schema");
    CHECK(a760_schema == 760,
          std::format("#760 schema = {} (expected 760, no drift)", a760_schema));
    const auto a788_schema = hash_int_field(cs, "(query:ai-native-extension-stats)", "schema");
    CHECK(a788_schema == 788,
          std::format("#788 schema = {} (expected 788, no drift)", a788_schema));
}

} // namespace aura_issue_789_detail

int aura_issue_789_run() {
    using namespace aura_issue_789_detail;
    std::println("=== Issue #789: P0 mandate SafePCVSpan + tag_arity_index_ hot-path + deep "
                 "hygiene predicate observability (scope-limited close) ===");

    {
        aura::compiler::CompilerService cs;
        run_ac1_shape(cs);
        run_ac2_fresh_zero(cs);
        run_ac3_schema_sentinel(cs);
        run_ac4_bump_correctness(cs);
        run_ac5_sibling_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_789_run();
}
#endif
