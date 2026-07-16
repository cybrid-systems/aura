// @category: integration
// @reason: Issue #1470 — query:ai-closedloop-readiness-stats
// consolidated AI closed-loop readiness observability primitive.
//
// Scope-limited close matching #457 / #470 / #527 / #738 / #632 /
// #622 / #918 pattern.
//
// Discovery before this PR (no duplication): the 5 counter types
// that feed this primitive are already exposed via scattered
// primitives:
//   - generation_wrap_count      — #457 query:stable-ref-stats +
//                                  #470 stable-ref-stats-hash[0]
//   - stable_ref_invalidations   — #457 + #470 stable-ref-stats-hash[1]
//   - atomic_batch_commits       — #192/#213 + #622 atomic-batch-stats-hash
//   - macro_hygiene_skipped      — #918 Phase 1 (via
//                                  InlinePass::total_macro_hygiene_skipped()
//                                  accessed through ir_inline_hygiene_skipped
//                                  helper in this file)
//   - mark_dirty_boundary_prune_count — exposed via workspace
//                                  accessor at src/core/ast.ixx:3964
//
// What the issue body asks for is a CONSOLIDATED hash view that
// surfaces all 5 in one call for AI editing loops. #1470 ships ONE
// new Aura primitive `query:ai-closedloop-readiness-stats` with the
// 5 fields + recommendation. AI Agent consumes this instead of
// calling 5 separate stats primitives.

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_1470_detail {

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
        "(hash-ref (engine:metrics \"query:ai-closedloop-readiness-stats\") '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace aura_issue_1470_detail

int aura_issue_1470_run() {
    using namespace aura_issue_1470_detail;
    std::println("=== Issue #1470: query:ai-closedloop-readiness-stats ===");

    aura::compiler::CompilerService cs;

    // AC1: primitive returns a hash with all 6 documented fields
    // (5 counters + recommendation). All values present and >= 0;
    // recommendation in [0, 4].
    {
        std::println(
            "\n--- AC1: (engine:metrics \"query:ai-closedloop-readiness-stats\") shape ---");
        auto h = cs.eval("(engine:metrics \"query:ai-closedloop-readiness-stats\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "ai-closedloop-readiness-stats returns a hash");

        const auto wraps = hash_int(cs, "wraps");
        const auto invalidations = hash_int(cs, "invalidations");
        const auto batch_commits = hash_int(cs, "batch-commits");
        const auto hygiene_skips = hash_int(cs, "hygiene-skips");
        const auto dirty_prunes = hash_int(cs, "dirty-prunes");
        const auto rec = hash_int(cs, "recommendation");

        CHECK(wraps >= 0, std::format("wraps >= 0 (got {})", wraps));
        CHECK(invalidations >= 0, std::format("invalidations >= 0 (got {})", invalidations));
        CHECK(batch_commits >= 0, std::format("batch-commits >= 0 (got {})", batch_commits));
        CHECK(hygiene_skips >= 0, std::format("hygiene-skips >= 0 (got {})", hygiene_skips));
        CHECK(dirty_prunes >= 0, std::format("dirty-prunes >= 0 (got {})", dirty_prunes));
        CHECK(rec >= 0 && rec <= 4, std::format("recommendation in [0,4] (got {})", rec));
    }

    // AC2: fresh service — all 5 counters at 0, recommendation 0
    // (AC1 only reads counters via eval — no bumps). Verifies the
    // primitive is queryable on a brand-new CompilerService and the
    // documented baseline matches reality.
    {
        std::println("\n--- AC2: fresh service baseline (all zeros) ---");
        const auto wraps = hash_int(cs, "wraps");
        const auto invalidations = hash_int(cs, "invalidations");
        const auto batch_commits = hash_int(cs, "batch-commits");
        const auto hygiene_skips = hash_int(cs, "hygiene-skips");
        const auto dirty_prunes = hash_int(cs, "dirty-prunes");
        const auto rec = hash_int(cs, "recommendation");
        CHECK(wraps == 0, std::format("fresh wraps == 0 (got {})", wraps));
        CHECK(invalidations == 0, std::format("fresh invalidations == 0 (got {})", invalidations));
        CHECK(batch_commits == 0, std::format("fresh batch-commits == 0 (got {})", batch_commits));
        CHECK(hygiene_skips == 0, std::format("fresh hygiene-skips == 0 (got {})", hygiene_skips));
        CHECK(dirty_prunes == 0, std::format("fresh dirty-prunes == 0 (got {})", dirty_prunes));
        CHECK(rec == 0, std::format("fresh recommendation == 0 (got {})", rec));
    }

    // AC3: existing primitives remain reachable (back-compat —
    // #1470 doesn't disturb the existing surface).
    {
        std::println("\n--- AC3: existing primitives back-compat ---");
        auto h = cs.eval("(engine:metrics \"query:stable-ref-stats-hash\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "stable-ref-stats-hash still returns a hash");
        h = cs.eval("(engine:metrics \"query:atomic-batch-stats-hash\")");
        CHECK(h && aura::compiler::types::is_hash(*h),
              "atomic-batch-stats-hash still returns a hash");
        h = cs.eval("(engine:metrics \"query:stable-ref-cow-fiber-stats\")");
        CHECK(h && aura::compiler::types::is_int(*h),
              "stable-ref-cow-fiber-stats still returns an int");
    }

    // AC4: recommendation enum contract documented:
    //   0 = healthy (all counters below thresholds)
    //   1 = wraps detected
    //   2 = high invalidation rate (>= 10)
    //   3 = high hygiene-skip rate (>= 100)
    //   4 = high dirty-prune rate (>= 50)
    // On a fresh service all 5 are at 0 → rec must be 0.
    {
        std::println("\n--- AC4: recommendation enum contract ---");
        const auto rec = hash_int(cs, "recommendation");
        CHECK(rec == 0, std::format("fresh service recommendation == 0 (got {})", rec));
    }

    std::println("\n--- harness totals ---");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

int main() {
    return aura_issue_1470_run();
}