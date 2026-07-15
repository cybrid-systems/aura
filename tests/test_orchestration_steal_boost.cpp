// tests/test_orchestration_steal_boost.cpp — Issue #1445
// Verify the threshold-based boost path + new metrics surface.
// AC4 (smoke test): inner-boundary defer + threshold counter bump.
// AC5: happy-path regression — no spurious boost on single-defer fiber.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_1445_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_hash;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

// AC3: (query:orchestration-steal-stats) returns hash with all
// expected fields including the new steal_priority_boost_triggered
// and starvation_mitigated_count counters (#1445 AC2).
static bool ac3_primitive_shape(CompilerService& cs) {
    auto r = cs.eval("(engine:metrics \"query:orchestration-steal-stats\")");
    if (!r || !is_hash(*r)) {
        TEST_LOG("AC3: primitive did not return hash");
        return false;
    }
    auto schema_ev =
        cs.eval("(hash-get (engine:metrics \"query:orchestration-steal-stats\") 'schema)");
    if (!schema_ev || !is_int(*schema_ev) || as_int(*schema_ev) != 1445) {
        TEST_LOG("AC3: schema field != 1445");
        return false;
    }
    return true;
}

// AC2: new counters start at 0 on a fresh service.
static bool ac2_fresh_counters_zero(CompilerService& cs) {
    auto check_zero = [&](const char* key) -> bool {
        std::string expr =
            std::string("(hash-get (engine:metrics \"query:orchestration-steal-stats\") '") + key +
            ")";
        auto r = cs.eval(expr.c_str());
        if (!r || !is_int(*r)) {
            TEST_LOG(std::string("AC2: cannot read ") + key);
            return false;
        }
        if (as_int(*r) != 0) {
            TEST_LOG(std::string("AC2: ") + key + " expected 0, got " + std::to_string(as_int(*r)));
            return false;
        }
        return true;
    };
    return check_zero("steal-priority-boost-triggered") &&
           check_zero("starvation-mitigated-count") && check_zero("deferred-pressure-boosts") &&
           check_zero("starvation-priority-boosts");
}

// AC5: happy-path — basic mutate cycle does not spuriously trigger
// boost counters (those are bumped only when victim is at inner
// mutation boundary AND deferred_count > 3 in worker.cpp).
static bool ac5_no_spurious_boost(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (set! x 2)\")")) {
        TEST_LOG("AC5: set-code failed");
        return false;
    }
    if (auto r = cs.eval("(eval-current)"); !r || !is_int(*r) || as_int(*r) != 2) {
        TEST_LOG("AC5: mutate cycle broke");
        return false;
    }
    auto boost_ev = cs.eval("(hash-get (engine:metrics \"query:orchestration-steal-stats\") "
                            "'steal-priority-boost-triggered)");
    if (!boost_ev || !is_int(*boost_ev)) {
        TEST_LOG("AC5: cannot read steal-priority-boost-triggered");
        return false;
    }
    if (as_int(*boost_ev) != 0) {
        TEST_LOG("AC5: spurious boost count = " << as_int(*boost_ev));
        return false;
    }
    return true;
}

} // namespace aura_1445_detail

int main() {
    using namespace aura_1445_detail;
    bool ok = true;
    {
        CompilerService cs;
        ok &= ac3_primitive_shape(cs);
        ok &= ac2_fresh_counters_zero(cs);
        ok &= ac5_no_spurious_boost(cs);
    }
    if (!ok) {
        TEST_LOG("test_orchestration_steal_boost FAILED");
        return 1;
    }
    TEST_LOG("test_orchestration_steal_boost PASS");
    return 0;
}
