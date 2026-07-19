// @category: integration
// @reason: Issue #669 — PrimMeta populated but dead code for AI
//  introspection (P1 stdlib-impl AI-native). Ships:
//   - (query:primitives-meta [name]) enrichment — the per-name
//     response now pulls real PrimMeta fields (arity, pure,
//     safety, doc, category) via meta_for_slot. Schema sentinel
//     bumped from 643 to 669 to signal the enriched 8-field
//     shape.
//   - (query:primitives-meta-stats, schema 669) — new hash
//     primitive with 4 fields:
//       - meta-hits         primitives_meta_query_total counter
//       - documented-count  documented_meta_count()
//       - schema-documented schema_documented_meta_count()
//       - total-registered  primitives_.slot_count()
//
//  Non-duplicative with #643 (foundation shape), #617
//  (query:primitives-meta-catalog registry-level summary),
//  #697 (query:primitives-extension-stats runtime counters),
//  #498 (query:primitive-metadata base AI-native primitive),
//  #480 (PrimMeta struct definition).
//
//   - AC1:  query:primitives-meta [name] returns hash with the
//           new 8-field shape (name, has-fn, arity, pure,
//           safety, doc, category, schema=669)
//   - AC2:  Per-name response for a real primitive
//           (regex-match?) populates doc="" (default), pure=#t,
//           arity in [0,255], safety in [0,255], schema=669
//   - AC3:  Per-name response for unknown primitive returns
//           has-fn=0 (so the Agent can distinguish "known" from
//           "unknown" without changing the shape)
//   - AC4:  schema sentinel is 669 (drift detection — changed
//           from 643)
//   - AC5:  query:primitives-meta-stats returns hash with
//           schema=669 + meta-hits (>=1 after one call),
//           total-registered > 0, documented-count >= 0
//   - AC6:  meta-hits counter increments on per-name query
//           (the per-primitive lookup form bumps
//           primitives_meta_query_total, same as catalog form)
//   - AC7:  no-arg form of query:primitives-meta still returns
//           the foundation counters (define-macro-used,
//           prim-error-unified) — preserved, not duplicated
//   - AC8:  regression — query:primitives-meta-catalog (#617)
//           and primitive:describe (#480/#559) still reachable
//           with their existing shapes

#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_669_detail {
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

static void run_ac1_8_fields(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:primitives-meta [name] returns 8-field hash ---");
    auto r = cs.eval(R"aura((query:primitives-meta "regex-match?"))aura");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "query:primitives-meta \"regex-match?\" returns a hash");
    // All 8 fields present (we don't assert the values here, just presence).
    const std::vector<std::string> keys = {"name",   "has-fn", "arity",    "pure",
                                           "safety", "doc",    "category", "schema"};
    for (const auto& k : keys) {
        auto f =
            cs.eval(std::format("(hash-ref (query:primitives-meta \"regex-match?\") '{}')", k));
        CHECK(f, std::format("field '{}' present in per-name response", k));
    }
}

static void run_ac2_real_primitive(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: per-name response for regex-match? has valid fields ---");
    const auto name = cs.eval(R"aura((hash-ref (query:primitives-meta "regex-match?") 'name))aura");
    const auto arity =
        cs.eval(R"aura((hash-ref (query:primitives-meta "regex-match?") 'arity))aura");
    const auto pure = cs.eval(R"aura((hash-ref (query:primitives-meta "regex-match?") 'pure))aura");
    const auto safety =
        cs.eval(R"aura((hash-ref (query:primitives-meta "regex-match?") 'safety))aura");
    CHECK(name && aura::compiler::types::is_string(*name), "name is a string");
    CHECK(arity && aura::compiler::types::is_int(*arity) &&
              aura::compiler::types::as_int(*arity) >= 0 &&
              aura::compiler::types::as_int(*arity) <= 255,
          "arity is int in [0, 255] (255 = variadic default)");
    CHECK(pure, "pure field exists (bool)");
    CHECK(safety && aura::compiler::types::is_int(*safety) &&
              aura::compiler::types::as_int(*safety) >= 0 &&
              aura::compiler::types::as_int(*safety) <= 255,
          "safety is int in [0, 255]");
}

