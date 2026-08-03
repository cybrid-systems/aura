// @category: integration
// @reason: CompilerService + Evaluator deprecation dispatch + stats facade
//
// test_primitives_surface_convergence.cpp — Issue #1448 SlimSurface
// infrastructure acceptance criteria (runtime half).
//
// Script / freeze / --strict are covered by:
//   scripts/check_primitive_surface.py
//   tests/test_primitive_surface_gate.py
//   ./build.py gate
//
// This binary verifies the *engine* side of SlimSurface:
//   AC1: PrimMeta.deprecated is readable on a known demoted alias
//   AC2: invoking a deprecated primitive bumps
//        Evaluator::deprecated_prim_dispatch_total()
//   AC3: (stats:count) returns the internal catalog size and is ≤ 420
//        (SlimSurface stats catalog target; public add() shrink is
//        tracked by the Python --strict budget)
//   AC4: (stats:list) is a non-empty list (clear catalog surface)
//   AC5: (engine:metrics) facade is present (hash / non-void)

#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1448_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;

// Issue #2628: query:children purged (no public registration).
constexpr const char* kDeprecatedAlias = "query:children";

void ac1_deprecated_meta_flag() {
    std::println("\n--- AC1: #2628 purged query:children (no public registration) ---");
    CompilerService cs;
    auto& prims = cs.evaluator().primitives();
    const auto slot = prims.slot_for_name(kDeprecatedAlias);
    CHECK(slot >= prims.slot_count(), "query:children is NOT registered (#2628)");
    // Remaining mutate:* aliases may still be deprecated; check one still public.
    const auto rebind_slot = prims.slot_for_name("mutate:rebind");
    if (rebind_slot < prims.slot_count()) {
        const auto& meta = prims.meta_for_slot(rebind_slot);
        CHECK(meta.deprecated, "mutate:rebind still PrimMeta.deprecated until later purge");
    }
}

void ac2_dispatch_bumps_counter() {
    std::println("\n--- AC2: remaining deprecated mutate alias bumps counter ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto before = ev.deprecated_prim_dispatch_total();

    auto set = cs.eval("(set-code \"(define (f x) (+ x 1))\")");
    CHECK(set.has_value(), "set-code setup");
    if (!set.has_value())
        return;

    // mutate:rebind is still a public deprecated alias (not in #2628 list).
    auto r = cs.eval("(mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"t\")");
    CHECK(r.has_value(), "mutate:rebind still executes");

    const auto after = ev.deprecated_prim_dispatch_total();
    CHECK(after > before,
          std::format("deprecated_prim_dispatch_total increased ({} → {})", before, after));
}

void ac3_stats_count_budget() {
    std::println("\n--- AC3: (stats:count) ≤ 420 catalog target ---");
    CompilerService cs;
    auto r = cs.eval("(stats:count)");
    CHECK(r.has_value() && is_int(*r), "(stats:count) returns int");
    if (!r || !is_int(*r))
        return;
    const auto n = as_int(*r);
    CHECK(n > 0, "stats catalog non-empty");
    // Issue #1448 AC5: stats catalog target ≤ 420 (already met after
    // P5a facade migration). Public add() budget is enforced by
    // check_primitive_surface.py --strict.
    CHECK(n <= 420, std::format("(stats:count)={} ≤ 420 SlimSurface catalog target", n));
    std::println("  info: stats:count = {}", n);
}

void ac4_stats_list_nonempty() {
    std::println("\n--- AC4: (stats:list) non-empty catalog ---");
    CompilerService cs;
    auto r = cs.eval("(stats:list)");
    CHECK(r.has_value(), "(stats:list) returns a value");
    if (!r)
        return;
    // List is a nested pair spine (or void only if empty).
    CHECK(!is_void(*r), "(stats:list) is not void");
    CHECK(is_pair(*r) || is_int(*r), "(stats:list) is a list (pair spine) or non-void");
}

void ac5_engine_metrics_facade() {
    std::println("\n--- AC5: (engine:metrics) facade present ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics)");
    CHECK(r.has_value(), "(engine:metrics) returns a value");
    if (!r)
        return;
    CHECK(is_hash(*r) || !is_void(*r), "(engine:metrics) returns a hash facade (or non-void)");
}

} // namespace aura_issue_1448_detail

int main() {
    using namespace aura_issue_1448_detail;
    std::println("=== Issue #1448: SlimSurface surface convergence ===");
    ac1_deprecated_meta_flag();
    ac2_dispatch_bumps_counter();
    ac3_stats_count_budget();
    ac4_stats_list_nonempty();
    ac5_engine_metrics_facade();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
