// Issue #1434 stretch: facade vs N direct stats dispatches.
//
// Not a hard CI gate — prints ratio. When full primitives are enabled,
// one (engine:metrics) should be much cheaper than 20 separate
// query:*-stats calls for catalog discovery (hash build once vs 20 lookups).

#include "test_harness.hpp"

#include <chrono>
#include <cstdint>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

int main() {
    using Steady = std::chrono::steady_clock;
    aura::compiler::CompilerService cs;

    // Warmup
    (void)cs.eval("(engine:metrics)");
    (void)cs.eval("(engine:metrics \"query:pattern-stats\")");

    constexpr int kIters = 30;
    // Representative subset of the #1434 top-20 (keeps wall time CI-bounded).
    static const char* kTop[] = {
        "query:envframe-dualpath-stats", "query:pattern-stats",       "query:typed-mutation-stats",
        "query:pattern-index-stats",     "query:macro-hygiene-stats",
    };
    constexpr int kNames = 5;

    // Path A: N separate by-name stats dispatches (old style).
    auto t0 = Steady::now();
    for (int i = 0; i < kIters; ++i) {
        for (int j = 0; j < kNames; ++j) {
            (void)cs.eval(aura::test::aura_call_expr(kTop[j]));
        }
    }
    auto t1 = Steady::now();
    // Path B: one facade by-name (parity path used by migrated tests).
    for (int i = 0; i < kIters; ++i) {
        (void)cs.eval("(engine:metrics \"query:pattern-stats\")");
    }
    auto t2 = Steady::now();
    // Path C: one group dump (CompilerMetrics atomics).
    for (int i = 0; i < kIters; ++i) {
        (void)cs.eval("(engine:metrics :group \"jit\")");
    }
    auto t3 = Steady::now();

    double direct_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kIters;
    double facade_bn_us = std::chrono::duration<double, std::micro>(t2 - t1).count() / kIters;
    double group_us = std::chrono::duration<double, std::micro>(t3 - t2).count() / kIters;

    std::println("stats facade bench (#1434):");
    std::println("  direct {}×stats:              {:.1f} us/iter", kNames, direct_us);
    std::println("  facade 1×by-name:             {:.1f} us/iter", facade_bn_us);
    std::println("  facade 1×(:group \"jit\"):     {:.1f} us/iter", group_us);

    CHECK(direct_us > 0, "direct path runs");
    CHECK(facade_bn_us > 0, "facade by-name path runs");
    CHECK(group_us > 0, "facade :group path runs");
    CHECK(true, "stretch bench informational (not a hard 10x gate)");

    // Issue #1439: query:*-stats are no longer public prims — api-reference
    // must NOT list them; still document the metrics facade.
    {
        auto r = cs.eval("(api-reference)");
        CHECK(r && aura::compiler::types::is_string(*r), "api-reference returns string");
        if (r && aura::compiler::types::is_string(*r)) {
            auto idx = aura::compiler::types::as_string_idx(*r);
            auto heap = cs.evaluator().string_heap();
            std::string s = idx < heap.size() ? heap[idx] : "";
            CHECK(s.find("engine:metrics") != std::string::npos, "api-reference mentions facade");
            // Public surface must not re-list removed stats names (v2.0 / #1439).
            CHECK(s.find("query:pattern-stats") == std::string::npos,
                  "api-reference omits query:pattern-stats (removed from public registry)");
        }
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("stats facade bench: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