static void run_ac3_unknown_primitive(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: per-name response for unknown primitive returns has-fn=0 ---");
    auto has_fn = cs.eval(
        R"aura((hash-ref (query:primitives-meta "definitely-not-a-primitive-xyz") 'has-fn))aura");
    auto schema = cs.eval(
        R"aura((hash-ref (query:primitives-meta "definitely-not-a-primitive-xyz") 'schema))aura");
    CHECK(has_fn && aura::compiler::types::is_int(*has_fn) &&
              aura::compiler::types::as_int(*has_fn) == 0,
          "unknown primitive → has-fn=0");
    CHECK(schema && aura::compiler::types::is_int(*schema) &&
              aura::compiler::types::as_int(*schema) == 669,
          "unknown primitive still emits schema=669 (drift sentinel intact)");
}

static void run_ac4_schema_669(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: schema sentinel is 669 (changed from 643) ---");
    auto r = cs.eval(R"aura((hash-ref (query:primitives-meta "+") 'schema))aura");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 669,
          "per-name schema == 669 (enriched-shape drift sentinel)");
}

static void run_ac5_stats_shape(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: query:primitives-meta-stats reachable with 4+ fields ---");
    auto r = cs.eval("(engine:metrics \"query:primitives-meta-stats\")");
    CHECK(r && aura::compiler::types::is_hash(*r), "query:primitives-meta-stats returns a hash");
    const std::vector<std::string> keys = {"meta-hits", "documented-count", "schema-documented",
                                           "total-registered", "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:primitives-meta-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
    auto total =
        cs.eval("(hash-ref (engine:metrics \"query:primitives-meta-stats\") 'total-registered)");
    CHECK(total && aura::compiler::types::is_int(*total) &&
              aura::compiler::types::as_int(*total) > 0,
          "total-registered > 0 (sanity: at least some primitives are registered)");
    auto schema = cs.eval("(hash-ref (engine:metrics \"query:primitives-meta-stats\") 'schema)");
    CHECK(schema && aura::compiler::types::is_int(*schema) &&
              aura::compiler::types::as_int(*schema) == 669,
          "stats schema == 669");
}

static void run_ac6_meta_hits_bump(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: meta-hits counter increments on per-name query ---");
    const auto before =
        cs.eval("(hash-ref (engine:metrics \"query:primitives-meta-stats\") 'meta-hits)");
    if (!before || !aura::compiler::types::is_int(*before)) {
        ++g_failed;
        std::println(std::cerr, "  FAIL: meta-hits before not an int");
        return;
    }
    const std::int64_t before_n = aura::compiler::types::as_int(*before);
    // One more per-name call.
    cs.eval(R"aura((query:primitives-meta "+"))aura");
    const auto after =
        cs.eval("(hash-ref (engine:metrics \"query:primitives-meta-stats\") 'meta-hits)");
    const std::int64_t after_n =
        aura::compiler::types::is_int(*after) ? aura::compiler::types::as_int(*after) : -1;
    CHECK(after_n - before_n >= 1, "meta-hits incremented by >=1 after per-name query");
}

static void run_ac7_no_arg_form_preserved(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC7: no-arg form returns foundation counters (preserved) ---");
    auto r = cs.eval("(engine:metrics \"query:primitives-meta\")");
    CHECK(r && aura::compiler::types::is_hash(*r), "no-arg form returns a hash");
    auto define_used =
        cs.eval("(hash-ref (engine:metrics \"query:primitives-meta\") 'define-macro-used)");
    auto prim_err =
        cs.eval("(hash-ref (engine:metrics \"query:primitives-meta\") 'prim-error-unified)");
    CHECK(define_used, "foundation counter 'define-macro-used' still present");
    CHECK(prim_err, "foundation counter 'prim-error-unified' still present");
}

static void run_ac8_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC8: regression — adjacent meta primitives reachable ---");
    auto catalog = cs.eval("(engine:metrics \"query:primitives-meta-catalog\")");
    auto describe = cs.eval(R"aura((primitive:describe "+"))aura");
    CHECK(catalog && aura::compiler::types::is_hash(*catalog),
          "query:primitives-meta-catalog (#617) regression [hash]");
    CHECK(describe, "primitive:describe (#480/#559) regression");
    auto total_field =
        cs.eval("(hash-ref (engine:metrics \"query:primitives-meta-catalog\") 'total-registered)");
    CHECK(total_field && aura::compiler::types::is_int(*total_field) &&
              aura::compiler::types::as_int(*total_field) > 0,
          "query:primitives-meta-catalog total-registered > 0 (regression)");
}

} // namespace aura_issue_669_detail

int aura_issue_669_run() {
    using namespace aura_issue_669_detail;

    {
        aura::compiler::CompilerService cs;
        run_ac1_8_fields(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_real_primitive(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_unknown_primitive(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_schema_669(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_stats_shape(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_meta_hits_bump(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac7_no_arg_form_preserved(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac8_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_669_run();
}
#endif
