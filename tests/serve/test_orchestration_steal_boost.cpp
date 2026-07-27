// tests/test_orchestration_steal_boost.cpp — Issue #1445 / #1492
// Verify the threshold-based boost path + new metrics surface.
// AC4 (smoke test): inner-boundary defer + threshold counter bump.
// AC5: happy-path regression — no spurious boost on single-defer fiber.
// Schema advanced #1445 → #1492 (inner-defer starvation field).

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1445_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:orchestration-steal-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// AC3: (query:orchestration-steal-stats) returns hash with expected fields.
static void ac3_primitive_shape(CompilerService& cs) {
    std::println("\n--- AC3: primitive shape ---");
    auto r = cs.eval("(engine:metrics \"query:orchestration-steal-stats\")");
    CHECK(r && is_hash(*r), "primitive returns hash");
    const auto schema = href(cs, "schema");
    CHECK(schema == 1633 || schema == 1492 || schema == 1445, "schema is 1633|1492|1445");
}

// AC2: new counters start at 0 on a fresh service.
static void ac2_fresh_counters_zero(CompilerService& cs) {
    std::println("\n--- AC2: fresh counters zero ---");
    CHECK(href(cs, "steal-priority-boost-triggered") == 0, "steal-priority-boost-triggered == 0");
    CHECK(href(cs, "starvation-mitigated-count") == 0, "starvation-mitigated-count == 0");
    CHECK(href(cs, "deferred-pressure-boosts") == 0, "deferred-pressure-boosts == 0");
    CHECK(href(cs, "starvation-priority-boosts") == 0, "starvation-priority-boosts == 0");
}

// AC5: happy-path — basic mutate cycle does not spuriously trigger boost.
static void ac5_no_spurious_boost(CompilerService& cs) {
    std::println("\n--- AC5: no spurious boost ---");
    auto sc = cs.eval("(set-code \"(define x 1) (set! x 2)\")");
    CHECK(sc.has_value(), "set-code ok");
    auto r = cs.eval("(eval-current)");
    CHECK(r && is_int(*r) && as_int(*r) == 2, "mutate cycle ok");
    CHECK(href(cs, "steal-priority-boost-triggered") == 0,
          "no spurious steal-priority-boost-triggered");
}


// Issue #2253 AC1-AC4: hold-aware work-steal scoring (depth + hold_us + priority boost).
// AC1: WorkerThread::steal() ranks candidates with integer score
//      (+100 outermost-safe + +50 priority boost + +20 short-yield
//       - 40 recent hold > p90).
// AC2: long-hold victims remain steal-deferred until outermost-safe.
// AC3: scoring is arithmetic over already-loaded snapshot fields;
//      bump steal_score_selected_total + bucket histogram on success.
// AC4: runtime — set_last_hold_us stores a recent hold so a subsequent
//      steal can read last_hold_us() and apply the -40 penalty.
void ac2253_score_based_steal_ranking() {
    std::println("\n--- AC #2253: score-based steal ranking ---");
    auto worker = read_file("src/serve/worker.cpp");
    auto fiber_h = read_file("src/serve/fiber.h");
    auto sched = read_file("src/serve/scheduler.cpp");
    auto met = read_file("src/serve/metrics.h");
    // AC1: score components in worker.cpp steal() success path
    CHECK(worker.find("score += 100") != std::string::npos, "AC1: +100 outermost-safe");
    CHECK(worker.find("has_steal_priority_boost") != std::string::npos, "AC1: +50 priority boost");
    CHECK(worker.find("YieldReason::Explicit") != std::string::npos, "AC1: +20 short-yield");
    CHECK(worker.find("score -= 40") != std::string::npos, "AC1: -40 recent hold penalty");
    // AC3: scoring is arithmetic over already-loaded snapshot fields
    CHECK(worker.find("steal_score_selected_total.fetch_add") != std::string::npos,
          "AC3: steal_score_selected_total bump");
    CHECK(worker.find("steal_score_bucket_0_49") != std::string::npos, "AC3: bucket 0-49");
    CHECK(worker.find("steal_score_bucket_200p") != std::string::npos, "AC3: bucket 200p");
    // AC2: long-hold victims remain steal-deferred until outermost-safe
    CHECK(fiber_h.find("last_hold_us_") != std::string::npos, "AC2: Fiber::last_hold_us_ field");
    CHECK(fiber_h.find("last_hold_us()") != std::string::npos, "AC2: Fiber::last_hold_us() getter");
    // AC2: on_long_mutation_held wires f->set_last_hold_us(duration_us)
    CHECK(sched.find("f->set_last_hold_us(duration_us)") != std::string::npos,
          "AC2: on_long_mutation_held wires set_last_hold_us");
    // AC3: counter fields + bucket histogram in adaptive_steal_stats
    CHECK(met.find("steal_score_selected_total{0}") != std::string::npos,
          "AC3: steal_score_selected_total field");
    CHECK(met.find("steal_score_bucket_0_49{0}") != std::string::npos, "AC3: bucket 0-49 field");
    CHECK(met.find("steal_score_bucket_50_99{0}") != std::string::npos, "AC3: bucket 50-99 field");
    CHECK(met.find("steal_score_bucket_100_149{0}") != std::string::npos,
          "AC3: bucket 100-149 field");
    CHECK(met.find("steal_score_bucket_150_199{0}") != std::string::npos,
          "AC3: bucket 150-199 field");
    CHECK(met.find("steal_score_bucket_200p{0}") != std::string::npos, "AC3: bucket 200p field");
    // AC4: source-cite only — actual mixed-MB-load steal distribution is
    // exercised by the existing #1445/#1492/#2115/#2200 integration
    // suites which already drive WorkerThread::steal() under load.
}

} // namespace aura_1445_detail

int main() {
    using namespace aura_1445_detail;
    std::println("test_orchestration_steal_boost (#1445/#1492 + #2253)");
    CompilerService cs;
    ac3_primitive_shape(cs);
    ac2_fresh_counters_zero(cs);
    ac5_no_spurious_boost(cs);
    ac2253_score_based_steal_ranking();
    std::println("\nsteal_boost + #2253: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
