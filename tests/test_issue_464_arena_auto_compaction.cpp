// @category: unit
// @reason: pure C++ test of ArenaGroup auto-compaction
//          lifecycle + EDSL (query:arena-auto-stats) primitive

// test_issue_464_arena_auto_compaction.cpp — Issue #464:
// Arena auto-compaction policy + fiber scheduler / AI
// orchestration integration (refine #430).
//
// Full scope is multi-week (per-arena auto-trigger
// with EMA-tuned threshold, fiber-safety yield during
// compaction, scheduler hook, MutationBoundaryGuard
// auto-call, debug-mode logs).
//
// Scope-limited close ships the COUNTER + ENTRY-POINT
// LAYER (precondition for the rest):
//   1. ArenaGroup::auto_compact_with_safety() — production
//      entry point that combines adaptive_compact_all()
//      with the closed-loop counters
//   2. ArenaGroup::bump_auto_compact_guard_call() —
//      called from MutationBoundaryGuard dtor
//   3. ArenaGroup::bump_compaction_yield_check() — fiber-
//      safety counter helper
//   4. 2 new CompilerMetrics atomics:
//      auto_compact_guard_call_count,
//      compaction_yield_checks
//   5. MutationBoundaryGuard dtor calls
//      bump_auto_compact_guard_call() on outermost +
//      success path
//   6. (query:arena-auto-stats) Aura primitive — 4-field
//      hash
//   7. (stats:count) 50 → 51
//
// Test cases:
//   AC1:  ArenaGroup::auto_compact_with_safety returns 0
//         on an empty group
//   AC2:  ArenaGroup::bump_auto_compact_guard_call bumps
//         the counter
//   AC3:  ArenaGroup::bump_compaction_yield_check bumps
//         the counter
//   AC4:  auto_compact_with_safety bumps both counters
//   AC5:  EDSL: (query:arena-auto-stats) returns a hash
//         with 4 fields
//   AC6:  EDSL: guard-call counter is observable (post-
//         mutate increments it via the guard dtor)
//   AC7:  EDSL: (stats:count) >= 51
//   AC8:  EDSL: (stats:list) includes query:arena-auto-stats
//   AC9:  Fresh service: all 4 counters default to 0
//   AC10: After many mutate calls, guard-call counter
//         advances (long-AI-session closed-loop signal)

#include "test_harness.hpp"

import std;
import aura.core;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.diag;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_464_detail {

using aura::test::g_failed;
using aura::test::g_passed;

// ── AC1: empty group auto_compact_with_safety returns 0
bool test_empty_group() {
    std::println("\n--- AC1: empty group auto_compact_with_safety ---");
    aura::ast::ArenaGroup g;
    auto saved = g.auto_compact_with_safety();
    CHECK(saved == 0, "0 bytes reclaimed on empty group");
    CHECK(g.auto_compact_guard_call_count() == 1, "guard-call counter == 1");
    CHECK(g.compaction_yield_checks_group() == 1, "yield-check counter == 1");
    return true;
}

// ── AC2: bump_auto_compact_guard_call bumps the counter
bool test_bump_guard_call() {
    std::println("\n--- AC2: bump_auto_compact_guard_call ---");
    aura::ast::ArenaGroup g;
    CHECK(g.auto_compact_guard_call_count() == 0, "starts at 0");
    g.bump_auto_compact_guard_call();
    g.bump_auto_compact_guard_call();
    g.bump_auto_compact_guard_call();
    CHECK(g.auto_compact_guard_call_count() == 3,
          std::format("after 3 bumps == 3 (got {})", g.auto_compact_guard_call_count()));
    return true;
}

// ── AC3: bump_compaction_yield_check bumps the counter
bool test_bump_yield_check() {
    std::println("\n--- AC3: bump_compaction_yield_check ---");
    aura::ast::ArenaGroup g;
    CHECK(g.compaction_yield_checks_group() == 0, "starts at 0");
    g.bump_compaction_yield_check();
    g.bump_compaction_yield_check();
    CHECK(g.compaction_yield_checks_group() == 2,
          std::format("after 2 bumps == 2 (got {})", g.compaction_yield_checks_group()));
    return true;
}

// ── AC4: auto_compact_with_safety bumps both counters
bool test_with_safety_bumps_both() {
    std::println("\n--- AC4: auto_compact_with_safety bumps both ---");
    aura::ast::ArenaGroup g;
    // Pre-populate an arena so adaptive_compact_all has
    // something to consider.
    auto& arena = g.module_arena("test_module", 4096);
    (void)arena; // existence is enough
    auto before_guard = g.auto_compact_guard_call_count();
    auto before_yield = g.compaction_yield_checks_group();
    g.auto_compact_with_safety();
    CHECK(g.auto_compact_guard_call_count() == before_guard + 1,
          "guard-call counter advances by 1");
    CHECK(g.compaction_yield_checks_group() == before_yield + 1,
          "yield-check counter advances by 1");
    return true;
}

// ── AC5: EDSL: (query:arena-auto-stats) returns a hash
//         with 4 fields
bool test_edsl_stats_returns_hash() {
    std::println("\n--- AC5: EDSL (query:arena-auto-stats) ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(query:arena-auto-stats)");
    if (!r) {
        CHECK(false, "eval returned error");
        return true;
    }
    auto v = *r;
    CHECK(aura::compiler::types::is_hash(v), "(query:arena-auto-stats) returns a hash");
    for (auto key : {"auto-compact-guard-call-count", "compaction-yield-checks",
                     "auto-compact-trigger-count", "auto-compact-skip-count"}) {
        auto rr = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:arena-auto-stats\") '{}')", key));
        if (!rr) {
            CHECK(false, std::format("hash-ref for '{}' failed", key));
            continue;
        }
        CHECK(aura::compiler::types::is_int(*rr), std::format("hash-ref '{}' returns int", key));
    }
    return true;
}

