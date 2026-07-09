// test_runtime_concurrent_full_cycle_chaos.cpp — Issue #755:
// End-to-end concurrent safety full-cycle integration observability
// (MutationBoundary + fiber steal + AOT hot-reload + GC safepoint +
// panic recovery; refines #732/#731/#730/#674/#739).
//
//   - AC1:  query:concurrent-safety-full-cycle-stats reachable (schema 755)
//   - AC2:  steal-boundary-success bumps on direct path
//   - AC3:  aot-reload-at-guard bumps on direct path
//   - AC4:  gc-safepoint-during-steal bumps on direct path
//   - AC5:  recovery-success bumps on direct path
//   - AC6:  safety-events-total == sum of 4 per-counter fields
//   - AC7:  real full-cycle exercise (steal probe + migration + checkpoint)
//   - AC8:  query regression (#674 chaos-stats, #754 orchestration-llm)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_probe_linear_on_steal();
extern "C" void aura_evaluator_resume_fiber_migration();

namespace aura_issue_755_detail {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (query:concurrent-safety-full-cycle-stats) '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t steal_boundary_success(CompilerService& cs) {
    return stat_int(cs, "steal-boundary-success");
}
static std::int64_t aot_reload_at_guard(CompilerService& cs) {
    return stat_int(cs, "aot-reload-at-guard");
}
static std::int64_t gc_safepoint_during_steal(CompilerService& cs) {
    return stat_int(cs, "gc-safepoint-during-steal");
}
static std::int64_t recovery_success(CompilerService& cs) {
    return stat_int(cs, "recovery-success");
}
static std::int64_t events_total(CompilerService& cs) {
    return stat_int(cs, "safety-events-total");
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:concurrent-safety-full-cycle-stats (schema 755) ---");
    auto h = cs.eval("(query:concurrent-safety-full-cycle-stats)");
    CHECK(h && is_hash(*h), "concurrent-safety-full-cycle-stats returns hash");
    CHECK(stat_int(cs, "schema") == 755, "schema == 755");
    CHECK(steal_boundary_success(cs) >= 0, "steal-boundary-success non-negative");
    CHECK(aot_reload_at_guard(cs) >= 0, "aot-reload-at-guard non-negative");
    CHECK(gc_safepoint_during_steal(cs) >= 0, "gc-safepoint-during-steal non-negative");
    CHECK(recovery_success(cs) >= 0, "recovery-success non-negative");

    std::println("\n--- AC2: steal-boundary-success bumps on direct path ---");
    const auto s0 = steal_boundary_success(cs);
    cs.evaluator().bump_concurrent_safety_steal_boundary_success();
    cs.evaluator().bump_concurrent_safety_steal_boundary_success();
    CHECK(steal_boundary_success(cs) == s0 + 2, "steal-boundary-success bumps by exactly 2");

    std::println("\n--- AC3: aot-reload-at-guard bumps on direct path ---");
    const auto a0 = aot_reload_at_guard(cs);
    cs.evaluator().bump_concurrent_safety_aot_reload_at_guard();
    CHECK(aot_reload_at_guard(cs) == a0 + 1, "aot-reload-at-guard bumps by exactly 1");

    std::println("\n--- AC4: gc-safepoint-during-steal bumps on direct path ---");
    const auto g0 = gc_safepoint_during_steal(cs);
    cs.evaluator().bump_concurrent_safety_gc_safepoint_during_steal();
    cs.evaluator().bump_concurrent_safety_gc_safepoint_during_steal();
    cs.evaluator().bump_concurrent_safety_gc_safepoint_during_steal();
    CHECK(gc_safepoint_during_steal(cs) == g0 + 3, "gc-safepoint-during-steal bumps by exactly 3");

    std::println("\n--- AC5: recovery-success bumps on direct path ---");
    const auto r0 = recovery_success(cs);
    cs.evaluator().bump_concurrent_safety_recovery_success();
    cs.evaluator().bump_concurrent_safety_recovery_success();
    CHECK(recovery_success(cs) == r0 + 2, "recovery-success bumps by exactly 2");

    std::println("\n--- AC6: safety-events-total == sum ---");
    const auto s = steal_boundary_success(cs);
    const auto a = aot_reload_at_guard(cs);
    const auto g = gc_safepoint_during_steal(cs);
    const auto r = recovery_success(cs);
    const auto tot = events_total(cs);
    CHECK(tot == s + a + g + r, "safety-events-total == sum of 4 counters");

    std::println("\n--- AC7: real full-cycle exercise ---");
    const auto ev7a = events_total(cs);
    const auto s7a = steal_boundary_success(cs);
    const auto g7a = gc_safepoint_during_steal(cs);
    const auto r7a = recovery_success(cs);

    cs.evaluator().bind_yield_hook_evaluator();
    aura_evaluator_probe_linear_on_steal();
    cs.evaluator().unbind_yield_hook_evaluator();
    aura_evaluator_resume_fiber_migration();

    CHECK(steal_boundary_success(cs) > s7a,
          "steal-boundary-success grew after probe_linear_on_steal");
    CHECK(gc_safepoint_during_steal(cs) > g7a,
          "gc-safepoint-during-steal grew after resume_fiber_migration");

    cs.eval("(set-code \"(define base 1) base\")");
    cs.eval("(eval-current)");
    const auto ck7a = recovery_success(cs);
    auto saved = cs.eval("(panic-checkpoint)");
    CHECK(saved && is_bool(*saved) && as_bool(*saved), "panic-checkpoint succeeds");
    cs.eval("(set-code \"(define base 999) base\")");
    cs.eval("(eval-current)");
    auto restored = cs.eval("(panic-restore)");
    CHECK(restored && is_bool(*restored) && as_bool(*restored), "panic-restore succeeds");
    CHECK(recovery_success(cs) > r7a, "recovery-success grew after panic-restore");
    CHECK(recovery_success(cs) > ck7a, "recovery-success grew after checkpoint cycle");
    CHECK(events_total(cs) > ev7a, "safety-events-total monotonic over full-cycle matrix");

    std::println("\n--- AC8: query regression ---");
    auto chaos = cs.eval("(query:self-evolution-chaos-stats)");
    auto orch = cs.eval("(query:orchestration-llm-bottleneck-stats)");
    CHECK(chaos && is_hash(*chaos), "self-evolution-chaos-stats regression (#674)");
    CHECK(orch && is_hash(*orch), "orchestration-llm-bottleneck-stats regression (#754)");
}

} // namespace aura_issue_755_detail

int aura_issue_runtime_concurrent_full_cycle_chaos_run() {
    aura::compiler::CompilerService cs;
    aura_issue_755_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_runtime_concurrent_full_cycle_chaos_run();
}
#endif
