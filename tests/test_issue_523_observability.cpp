// @category: integration
// @reason: Issue #523 — envframe-production-safety-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_523_detail {
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
        "(hash-ref (engine:metrics \"query:envframe-production-safety-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_closure_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define (f x) x) (define a 1) (f 42)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_523_detail

int aura_issue_523_observability_run() {
    using namespace aura_issue_523_detail;

    std::println("=== Issue #523: envframe-production-safety-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_closure_workspace(cs), "closure workspace setup");

    // AC1: query:envframe-production-safety-stats returns hash
    {
        std::println("\n--- AC1: query:envframe-production-safety-stats ---");
        auto stats = cs.eval("(query:envframe-production-safety-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:envframe-production-safety-stats returns hash");
        CHECK(hash_int(cs, "dual-path-desync") >= 0, "dual-path-desync present");
        CHECK(hash_int(cs, "dual-path-sync-count") >= 0, "dual-path-sync-count present");
        CHECK(hash_int(cs, "stale-refresh-count") >= 0, "stale-refresh-count present");
        CHECK(hash_int(cs, "version-mismatch-in-walk") >= 0, "version-mismatch-in-walk present");
        CHECK(hash_int(cs, "gc-walk-safe-skips") >= 0, "gc-walk-safe-skips present");
        CHECK(hash_int(cs, "gc-envframe-stale-skipped") >= 0, "gc-envframe-stale-skipped present");
        CHECK(hash_int(cs, "post-rollback-invalidations") >= 0,
              "post-rollback-invalidations present");
        CHECK(hash_int(cs, "defuse-version") >= 0, "defuse-version present");
        CHECK(hash_int(cs, "dual-path-sync-rate-pct") >= 0, "dual-path-sync-rate-pct present");
        CHECK(hash_int(cs, "envframe-production-safety-total") >= 0,
              "envframe-production-safety-total present");
        CHECK(hash_int(cs, "envframe-production-safety-recommendation") >= 0,
              "envframe-production-safety-recommendation present");
    }

    const auto total_before = hash_int(cs, "envframe-production-safety-total");
    const auto sync_before = hash_int(cs, "dual-path-sync-count");

    // AC2: evaluator bump accessors increase production counters
    {
        std::println("\n--- AC2: envframe bump accessors ---");
        auto& ev = cs.evaluator();
        ev.bump_bindings_dual_sync_count();
        ev.bump_envframe_stale_refresh_count();
        ev.bump_envframe_version_mismatch_in_walk();
        ev.bump_envframe_gc_walk_safe_skips();
        ev.bump_envframe_post_rollback_invalidations(2);
        cs.bump_gc_envframe_stale_skipped();
        const auto sync_after = hash_int(cs, "dual-path-sync-count");
        CHECK(sync_after > sync_before,
              std::format("dual-path-sync-count grew ({} -> {})", sync_before, sync_after));
        CHECK(hash_int(cs, "stale-refresh-count") >= 1, "stale-refresh-count bumped");
        CHECK(hash_int(cs, "version-mismatch-in-walk") >= 1, "version-mismatch-in-walk bumped");
        CHECK(hash_int(cs, "gc-walk-safe-skips") >= 1, "gc-walk-safe-skips bumped");
        CHECK(hash_int(cs, "gc-envframe-stale-skipped") >= 1, "gc-envframe-stale-skipped bumped");
        CHECK(hash_int(cs, "post-rollback-invalidations") >= 2,
              "post-rollback-invalidations bumped");
    }

    // AC3: mutate + closure call path
    {
        std::println("\n--- AC3: mutate + closure call ---");
        (void)cs.eval("(mutate:rebind \"a\" \"10\")");
        (void)cs.eval("(eval-current)");
        for (int i = 0; i < 3; ++i) {
            (void)cs.eval("(f 42)");
        }
        const auto total_after = hash_int(cs, "envframe-production-safety-total");
        CHECK(total_after >= total_before,
              std::format("envframe-production-safety-total monotonic ({} -> {})", total_before,
                          total_after));
    }

    // AC4: related EnvFrame primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto eds = cs.eval("(query:envframe-dualpath-stats)");
        auto stale = cs.eval("(query:envframe-dualpath-stale-stats)");
        auto ces = cs.eval("(query:closure-env-safety-stats)");
        CHECK(eds && aura::compiler::types::is_int(*eds), "envframe-dualpath-stats regression");
        CHECK(stale && aura::compiler::types::is_int(*stale),
              "envframe-dualpath-stale-stats regression");
        CHECK(ces && aura::compiler::types::is_hash(*ces),
              "closure-env-safety-stats hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 119,
              "stats:count >= 119");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_523_observability_run();
}
#endif
