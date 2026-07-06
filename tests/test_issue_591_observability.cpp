// @category: integration
// @reason: Issue #591 — scheduler-mutation-coord-stats steal/GC slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_591_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:scheduler-mutation-coord-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define acc 0)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_591_detail

int main() {
    using namespace aura_issue_591_detail;

    std::println("=== Issue #591: scheduler-mutation-coord-stats steal/GC coordination ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:scheduler-mutation-coord-stats returns hash with #591 fields
    {
        std::println("\n--- AC1: query:scheduler-mutation-coord-stats ---");
        auto stats = cs.eval("(query:scheduler-mutation-coord-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:scheduler-mutation-coord-stats returns hash");
        CHECK(hash_int(cs, "gc-pauses-attributed-to-mutation") >= 0,
              "gc-pauses-attributed-to-mutation present (#618)");
        CHECK(hash_int(cs, "mutation-boundary-depth") >= 0, "mutation-boundary-depth present");
        CHECK(hash_int(cs, "gc-frequency-tune-ratio") >= 0 &&
                  hash_int(cs, "gc-frequency-tune-ratio") <= 100,
              "gc-frequency-tune-ratio in [0,100]");
        CHECK(hash_int(cs, "schema") == 618, "schema == 618 (#618 regression)");
        CHECK(hash_int(cs, "steal-deferred-count") >= 0, "steal-deferred-count present");
        CHECK(hash_int(cs, "safepoint-wait-on-boundary-us") >= 0,
              "safepoint-wait-on-boundary-us present");
        CHECK(hash_int(cs, "wakeup-after-defer-success") >= 0,
              "wakeup-after-defer-success present");
        CHECK(hash_int(cs, "mutation-coord-schema") == 591, "mutation-coord-schema == 591");
        CHECK(hash_int(cs, "scheduler-mutation-coord-total") >= 0,
              "scheduler-mutation-coord-total present");
        CHECK(hash_int(cs, "scheduler-mutation-coord-recommendation") >= 0,
              "scheduler-mutation-coord-recommendation present");
    }

    const auto gc_req_before = hash_int(cs, "scheduler-mutation-coord-total");

    // AC2: GC safepoint request + tune ratio coordination
    {
        std::println("\n--- AC2: GC safepoint + tune-gc-frequency ---");
        (void)cs.eval("(mutate:request-gc-safepoint 25)");
        (void)cs.eval("(orchestration:tune-gc-frequency 70)");
        const auto ratio = hash_int(cs, "gc-frequency-tune-ratio");
        CHECK(ratio == 70, std::format("gc-frequency-tune-ratio == 70 (got {})", ratio));
        const auto total_after = hash_int(cs, "scheduler-mutation-coord-total");
        CHECK(total_after >= gc_req_before,
              std::format("scheduler-mutation-coord-total monotonic ({} -> {})", gc_req_before,
                          total_after));
        CHECK(hash_int(cs, "safepoint-wait-on-boundary-us") >= 0,
              "safepoint-wait-on-boundary-us readable after GC request");
        (void)cs.eval("(orchestration:tune-gc-frequency 50)");
    }

    // AC3: MutationBoundary Guard + mutate/query cycle
    {
        std::println("\n--- AC3: mutate + query under Guard ---");
        (void)cs.eval("(mutate:rebind \"acc\" \"42\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern \"acc\")");
        CHECK(hash_int(cs, "mutation-boundary-depth") >= 0,
              "mutation-boundary-depth readable after mutate cycle");
        CHECK(hash_int(cs, "wakeup-after-defer-success") >= 0,
              "wakeup-after-defer-success readable after mutate cycle");
    }

    // AC4: related scheduler/orchestration primitive regression
    {
        std::println("\n--- AC4: regression ---");
        auto work_steal = cs.eval("(query:work-steal-stats)");
        auto multi = cs.eval("(query:multi-fiber-orchestration-stats)");
        auto stealbudget = cs.eval("(query:scheduler-stealbudget-adaptive-stats)");
        auto legacy = cs.eval("(query:orchestration-metrics)");
        CHECK(work_steal && aura::compiler::types::is_hash(*work_steal),
              "query:work-steal-stats hash regression (#500)");
        CHECK(multi && aura::compiler::types::is_hash(*multi),
              "query:multi-fiber-orchestration-stats hash regression (#521)");
        CHECK(stealbudget && aura::compiler::types::is_hash(*stealbudget),
              "query:scheduler-stealbudget-adaptive-stats hash regression (#706)");
        CHECK(legacy && aura::compiler::types::is_string(*legacy),
              "query:orchestration-metrics string regression (#451)");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 168,
              "stats:count >= 168");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}