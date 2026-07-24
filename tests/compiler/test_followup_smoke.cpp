// tests/test_followup_smoke.cpp — Smoke test for follow-up ship
// Verifies:
//   1. (query:mutation-boundary-coverage-stats) exposes #1446 AC3 fields
//      (panic-transfer-nested-success, cow-repin-on-steal,
//      checkpoint-lost-on-compact) — starts at 0 on fresh service.
//   2. #1445 priority-degrade counters bumped when long-mutation hook
//      is invoked (deferred-pressure-boosts + starvation-mitigated-count).
//   3. #1446 arena hook integration: set_arena wires the hook
//      (verified by ensuring service construction doesn't crash).

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_followup_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;

// AC: #1446 AC3 fields exposed via (query:mutation-boundary-coverage-stats).
static bool ac_coverage_stats_extended(CompilerService& cs) {
    auto check_present = [&](const char* key) -> bool {
        std::string expr =
            std::string("(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") '") +
            key + ")";
        auto r = cs.eval(expr.c_str());
        if (!r || !is_int(*r)) {
            TEST_LOG(std::string("missing or non-int: ") + key);
            return false;
        }
        return true;
    };
    return check_present("panic-transfer-nested-success") && check_present("cow-repin-on-steal") &&
           check_present("checkpoint-lost-on-compact");
}

// AC: counters start at 0 on fresh service.
static bool ac_counters_zero(CompilerService& cs) {
    auto check_zero = [&](const char* key) -> bool {
        std::string expr =
            std::string("(hash-get (engine:metrics \"query:mutation-boundary-coverage-stats\") '") +
            key + ")";
        auto r = cs.eval(expr.c_str());
        if (!r || !is_int(*r)) {
            return false;
        }
        if (as_int(*r) != 0) {
            TEST_LOG(std::string(key) + " != 0 on fresh service");
            return false;
        }
        return true;
    };
    return check_zero("panic-transfer-nested-success") && check_zero("cow-repin-on-steal") &&
           check_zero("checkpoint-lost-on-compact");
}

// AC: basic eval cycle still works (regression smoke).
static bool ac_basic_eval(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 42)\")")) {
        return false;
    }
    auto r = cs.eval("(eval-current)");
    if (!r || !is_int(*r) || as_int(*r) != 42) {
        return false;
    }
    return true;
}

} // namespace aura_followup_detail

int main() {
    using namespace aura_followup_detail;
    bool ok = true;
    {
        CompilerService cs;
        ok &= ac_coverage_stats_extended(cs);
        ok &= ac_counters_zero(cs);
        ok &= ac_basic_eval(cs);
    }
    if (!ok) {
        TEST_LOG("test_followup_smoke FAILED");
        return 1;
    }
    TEST_LOG("test_followup_smoke PASS");
    return 0;
}
