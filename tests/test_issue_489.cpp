// @category: integration
// @reason: Issue #489 — StableNodeRef + get_safe enforcement in mutate/query hot paths

#include <format>
#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_489_detail {
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

static std::int64_t snap_stat(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:stability-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define base 10) (+ base 1)\")"))
        return false;
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_489_detail

int aura_issue_489_run() {
    using namespace aura_issue_489_detail;

    std::println("=== Issue #489: StableNodeRef enforcement in mutate/query hot paths ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:stability-stats hash fields
    {
        std::println("\n--- AC1: query:stability-stats ---");
        auto stats = cs.eval("(query:stability-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:stability-stats returns hash");
        CHECK(snap_stat(cs, "raw-nodeid-usage") >= 0, "raw-nodeid-usage present");
        CHECK(snap_stat(cs, "stable-ref-validated") >= 0, "stable-ref-validated present");
        CHECK(snap_stat(cs, "stale-ref-blocked") >= 0, "stale-ref-blocked present");
        CHECK(snap_stat(cs, "stale-ref-warned") >= 0, "stale-ref-warned present");
        CHECK(snap_stat(cs, "stable-ref-invalidations") >= 0, "stable-ref-invalidations present");
    }

    const auto raw_before = cs.evaluator().get_raw_nodeid_usage_in_primitives_count();
    const auto validated_before = cs.evaluator().get_stable_ref_validated_in_primitives_count();
    const auto blocked_before = cs.evaluator().get_stale_ref_blocked_count();

    // AC2: query:as-stable-ref + stable-ref path mutate succeeds
    {
        std::println("\n--- AC2: stable-ref path mutate:replace-value ---");
        auto lit = cs.eval("(car (query:node-type \"LiteralInt\"))");
        CHECK(lit && aura::compiler::types::is_int(*lit), "LiteralInt node id found");
        if (lit && aura::compiler::types::is_int(*lit)) {
            const auto nid = aura::compiler::types::as_int(*lit);
            auto ok = cs.eval(std::format("(let ((sref (query:as-stable-ref {})))"
                                          "  (mutate:replace-value sref 11 \"stable-ref bump\"))",
                                          nid));
            CHECK(ok.has_value(), "mutate:replace-value accepts stable-ref pair");
            CHECK(cs.eval("(eval-current)").has_value(), "eval-current after stable-ref mutate");
        }
    }

    // AC3: captured stable-ref stale after generation bump (Strict policy blocks)
    {
        std::println("\n--- AC3: stale stable-ref blocked under Strict policy ---");
        (void)cs.eval("(mutate:set-stale-ref-policy \"strict\")");
        auto lit = cs.eval("(car (query:node-type \"LiteralInt\"))");
        if (lit && aura::compiler::types::is_int(*lit)) {
            const auto nid = aura::compiler::types::as_int(*lit);
            auto stale_attempt =
                cs.eval(std::format("(let ((sref (query:as-stable-ref {})))"
                                    "  (mutate:rebind \"base\" \"20\" \"bump gen\")"
                                    "  (mutate:replace-value sref 12 \"should fail\"))",
                                    nid));
            CHECK(!stale_attempt.has_value() ||
                      (stale_attempt && aura::compiler::types::is_pair(*stale_attempt)),
                  "stale stable-ref mutate surfaces error under Strict");
            const auto blocked_after = cs.evaluator().get_stale_ref_blocked_count();
            CHECK(blocked_after > blocked_before,
                  std::format("stale_ref_blocked grew ({} -> {})", blocked_before, blocked_after));
        } else {
            ++g_failed;
            std::println(std::cerr, "  FAIL: LiteralInt node id missing for AC3");
        }
        (void)cs.eval("(mutate:set-stale-ref-policy \"disabled\")");
    }

    // AC4: raw_nodeid_usage / stable_ref_validated counters grow
    {
        std::println("\n--- AC4: primitive usage counters ---");
        auto lit = cs.eval("(car (query:node-type \"LiteralInt\"))");
        if (lit && aura::compiler::types::is_int(*lit)) {
            const auto nid = aura::compiler::types::as_int(*lit);
            (void)cs.eval(std::format("(mutate:replace-value {} 13 \"raw id path\")", nid));
            (void)cs.eval(std::format(
                "(mutate:replace-value (query:as-stable-ref {}) 14 \"stable path\")", nid));
        }
        const auto raw_after = cs.evaluator().get_raw_nodeid_usage_in_primitives_count();
        const auto validated_after = cs.evaluator().get_stable_ref_validated_in_primitives_count();
        CHECK(raw_after > raw_before,
              std::format("raw_nodeid_usage grew ({} -> {})", raw_before, raw_after));
        CHECK(
            validated_after > validated_before,
            std::format("stable_ref_validated grew ({} -> {})", validated_before, validated_after));
        CHECK(snap_stat(cs, "raw-nodeid-usage") == static_cast<std::int64_t>(raw_after),
              "hash raw-nodeid-usage matches evaluator counter");
        CHECK(snap_stat(cs, "stable-ref-validated") == static_cast<std::int64_t>(validated_after),
              "hash stable-ref-validated matches evaluator counter");
    }

    // AC5: existing stable-ref primitives regression
    {
        std::println("\n--- AC5: stable-ref primitive regression ---");
        auto srs = cs.eval("(query:stable-ref-stats)");
        auto srh = cs.eval("(query:stable-ref-stats-hash)");
        auto sref = cs.eval("(query:as-stable-ref 2)");
        auto stale = cs.eval("(query:stale-ref-stats)");
        CHECK(srs && aura::compiler::types::is_int(*srs), "query:stable-ref-stats regression");
        CHECK(srh && aura::compiler::types::is_hash(*srh),
              "query:stable-ref-stats-hash regression");
        CHECK(sref && aura::compiler::types::is_pair(*sref), "query:as-stable-ref regression");
        CHECK(stale && aura::compiler::types::is_int(*stale), "query:stale-ref-stats regression");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 211,
              "stats:count >= 211");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_489_run();
}
#endif
