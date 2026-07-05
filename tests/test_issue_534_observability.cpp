// @category: integration
// @reason: Issue #534 — arena-production-compaction-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_534_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:arena-production-compaction-stats) '{}')", key));
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

} // namespace aura_issue_534_detail

int main() {
    using namespace aura_issue_534_detail;

    std::println("=== Issue #534: arena-production-compaction-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup + eval-current");

    // AC1: query:arena-production-compaction-stats returns hash
    {
        std::println("\n--- AC1: query:arena-production-compaction-stats ---");
        auto stats = cs.eval("(query:arena-production-compaction-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:arena-production-compaction-stats returns hash");
        CHECK(hash_int(cs, "fragmentation-ratio-pct") >= 0, "fragmentation-ratio-pct present");
        CHECK(hash_int(cs, "peak-used-bytes") >= 0, "peak-used-bytes present");
        CHECK(hash_int(cs, "auto-compact-triggers") >= 0, "auto-compact-triggers present");
        CHECK(hash_int(cs, "auto-compact-skips") >= 0, "auto-compact-skips present");
        CHECK(hash_int(cs, "auto-compact-guard-calls") >= 0, "auto-compact-guard-calls present");
        CHECK(hash_int(cs, "compactions") >= 0, "compactions present");
        CHECK(hash_int(cs, "bytes-saved") >= 0, "bytes-saved present");
        CHECK(hash_int(cs, "last-saved") >= 0, "last-saved present");
        CHECK(hash_int(cs, "defrag-attempted-count") >= 0, "defrag-attempted-count present");
        CHECK(hash_int(cs, "defrag-saved-bytes") >= 0, "defrag-saved-bytes present");
        CHECK(hash_int(cs, "compaction-yield-checks") >= 0, "compaction-yield-checks present");
        CHECK(hash_int(cs, "paused-by-boundary") >= 0, "paused-by-boundary present");
        CHECK(hash_int(cs, "gc-safepoint-waits") >= 0, "gc-safepoint-waits present");
        CHECK(hash_int(cs, "safepoint-coordination-count") >= 0,
              "safepoint-coordination-count present");
        CHECK(hash_int(cs, "mutation-volume") >= 0, "mutation-volume present");
        CHECK(hash_int(cs, "dirty-propagation") >= 0, "dirty-propagation present");
        CHECK(hash_int(cs, "compaction-efficiency-pct") >= 0, "compaction-efficiency-pct present");
        CHECK(hash_int(cs, "arena-production-compaction-total") >= 0,
              "arena-production-compaction-total present");
        CHECK(hash_int(cs, "arena-production-compaction-recommendation") >= 0,
              "arena-production-compaction-recommendation present");
    }

    const auto guard_before = hash_int(cs, "auto-compact-guard-calls");
    const auto total_before = hash_int(cs, "arena-production-compaction-total");

    // AC2: Guard mutate bumps compaction lifecycle counters
    {
        std::println("\n--- AC2: Guard mutate compaction lifecycle ---");
        CHECK(cs.eval("(mutate:rebind \"f\" \"(lambda (x) (+ x 2))\" \"issue534\")").has_value(),
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
        const auto total_after = hash_int(cs, "arena-production-compaction-total");
        CHECK(total_after >= total_before,
              std::format("arena-production-compaction-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related arena primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto compact_int = cs.eval("(query:arena-compaction-stats)");
        auto compact_hash = cs.eval("(query:arena-compaction-stats-hash)");
        auto auto_stats = cs.eval("(query:arena-auto-stats)");
        auto frag_snap = cs.eval("(query:arena-fragmentation-snapshot)");
        CHECK(compact_int && aura::compiler::types::is_int(*compact_int),
              "query:arena-compaction-stats int regression");
        CHECK(compact_hash && aura::compiler::types::is_hash(*compact_hash),
              "query:arena-compaction-stats-hash hash regression");
        CHECK(auto_stats && aura::compiler::types::is_hash(*auto_stats),
              "query:arena-auto-stats hash regression");
        CHECK(frag_snap && aura::compiler::types::is_hash(*frag_snap),
              "query:arena-fragmentation-snapshot hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 129,
              "stats:count >= 129");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}