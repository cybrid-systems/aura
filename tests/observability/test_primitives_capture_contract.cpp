// test_primitives_capture_contract.cpp — Issue #751:
// PRIM_ERROR / capture discipline enforcement + query:primitives-contract-stats
// (refines #728/#671/#615; non-duplicative with #671 consistency-stats axis).
//
//   - AC1: query:primitives-contract-stats reachable (schema 751)
//   - AC2: PRIM_ERROR path bumps prim-error-hits
//   - AC3: primitives:contract-probe records capture violations
//   - AC4: compliant probe does not add violations
//   - AC5: multi-round error + probe matrix monotonic
//   - AC6: query regression (consistency-stats, error-stats, registry-stats)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_751_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t loc_hash(CompilerService& cs, const std::string& key) {
    auto r =
        cs.eval("(hash-ref (engine:metrics \"query:primitives-contract-stats\") \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto viol = loc_hash(cs, "capture-violations");
    const auto hits = loc_hash(cs, "prim-error-hits");
    const auto pct = loc_hash(cs, "style-compliance-pct");
    if (viol < 0 || hits < 0 || pct < 0)
        return -1;
    return viol + hits + pct;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:primitives-contract-stats (schema 751) ---");
    auto h = cs.eval("(engine:metrics \"query:primitives-contract-stats\")");
    CHECK(h && is_hash(*h), "primitives-contract-stats returns hash");
    CHECK(loc_hash(cs, "schema") == 751, "schema == 751");
    CHECK(loc_hash(cs, "capture-violations") >= 0, "capture-violations present");
    CHECK(loc_hash(cs, "prim-error-hits") >= 0, "prim-error-hits present");
    CHECK(loc_hash(cs, "style-compliance-pct") >= 0, "style-compliance-pct present");
    CHECK(loc_hash(cs, "capture-contract-version") == 2, "capture-contract-version == 2");

    std::println("\n--- AC2: PRIM_ERROR bumps prim-error-hits ---");
    const auto hits0 = loc_hash(cs, "prim-error-hits");
    auto err = cs.eval("(regex-match? \"[\" \"test\")");
    CHECK(err && is_error(*err), "invalid regex returns error value");
    const auto hits1 = loc_hash(cs, "prim-error-hits");
    std::println("  prim-error-hits: {} -> {}", hits0, hits1);
    CHECK(hits1 > hits0, "prim-error-hits grew after PRIM_ERROR path");

    std::println("\n--- AC3: contract-probe records capture violation ---");
    const auto viol0 = loc_hash(cs, "capture-violations");
    auto bad = cs.eval("(primitives:contract-probe #f #f)");
    CHECK(bad && is_bool(*bad) && !as_bool(*bad), "contract-probe #f #f returns #f");
    const auto viol1 = loc_hash(cs, "capture-violations");
    std::println("  capture-violations: {} -> {}", viol0, viol1);
    CHECK(viol1 > viol0, "capture-violations grew after non-compliant probe");

    std::println("\n--- AC4: compliant probe passes without extra violation ---");
    const auto viol4a = loc_hash(cs, "capture-violations");
    auto good = cs.eval("(primitives:contract-probe #t #t)");
    CHECK(good && is_bool(*good) && as_bool(*good), "contract-probe #t #t returns #t");
    const auto viol4b = loc_hash(cs, "capture-violations");
    CHECK(viol4b == viol4a, "compliant probe does not add capture-violations");

    std::println("\n--- AC5: multi-round error + probe matrix ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(modulo 10 0)");
        (void)cs.eval("(primitives:contract-probe #f #t)");
        (void)cs.eval("(primitives:contract-probe #t #t)");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  contract stats sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "stats monotonic over error+probe matrix");

    std::println("\n--- AC6: query regression ---");
    auto consistency = cs.eval("(engine:metrics \"query:primitives-consistency-stats\")");
    auto errors = cs.eval("(engine:metrics \"query:primitives-error-stats\")");
    auto registry = cs.eval("(engine:metrics \"query:primitives-registry-stats\")");
    CHECK(consistency && is_hash(*consistency), "primitives-consistency-stats regression");
    CHECK(errors && is_hash(*errors), "primitives-error-stats regression");
    CHECK(registry && is_hash(*registry), "primitives-registry-stats regression");
}

} // namespace aura_issue_751_detail

int aura_issue_primitives_capture_contract_run() {
    aura::compiler::CompilerService cs;
    aura_issue_751_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_primitives_capture_contract_run();
}
#endif