// ── AC6: guard-call counter observable (post-mutate
//         increments via the guard dtor)
bool test_guard_dtor_bumps_counter() {
    std::println("\n--- AC6: guard dtor bumps counter ---");
    aura::compiler::CompilerService cs;
    // Get baseline
    auto before = cs.eval(
        "(hash-ref (engine:metrics \"query:arena-auto-stats\") 'auto-compact-guard-call-count)");
    if (!before || !aura::compiler::types::is_int(*before)) {
        CHECK(false, "could not read baseline guard-call count");
        return true;
    }
    auto before_n = aura::compiler::types::as_int(*before);

    // Trigger a mutate:rebind which goes through
    // MutationBoundaryGuard
    cs.eval(R"((mutate:rebind 'foo 42))");

    // Read again
    auto after = cs.eval(
        "(hash-ref (engine:metrics \"query:arena-auto-stats\") 'auto-compact-guard-call-count)");
    if (!after || !aura::compiler::types::is_int(*after)) {
        CHECK(false, "could not read post-mutate guard-call count");
        return true;
    }
    auto after_n = aura::compiler::types::as_int(*after);
    CHECK(after_n > before_n,
          std::format("guard-call count advances (before={}, after={})", before_n, after_n));
    return true;
}

// ── AC7: (stats:count) >= 51
bool test_stats_count() {
    std::println("\n--- AC7: (stats:count) >= 51 ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(stats:count)");
    if (!r || !aura::compiler::types::is_int(*r)) {
        CHECK(false, "stats:count not int");
        return true;
    }
    auto n = aura::compiler::types::as_int(*r);
    CHECK(n >= 51, std::format("stats:count >= 51 (got {})", n));
    return true;
}

// ── AC8: (stats:list) includes query:arena-auto-stats
bool test_stats_list_includes() {
    std::println("\n--- AC8: (stats:list) includes query:arena-auto-stats ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval(R"((if (member "query:arena-auto-stats" (stats:list)) #t #f))");
    if (!r) {
        CHECK(false, "eval failed");
        return true;
    }
    auto v = *r;
    CHECK(v.val != 0 && !aura::compiler::types::is_void(v),
          "query:arena-auto-stats is in (stats:list)");
    return true;
}

// ── AC9: fresh service: all 4 counters default to 0
bool test_fresh_service_defaults() {
    std::println("\n--- AC9: fresh service defaults ---");
    aura::compiler::CompilerService cs;
    auto guard = cs.eval(
        "(hash-ref (engine:metrics \"query:arena-auto-stats\") 'auto-compact-guard-call-count)");
    auto yield_ =
        cs.eval("(hash-ref (engine:metrics \"query:arena-auto-stats\") 'compaction-yield-checks)");
    auto trigger = cs.eval(
        "(hash-ref (engine:metrics \"query:arena-auto-stats\") 'auto-compact-trigger-count)");
    auto skip =
        cs.eval("(hash-ref (engine:metrics \"query:arena-auto-stats\") 'auto-compact-skip-count)");
    CHECK(aura::compiler::types::as_int(*guard) == 0, "guard-call == 0");
    CHECK(aura::compiler::types::as_int(*yield_) == 0, "yield-check == 0");
    CHECK(aura::compiler::types::as_int(*trigger) == 0, "trigger == 0");
    CHECK(aura::compiler::types::as_int(*skip) == 0, "skip == 0");
    return true;
}

// ── AC10: After many mutate calls, guard-call counter
//          advances (long-AI-session closed-loop signal)
bool test_long_session_signal() {
    std::println("\n--- AC10: long AI session signal ---");
    aura::compiler::CompilerService cs;
    auto before = cs.eval(
        "(hash-ref (engine:metrics \"query:arena-auto-stats\") 'auto-compact-guard-call-count)");
    auto before_n = aura::compiler::types::as_int(*before);
    for (int i = 0; i < 5; ++i) {
        cs.eval(std::format(R"((mutate:rebind 'foo_{} {}))", i, i * 10));
    }
    auto after = cs.eval(
        "(hash-ref (engine:metrics \"query:arena-auto-stats\") 'auto-compact-guard-call-count)");
    auto after_n = aura::compiler::types::as_int(*after);
    CHECK(
        after_n >= before_n + 5,
        std::format("5 mutates bump guard-call by >= 5 (before={}, after={})", before_n, after_n));
    return true;
}

} // namespace aura_issue_464_detail

int aura_issue_464_arena_auto_compaction_run() {
    using namespace aura_issue_464_detail;
    std::println("═══ Issue #464 Arena auto-compaction tests ═══");

    test_empty_group();
    test_bump_guard_call();
    test_bump_yield_check();
    test_with_safety_bumps_both();
    test_edsl_stats_returns_hash();
    test_guard_dtor_bumps_counter();
    test_stats_count();
    test_stats_list_includes();
    test_fresh_service_defaults();
    test_long_session_signal();

    std::println("\n──────────────────────────────────────");
    std::println("Total: {} passed, {} failed", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_464_arena_auto_compaction_run();
}
#endif
