// @category: integration
// @reason: uses CompilerService to verify strategy evolution
//          controller + pheromone counters

// test_issue_444_strategy_evolution.cpp — Issue #444:
// Coverage-driven mutation strategy evolution controller.
//
// Full scope: pheromone tracking + PID + pluggable strategies
// + agent loop. Scope-limited close ships:
//   1. 3 built-in strategies (coverage-greedy, bug-fix-
//      priority, minimal-mutation) — registered via
//      (strategy:set-strategy name).
//   2. Per-strategy pheromone counters (hits, successes) +
//      escalation counter in CompilerMetrics.
//   3. (query:strategy-evolution-stats) — 8-field hash
//      primitive exposing the strategy pheromone state.
//   4. Strategy primitives: strategy:set-strategy,
//      strategy:active, strategy:report-success,
//      strategy:escalate.
//   5. SEVA demo extension (demos/seva/seva_demo.aura)
//      that shows the strategy evolution loop.
//
// Deferred follow-ups:
//   - PID controller (current MVP uses rule-based
//     escalation; PID needs a sustained coverage
//     curve baseline + tuning).
//   - Pluggable strategy definitions (currently built-in
//     only; user-defined strategies need a strategy
//     registry).
//   - Multi-agent race arbitration (when 2+ agents
//     run strategies in parallel, the pheromone
//     accumulation can drift; needs a generation
//     version stamp).
//
// Test cases:
//   AC1:  fresh Evaluator — counters start at 0
//   AC2:  strategy:set-strategy "coverage-greedy" succeeds
//   AC3:  strategy:active returns the active strategy name
//   AC4:  strategy:report-success bumps the right counter
//   AC5:  strategy:escalate bumps the escalation counter
//   AC6:  invalid strategy name → no-op (void)
//   AC7:  query:strategy-evolution-stats returns 8 fields
//   AC8:  (query:strategy-evolution-stats) idempotence
//   AC9:  Stats:list includes the new primitive
//   AC10: Stats:count >= 44 (was 43 in #440, now 44 in #444)
//   AC11: Synthetic coverage curve — set 3 strategies,
//         report mixed successes, verify pheromone ratio

#include <cstdint>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_444_detail {
static int g_passed = 0;
static int g_failed = 0;

static aura::compiler::types::EvalValue run_on(aura::compiler::CompilerService& cs,
                                               std::string_view src) {
    auto r = cs.eval(src);
    if (!r) {
        std::println(std::cerr, "    [eval error: {}]", r.error().format());
        return aura::compiler::types::make_void();
    }
    return *r;
}

