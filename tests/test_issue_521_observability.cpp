// @category: integration
// @reason: Issue #521 — multi-fiber-orchestration-stats hash slice

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_521_detail {
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
    auto r = cs.eval(std::format("(hash-ref (query:multi-fiber-orchestration-stats) '{}')", key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static bool setup_workspace(aura::compiler::CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

} // namespace aura_issue_521_detail

int aura_issue_521_observability_run() {
    using namespace aura_issue_521_detail;

    std::println("=== Issue #521: multi-fiber-orchestration-stats hash ===");

    aura::compiler::CompilerService cs;
    CHECK(setup_workspace(cs), "workspace setup");

    // AC1: query:multi-fiber-orchestration-stats returns hash
    {
        std::println("\n--- AC1: query:multi-fiber-orchestration-stats ---");
        auto stats = cs.eval("(query:multi-fiber-orchestration-stats)");
        CHECK(stats && aura::compiler::types::is_hash(*stats),
              "query:multi-fiber-orchestration-stats returns hash");
        CHECK(hash_int(cs, "steal-attempts") >= 0, "steal-attempts present");
        CHECK(hash_int(cs, "steal-deferred-outermost") >= 0, "steal-deferred-outermost present");
        CHECK(hash_int(cs, "mutation-boundary-depth") >= 0, "mutation-boundary-depth present");
        CHECK(hash_int(cs, "gc-safepoint-requests") >= 0, "gc-safepoint-requests present");
        CHECK(hash_int(cs, "multi-fiber-orchestration-total") >= 0,
              "multi-fiber-orchestration-total present");
        CHECK(hash_int(cs, "multi-fiber-orchestration-recommendation") >= 0,
              "multi-fiber-orchestration-recommendation present");
    }

    const auto gc_req_before = hash_int(cs, "gc-safepoint-requests");
    const auto total_before = hash_int(cs, "multi-fiber-orchestration-total");

    // AC2: GC safepoint request bumps orchestration counters
    {
        std::println("\n--- AC2: mutate:request-gc-safepoint ---");
        (void)cs.eval("(mutate:request-gc-safepoint 30)");
        const auto gc_req_after = hash_int(cs, "gc-safepoint-requests");
        CHECK(gc_req_after > gc_req_before,
              std::format("gc-safepoint-requests bumped ({} -> {})", gc_req_before, gc_req_after));
    }

    // AC3: orchestration:tune-gc-frequency + scheduler-mutation-coord regression
    {
        std::println("\n--- AC3: orchestration:tune-gc-frequency ---");
        auto tune = cs.eval("(orchestration:tune-gc-frequency 80)");
        CHECK(tune && aura::compiler::types::is_int(*tune),
              "orchestration:tune-gc-frequency returns int");
        auto coord = cs.eval("(query:scheduler-mutation-coord-stats)");
        CHECK(coord && aura::compiler::types::is_hash(*coord),
              "query:scheduler-mutation-coord-stats hash regression");
    }

    // AC4: mutate cycle + legacy orchestration stats regression
    {
        std::println("\n--- AC4: regression ---");
        (void)cs.eval("(mutate:rebind \"x\" \"42\")");
        (void)cs.eval("(eval-current)");
        const auto total_after = hash_int(cs, "multi-fiber-orchestration-total");
        CHECK(total_after >= total_before,
              std::format("multi-fiber-orchestration-total monotonic ({} -> {})", total_before,
                          total_after));
        auto ros = cs.eval("(query:runtime-orchestration-stats)");
        CHECK(ros && aura::compiler::types::is_hash(*ros),
              "query:runtime-orchestration-stats hash regression");
        auto wss = cs.eval("(query:work-steal-stats)");
        CHECK(wss && aura::compiler::types::is_hash(*wss),
              "query:work-steal-stats hash regression");
    }

    // AC5: stats:count
    {
        std::println("\n--- AC5: stats:count ---");
        auto count = cs.eval("(stats:count)");
        CHECK(count && aura::compiler::types::is_int(*count) &&
                  aura::compiler::types::as_int(*count) >= 117,
              "stats:count >= 117");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_521_observability_run();
}
#endif
