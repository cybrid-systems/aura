// test_hygiene_violation_closed_loop_422.cpp
// Issue #422: Hygiene violation metrics + automatic
// detection in MutationBoundaryGuard mutate paths.
//
// Non-duplicative with #458 (hygiene-stats skip-only),
// #547 (pattern-hygiene-stats), #420/#421 macro bundles.
//
// AC1: query:hygiene-violation-stats reachable
// AC2: syntax:set-marker establishes protected target
// AC3: mutate:rebind on macro-introduced bumps attempts
// AC4: compile:snapshot exposes hygiene-violation-attempts
// AC5: ensure_hygiene_violation_detection hook
// AC6: multi-round blocked attempts monotonic
// AC7: query regression (pattern-hygiene-stats,
//      mutation-boundary-invariant-stats)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_422_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;

static std::int64_t hygiene_violation_stats(CompilerService& cs) {
    auto r = cs.eval("(query:hygiene-violation-stats)");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool stamp_macro_introduced_define(CompilerService& cs, std::int64_t& nid_out) {
    if (!cs.eval("(set-code \"(define myvar 42)\")"))
        return false;
    auto find_r = cs.eval("(car (query:find \"myvar\"))");
    if (!find_r || !is_int(*find_r))
        return false;
    nid_out = as_int(*find_r);
    auto set_r = cs.eval("(syntax:set-marker " + std::to_string(nid_out) + " 1)");
    if (!set_r || !is_bool(*set_r) || !as_bool(*set_r))
        return false;
    auto prot = cs.eval("(hygiene:protected? " + std::to_string(nid_out) + ")");
    return prot && is_bool(*prot) && as_bool(*prot);
}

static bool is_hygiene_protected_error(CompilerService& cs) {
    auto r = cs.eval("(let ((r (mutate:rebind \"myvar\" \"99\"))) "
                     "(if (and (pair? r) (string? (car r)) "
                     "         (string=? (car r) \"hygiene-protected\")) 1 0))");
    return r && is_int(*r) && as_int(*r) == 1;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:hygiene-violation-stats ---");
    const auto s0 = hygiene_violation_stats(cs);
    std::println("  hygiene-violation-stats = {}", s0);
    CHECK(s0 >= 0, "hygiene violation stats non-negative");

    std::println("\n--- AC2: stamp MacroIntroduced define ---");
    std::int64_t nid = 0;
    CHECK(stamp_macro_introduced_define(cs, nid), "syntax:set-marker stamps protected define");

    std::println("\n--- AC3: mutate:rebind bumps violation attempts ---");
    auto& ev = cs.evaluator();
    const auto stats3a = hygiene_violation_stats(cs);
    const auto attempts3a = ev.get_hygiene_violation_attempts();
    CHECK(is_hygiene_protected_error(cs), "mutate:rebind returns hygiene-protected error");
    const auto stats3b = hygiene_violation_stats(cs);
    const auto attempts3b = ev.get_hygiene_violation_attempts();
    std::println("  violation stats: {} -> {}", stats3a, stats3b);
    std::println("  attempts: {} -> {}", attempts3a, attempts3b);
    CHECK(attempts3b > attempts3a, "blocked mutate bumps attempts");
    CHECK(stats3b > stats3a, "hygiene-violation-stats grow");

    std::println("\n--- AC4: compile:snapshot hygiene key ---");
    auto snap = cs.eval("(hash-ref (compile:snapshot) \"hygiene-violation-attempts\")");
    CHECK(snap && is_int(*snap), "snapshot hygiene-violation-attempts");
    std::println("  snapshot attempts = {}", as_int(*snap));
    CHECK(as_int(*snap) >= attempts3b, "snapshot attempts >= live counter");

    std::println("\n--- AC5: ensure_hygiene_violation_detection ---");
    ev.ensure_hygiene_violation_detection();
    CHECK(ev.get_hygiene_violation_attempts() > 0, "attempts recorded after blocked mutate");

    std::println("\n--- AC6: multi-round blocked attempts ---");
    const auto stats6a = hygiene_violation_stats(cs);
    const auto attempts6a = ev.get_hygiene_violation_attempts();
    for (int round = 0; round < 3; ++round) {
        (void)is_hygiene_protected_error(cs);
    }
    const auto stats6b = hygiene_violation_stats(cs);
    const auto attempts6b = ev.get_hygiene_violation_attempts();
    std::println("  attempts: {} -> {}", attempts6a, attempts6b);
    CHECK(attempts6b > attempts6a, "attempts grow over blocked matrix");
    CHECK(stats6b > stats6a, "violation stats grow over matrix");

    std::println("\n--- AC7: query regression ---");
    auto phs = cs.eval("(engine:metrics \"query:pattern-hygiene-stats\")");
    auto mbi = cs.eval("(query:mutation-boundary-invariant-stats)");
    CHECK(phs && is_int(*phs), "pattern-hygiene-stats regression");
    CHECK(mbi && is_int(*mbi), "mutation-boundary-invariant-stats regression");
}

} // namespace aura_422_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_422_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}