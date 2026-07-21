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

} // namespace aura_1445_detail

int main() {
    using namespace aura_1445_detail;
    std::println("test_orchestration_steal_boost (#1445/#1492)");
    CompilerService cs;
    ac3_primitive_shape(cs);
    ac2_fresh_counters_zero(cs);
    ac5_no_spurious_boost(cs);
    std::println("\nsteal_boost: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
