// @category: integration
// @reason: Issue #516 — prompt6-memory-safety-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_516_detail {
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:prompt6-memory-safety-stats\") '{}')", key));
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

} // namespace aura_issue_516_detail

int aura_issue_516_observability_run() {
    using namespace aura_issue_516_detail;

    std::println("=== Issue #516: prompt6-memory-safety-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_closure_workspace(cs), "closure workspace setup");

    // AC1: query:prompt6-memory-safety-stats returns hash
    {
        std::println("\n--- AC1: query:prompt6-memory-safety-stats ---");
        auto stats = cs.eval("(engine:metrics \"query:prompt6-memory-safety-stats\")");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:prompt6-memory-safety-stats returns hash");
        CHECK(hash_int(cs, "bridge-epoch-hits") >= 0, "bridge-epoch-hits present");
        CHECK(hash_int(cs, "linear-check-passes") >= 0, "linear-check-passes present");
        CHECK(hash_int(cs, "gc-safepoint-waits") >= 0, "gc-safepoint-waits present");
        CHECK(hash_int(cs, "violation-count") >= 0, "violation-count present");
        CHECK(hash_int(cs, "safety-score") >= 0, "safety-score present");
        CHECK(hash_int(cs, "prompt6-memory-safety-total") >= 0,
              "prompt6-memory-safety-total present");
        CHECK(hash_int(cs, "prompt6-memory-safety-recommendation") >= 0,
              "prompt6-memory-safety-recommendation present");
    }

    const auto safety_before = hash_int(cs, "safety-score");
    const auto total_before = hash_int(cs, "prompt6-memory-safety-total");

    // AC2: bump accessors increase safety-score
    {
        std::println("\n--- AC2: safety counter bumps ---");
        cs.bump_bridge_epoch_hit_count();
        cs.bump_linear_check_pass_count();
        cs.bump_gc_envframe_stale_skipped();
        const auto safety_after = hash_int(cs, "safety-score");
        CHECK(safety_after > safety_before,
              std::format("safety-score grew ({} -> {})", safety_before, safety_after));
        CHECK(hash_int(cs, "bridge-epoch-hits") >= 1, "bridge-epoch-hits bumped");
        CHECK(hash_int(cs, "linear-check-passes") >= 1, "linear-check-passes bumped");
        CHECK(hash_int(cs, "gc-envframe-skipped") >= 1, "gc-envframe-skipped bumped");
    }

    // AC3: GC safepoint request bumps gc-safepoint-waits
    {
        std::println("\n--- AC3: GC safepoint coordination ---");
        const auto waits_before = hash_int(cs, "gc-safepoint-waits");
        (void)cs.eval("(mutate:request-gc-safepoint 20)");
        const auto waits_after = hash_int(cs, "gc-safepoint-waits");
        CHECK(waits_after > waits_before,
              std::format("gc-safepoint-waits grew ({} -> {})", waits_before, waits_after));
    }

    // AC4: mutate + closure call regression
    {
        std::println("\n--- AC4: mutate + closure regression ---");
        (void)cs.eval("(mutate:rebind \"a\" \"10\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(f 42)");
        const auto total_after = hash_int(cs, "prompt6-memory-safety-total");
        CHECK(total_after >= total_before,
              std::format("prompt6-memory-safety-total monotonic ({} -> {})", total_before,
                          total_after));
        auto ces = cs.eval("(engine:metrics \"query:closure-env-safety-stats\")");
        CHECK(ces && aura::compiler::types::is_hash(*ces),
              "query:closure-env-safety-stats hash regression");
    }

    // AC5: legacy Prompt6 int-sum primitives regression
    {
        std::println("\n--- AC5: Prompt6 int-sum regression ---");
        auto viol = cs.eval("(stats:get \"query:prompt6-violation-count\")");
        auto score = cs.eval("(stats:get \"query:prompt6-safety-score\")");
        CHECK(viol && aura::compiler::types::is_int(*viol),
              "query:prompt6-violation-count int regression");
        CHECK(score && aura::compiler::types::is_int(*score),
              "query:prompt6-safety-score int regression");
        CHECK(hash_int(cs, "violation-count") == aura::compiler::types::as_int(*viol),
              "hash violation-count matches int primitive");
        CHECK(hash_int(cs, "safety-score") == aura::compiler::types::as_int(*score),
              "hash safety-score matches int primitive");
    }

    // AC6: stats:count
    {
        std::println("\n--- AC6: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 115,
              "stats:count >= 115");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_516_observability_run();
}
#endif