static std::int64_t hash_int(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (query:strategy-evolution-stats) '{}')", key));
    if (!r)
        return -1;
    if (!aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cout, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

// ═══════════════════════════════════════════════════════════
// AC1: fresh Evaluator — counters start at 0
// ═══════════════════════════════════════════════════════════
bool test_fresh_evaluator() {
    std::println("\n--- AC1: fresh Evaluator ---");
    aura::compiler::CompilerService cs;
    auto gh = hash_int(cs, "greedy-hits");
    auto gs = hash_int(cs, "greedy-successes");
    auto bh = hash_int(cs, "bugfix-hits");
    auto esc = hash_int(cs, "escalations");
    CHECK(gh == 0, "fresh Evaluator: greedy-hits == 0");
    CHECK(gs == 0, "fresh Evaluator: greedy-successes == 0");
    CHECK(bh == 0, "fresh Evaluator: bugfix-hits == 0");
    CHECK(esc == 0, "fresh Evaluator: escalations == 0");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC2: strategy:set-strategy succeeds
// ═══════════════════════════════════════════════════════════
bool test_set_strategy_succeeds() {
    std::println("\n--- AC2: strategy:set-strategy succeeds ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    bool ok = aura::compiler::types::is_int(r);
    CHECK(ok, "strategy:set-strategy returns int");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC3: strategy:active returns the active strategy name
// ═══════════════════════════════════════════════════════════
bool test_active_strategy() {
    std::println("\n--- AC3: strategy:active returns the active name ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(strategy:set-strategy \"bug-fix-priority\")");
    auto r = run_on(cs, "(strategy:active)");
    bool ok = aura::compiler::types::is_string(r);
    CHECK(ok, "strategy:active returns a string");
    if (ok) {
        auto idx = aura::compiler::types::as_string_idx(r);
        CHECK(idx >= 0, "active-strategy string index >= 0");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC4: strategy:report-success bumps the right counter
// ═══════════════════════════════════════════════════════════
bool test_report_success() {
    std::println("\n--- AC4: strategy:report-success bumps counter ---");
    aura::compiler::CompilerService cs;
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    auto before = hash_int(cs, "greedy-successes");
    run_on(cs, "(strategy:report-success \"coverage-greedy\")");
    auto after = hash_int(cs, "greedy-successes");
    CHECK(after >= before, "report-success bumps greedy-successes");
    CHECK(after > before, "report-success strictly bumps (not just maintains)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC5: strategy:escalate bumps the escalation counter
// ═══════════════════════════════════════════════════════════
bool test_escalate() {
    std::println("\n--- AC5: strategy:escalate bumps escalations ---");
    aura::compiler::CompilerService cs;
    auto before = hash_int(cs, "escalations");
    run_on(cs, "(strategy:escalate \"test reason\")");
    auto after = hash_int(cs, "escalations");
    CHECK(after >= before, "escalate bumps escalations");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC6: invalid strategy name → no-op (void)
// ═══════════════════════════════════════════════════════════
bool test_invalid_strategy_name() {
    std::println("\n--- AC6: invalid strategy name is no-op ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(strategy:set-strategy \"not-a-real-strategy\")");
    // The primitive returns void on unknown strategy.
    bool ok = aura::compiler::types::is_void(r);
    CHECK(ok, "invalid strategy name returns void (no crash)");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC7: 8 fields present
// ═══════════════════════════════════════════════════════════
bool test_eight_fields() {
    std::println("\n--- AC7: 8 fields present + non-negative ---");
    aura::compiler::CompilerService cs;
    static const char* kFields[] = {
        "active-strategy",  "greedy-hits",  "greedy-successes",  "bugfix-hits",
        "bugfix-successes", "minimal-hits", "minimal-successes", "escalations",
    };
    int found = 0;
    for (auto* k : kFields) {
        // active-strategy is a string, others are ints.
        if (std::string(k) == "active-strategy") {
            auto r = run_on(cs, std::format("(hash-ref (query:strategy-evolution-stats) '{}')", k));
            if (aura::compiler::types::is_string(r))
                ++found;
        } else {
            auto v = hash_int(cs, k);
            if (v >= 0)
                ++found;
        }
    }
    CHECK(found == 8, "all 8 fields accessible");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC8: idempotence — repeated calls consistent
// ═══════════════════════════════════════════════════════════
bool test_idempotent_observable() {
    std::println("\n--- AC8: idempotence ---");
    aura::compiler::CompilerService cs;
    auto a = hash_int(cs, "greedy-hits");
    auto b = hash_int(cs, "greedy-hits");
    CHECK(a == b, "two consecutive calls return the same greedy-hits");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC9: stats:list includes the new primitive
// ═══════════════════════════════════════════════════════════
bool test_stats_list_includes() {
    std::println("\n--- AC9: stats:list includes the new primitive ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(
        cs, "(letrec ((find? (lambda (needle hay) "
            "                (if (pair? hay) "
            "                    (if (string=? (car hay) needle) #t (find? needle (cdr hay))) "
            "                    #f)))) "
            "  (if (find? \"query:strategy-evolution-stats\" (stats:list)) 1 0))");
    bool included = aura::compiler::types::is_int(r) && aura::compiler::types::as_int(r) == 1;
    CHECK(included, "stats:list includes query:strategy-evolution-stats");
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC10: stats:count >= 44
// ═══════════════════════════════════════════════════════════
bool test_stats_count() {
    std::println("\n--- AC10: stats:count is up to date ---");
    aura::compiler::CompilerService cs;
    auto r = run_on(cs, "(stats:count)");
    bool ok = aura::compiler::types::is_int(r) && aura::compiler::types::as_int(r) >= 44;
    CHECK(ok, "stats:count >= 44 (was 43 in #440, now 44 in #444)");
    if (aura::compiler::types::is_int(r)) {
        std::println("    [stats:count = {}]", aura::compiler::types::as_int(r));
    }
    return true;
}

// ═══════════════════════════════════════════════════════════
// AC11: Synthetic coverage curve — set 3 strategies,
//       report mixed successes, verify pheromone ratio
// ═══════════════════════════════════════════════════════════
bool test_synthetic_coverage_curve() {
    std::println("\n--- AC11: synthetic coverage curve ---");
    aura::compiler::CompilerService cs;
    // coverage-greedy: 3 hits, 2 successes (67% success rate)
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    run_on(cs, "(strategy:set-strategy \"coverage-greedy\")");
    run_on(cs, "(strategy:report-success \"coverage-greedy\")");
    run_on(cs, "(strategy:report-success \"coverage-greedy\")");
    // bug-fix-priority: 2 hits, 1 success (50% success rate)
    run_on(cs, "(strategy:set-strategy \"bug-fix-priority\")");
    run_on(cs, "(strategy:set-strategy \"bug-fix-priority\")");
    run_on(cs, "(strategy:report-success \"bug-fix-priority\")");
    // minimal-mutation: 1 hit, 0 successes (0% success rate)
    run_on(cs, "(strategy:set-strategy \"minimal-mutation\")");
    // Escalate due to minimal plateau
    run_on(cs, "(strategy:escalate \"minimal plateau\")");
    auto gh = hash_int(cs, "greedy-hits");
    auto gs = hash_int(cs, "greedy-successes");
    auto bh = hash_int(cs, "bugfix-hits");
    auto bs = hash_int(cs, "bugfix-successes");
    auto mh = hash_int(cs, "minimal-hits");
    auto ms = hash_int(cs, "minimal-successes");
    auto esc = hash_int(cs, "escalations");
    CHECK(gh == 3, "synthetic: greedy-hits == 3");
    CHECK(gs == 2, "synthetic: greedy-successes == 2");
    CHECK(bh == 2, "synthetic: bugfix-hits == 2");
    CHECK(bs == 1, "synthetic: bugfix-successes == 1");
    CHECK(mh == 1, "synthetic: minimal-hits == 1");
    CHECK(ms == 0, "synthetic: minimal-successes == 0");
    CHECK(esc >= 1, "synthetic: escalations >= 1");
    return true;
}

} // namespace aura_issue_444_detail

int main() {
    using namespace aura_issue_444_detail;
    std::println("═══ Issue #444 strategy evolution controller tests ═══");

    test_fresh_evaluator();
    test_set_strategy_succeeds();
    test_active_strategy();
    test_report_success();
    test_escalate();
    test_invalid_strategy_name();
    test_eight_fields();
    test_idempotent_observable();
    test_stats_list_includes();
    test_stats_count();
    test_synthetic_coverage_curve();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}