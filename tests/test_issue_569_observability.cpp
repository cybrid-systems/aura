// @category: integration
// @reason: Issue #569 — arena-auto-compact-defrag-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_569_detail {
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

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:arena-auto-compact-defrag-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (f x) (+ x 1)) (define base 10) (f base)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_569_detail

int main() {
    using namespace aura_issue_569_detail;

    std::println("=== Issue #569: arena-auto-compact-defrag-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:arena-auto-compact-defrag-stats returns hash
    {
        std::println("\n--- AC1: query:arena-auto-compact-defrag-stats ---");
        auto stats = cs.eval("(query:arena-auto-compact-defrag-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:arena-auto-compact-defrag-stats returns hash");
        CHECK(hash_int(cs, "fragmentation-ratio-pct") >= 0, "fragmentation-ratio-pct present");
        CHECK(hash_int(cs, "peak-used-bytes") >= 0, "peak-used-bytes present");
        CHECK(hash_int(cs, "live-dtor-count") >= 0, "live-dtor-count present");
        CHECK(hash_int(cs, "auto-compact-count") >= 0, "auto-compact-count present");
        CHECK(hash_int(cs, "auto-compact-skips") >= 0, "auto-compact-skips present");
        CHECK(hash_int(cs, "auto-compact-guard-calls") >= 0, "auto-compact-guard-calls present");
        CHECK(hash_int(cs, "defrag-saved-bytes") >= 0, "defrag-saved-bytes present");
        CHECK(hash_int(cs, "defrag-attempted-count") >= 0, "defrag-attempted-count present");
        CHECK(hash_int(cs, "safepoint-coordination-count") >= 0,
              "safepoint-coordination-count present");
        CHECK(hash_int(cs, "mutation-volume-trigger") >= 0, "mutation-volume-trigger present");
        CHECK(hash_int(cs, "threshold-config-count") >= 0, "threshold-config-count present");
        CHECK(hash_int(cs, "compaction-yield-checks") >= 0, "compaction-yield-checks present");
        CHECK(hash_int(cs, "paused-by-boundary") >= 0, "paused-by-boundary present");
        CHECK(hash_int(cs, "task4-review-schema") == 569, "task4-review-schema == 569");
        CHECK(hash_int(cs, "arena-auto-compact-defrag-total") >= 0,
              "arena-auto-compact-defrag-total present");
        CHECK(hash_int(cs, "arena-auto-compact-defrag-recommendation") >= 0,
              "arena-auto-compact-defrag-recommendation present");
    }

    const auto guard_before = hash_int(cs, "auto-compact-guard-calls");
    const auto total_before = hash_int(cs, "arena-auto-compact-defrag-total");

    // AC2: Guard mutate bumps compaction lifecycle counters
    {
        std::println("\n--- AC2: Guard mutate compaction lifecycle ---");
        CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"issue569\")").has_value(),
              "mutate:rebind lambda under Guard");
        (void)cs.eval("(eval-current)");
        const auto guard_after = hash_int(cs, "auto-compact-guard-calls");
        CHECK(guard_after > guard_before,
              std::format("auto-compact-guard-calls grew ({} -> {})", guard_before, guard_after));
    }

    // AC3: defrag request + query cycle
    {
        std::println("\n--- AC3: defrag request + query cycle ---");
        (void)cs.eval("(arena:request-defrag)");
        (void)cs.eval("(query:pattern \"f\")");
        const auto total_after = hash_int(cs, "arena-auto-compact-defrag-total");
        CHECK(total_after >= total_before,
              std::format("arena-auto-compact-defrag-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related arena primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto production = cs.eval("(query:arena-production-compaction-stats)");
        auto auto_compact = cs.eval("(query:arena-auto-compact-stats)");
        auto auto_stats = cs.eval("(query:arena-auto-stats)");
        auto frag_snap = cs.eval("(query:arena-fragmentation-snapshot)");
        CHECK(production && aura::compiler::types::is_hash(*production),
              "query:arena-production-compaction-stats hash regression (#534)");
        CHECK(auto_compact && aura::compiler::types::is_hash(*auto_compact),
              "query:arena-auto-compact-stats hash regression (#685)");
        CHECK(auto_stats && aura::compiler::types::is_hash(*auto_stats),
              "query:arena-auto-stats hash regression (#464)");
        CHECK(frag_snap && aura::compiler::types::is_hash(*frag_snap),
              "query:arena-fragmentation-snapshot hash regression (#604)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 138,
              "stats:count >= 138");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}