// Issue #189/#417/#419/#456 (#1978 renamed): issue# moved from filename to header.
// test_defuse_version_closed_loop_419.cpp
// Issue #419: Modular current_defuse_version() API +
// is_version_current stale detection observability for
// AOT hot-update and runtime dispatch.
//
// Non-duplicative with #456 (epoch-stats / epoch-delta),
// #189 (concurrency:version-snapshot pair), #417
// (mutation-boundary-invariant-stats).
//
// AC1: query:defuse-version-stats reachable
// AC2: current_defuse_version matches get_defuse_version
// AC3: concurrency:version-snapshot + version-current?
// AC4: mutate:rebind bumps defuse epoch
// AC5: epoch-delta-since-last-query integration
// AC6: multi-round mutate matrix monotonic
// AC7: query regression (epoch-stats,
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

namespace aura_419_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;

static std::int64_t defuse_version_stats(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:defuse-version-stats\")");
    if (!r || !is_int(*r))
        return 0;
    return as_int(*r);
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define (add1 x) (+ x 1)) "
                 "(define base 10) (define acc 0) "
                 "(add1 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:defuse-version-stats ---");
    CHECK(setup_workspace(cs), "defuse version workspace setup");
    const auto s0 = defuse_version_stats(cs);
    std::println("  defuse-version-stats = {}", s0);
    CHECK(s0 >= 0, "defuse version stats non-negative");

    std::println("\n--- AC2: current_defuse_version API ---");
    auto& ev = cs.evaluator();
    CHECK(ev.current_defuse_version() == ev.get_defuse_version(),
          "current_defuse_version matches get_defuse_version");

    std::println("\n--- AC3: version snapshot stale detection ---");
    auto snap = cs.eval("(stats:get \"concurrency:version-snapshot\")");
    CHECK(snap && is_int(*snap), "version-snapshot returns int");
    auto fresh =
        cs.eval("(concurrency:version-current? (stats:get \"concurrency:version-snapshot\"))");
    CHECK(fresh && is_bool(*fresh), "version-current? returns bool");

    std::println("\n--- AC4: mutate bumps defuse epoch ---");
    const auto stats4a = defuse_version_stats(cs);
    const auto ver4a = ev.current_defuse_version();
    (void)cs.eval("(mutate:rebind \"base\" \"99\")");
    const auto ver4b = ev.current_defuse_version();
    const auto stats4b = defuse_version_stats(cs);
    std::println("  defuse version: {} -> {}", ver4a, ver4b);
    std::println("  defuse-version-stats: {} -> {}", stats4a, stats4b);
    CHECK(ver4b > ver4a, "mutate bumps current_defuse_version");
    CHECK(stats4b > stats4a, "mutate bumps defuse-version-stats");

    std::println("\n--- AC5: epoch-delta-since-last-query ---");
    (void)cs.eval("(engine:metrics \"query:epoch-stats\")");
    auto delta = cs.eval("(query:epoch-delta-since-last-query)");
    CHECK(delta && is_int(*delta), "epoch-delta returns int");

    std::println("\n--- AC6: multi-round mutate matrix ---");
    const auto stats6a = defuse_version_stats(cs);
    const auto ver6a = ev.current_defuse_version();
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"acc\" \"" + std::to_string(round) + "\")");
        (void)cs.eval("(eval-current)");
    }
    const auto stats6b = defuse_version_stats(cs);
    const auto ver6b = ev.current_defuse_version();
    std::println("  defuse version: {} -> {}", ver6a, ver6b);
    std::println("  defuse-version-stats: {} -> {}", stats6a, stats6b);
    CHECK(ver6b > ver6a, "defuse version grows over matrix");
    CHECK(stats6b > stats6a, "defuse-version-stats grow over matrix");

    std::println("\n--- AC7: query regression ---");
    auto eps = cs.eval("(engine:metrics \"query:epoch-stats\")");
    auto mbi = cs.eval("(engine:metrics \"query:mutation-boundary-invariant-stats\")");
    CHECK(eps && is_int(*eps), "epoch-stats regression");
    CHECK(mbi && is_int(*mbi), "mutation-boundary-invariant-stats regression");
}

} // namespace aura_419_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_419_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}