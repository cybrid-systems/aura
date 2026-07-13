// @category: integration
// @reason: Issue #671 — primitives_detail lambda capture discipline
//  + style compliance observability (P1 stdlib-impl consistency).
//
//  Scope shipped:
//   - (query:primitives-consistency-stats, schema 671) hash
//     primitive with 7 fields:
//       - capture-violations-detected (primitive_capture_violations_total)
//       - style-compliance-pct (derived: 1 - viol/slots)
//       - registry-slots (primitives_.slot_count())
//       - documented-count (primitives_.documented_meta_count())
//       - capture-contract-version (kPrimCaptureContractVersion)
//       - recommended-action (0/1/2: no action / backfill / audit)
//       - schema=671
//   - PRIM_CAPTURE_HAS_ERROR_COUNTER + PRIM_CAPTURE_USES_GUARD
//     compile-time static_assert macros in primitives_detail.h.
//   - docs/design/primitives-style.md — canonical writeup of
//     the PRIM_CAPTURE_CONTRACT discipline + audit checklist.
//
//  Non-duplicative with #709 (query:primitives-registry-stats,
//  7-field registry-level summary), #615 (PRIM_ERROR macro shape),
//  #643 (DEFINE_PRIMITIVE_META macro), #617 (catalog summary).
//
//   - AC1:  query:primitives-consistency-stats reachable (schema 671)
//   - AC2:  7 fields present in the hash response
//   - AC3:  capture-violations-detected is int (>=0 on fresh CS)
//   - AC4:  style-compliance-pct is 100 on a fresh CS (no violations)
//   - AC5:  capture-contract-version is the value of
//           kPrimCaptureContractVersion (currently 1)
//   - AC6:  recommended-action is 0 on fresh CS (no violations,
//           documented == slots by default? — depends on
//           backfill state)
//   - AC7:  regression — (query:primitives-registry-stats) (#709)
//           still reachable with its existing fields

#include <iostream>
#include <print>
#include <string>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_671_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::int64_t hash_int(aura::compiler::CompilerService& cs, const std::string& prim,
                             const std::string& key) {
    auto r = cs.eval(std::format("(hash-ref ({}) '{}')", prim, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static void run_ac1_reachable(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC1: query:primitives-consistency-stats reachable (schema 671) ---");
    auto r = cs.eval("(query:primitives-consistency-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "query:primitives-consistency-stats returns a hash");
    auto schema = hash_int(cs, "query:primitives-consistency-stats", "schema");
    CHECK(schema == 671, "schema field == 671 (drift sentinel)");
}

static void run_ac2_seven_fields(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: 7 fields present in the hash response ---");
    const std::vector<std::string> keys = {
        "capture-violations-detected", "style-compliance-pct", "registry-slots", "documented-count",
        "capture-contract-version",    "recommended-action",   "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:primitives-consistency-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac3_violations_int(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: capture-violations-detected is int >= 0 on fresh CS ---");
    auto v = hash_int(cs, "query:primitives-consistency-stats", "capture-violations-detected");
    CHECK(v >= 0, std::format("capture-violations-detected is int >= 0 (got {})", v));
}

static void run_ac4_compliance_pct(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: style-compliance-pct is 100 on a fresh CS ---");
    auto pct = hash_int(cs, "query:primitives-consistency-stats", "style-compliance-pct");
    CHECK(pct == 100,
          std::format("style-compliance-pct == 100 on fresh CS (no violations; got {})", pct));
}

static void run_ac5_contract_version(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: capture-contract-version reflects the header constant ---");
    auto v = hash_int(cs, "query:primitives-consistency-stats", "capture-contract-version");
    // #751 bumped kPrimCaptureContractVersion to 2 (enforcement wiring).
    CHECK(v == 2, std::format("capture-contract-version == 2 (got {})", v));
}

static void run_ac6_recommended_action(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC6: recommended-action is 0 on a fresh CS ---");
    auto action = hash_int(cs, "query:primitives-consistency-stats", "recommended-action");
    // 0 = no action. The fresh CS has capture_violations = 0 (AC3
    // passes) AND documented_count depends on the seed registry
    // (in #669 follow-up it's still partial — most primitives are
    // default empty). So action could be 1 (backfill meta) on a
    // realistic registry. Accept either 0 or 1 here.
    CHECK(action == 0 || action == 1,
          std::format("recommended-action in [0,1] (got {}) — backfill or no-op", action));
}

static void run_ac7_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC7: regression — registry-stats (#709) still reachable ---");
    auto r = cs.eval("(query:primitives-registry-stats)");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "query:primitives-registry-stats (#709) returns a hash");
    // Verify the existing capture-violations field still surfaces
    // (#709 shipped it as one of 7 fields; #671 carves out a
    // dedicated primitive so the regression is intentional).
    auto viol = hash_int(cs, "query:primitives-registry-stats", "capture-violations");
    CHECK(viol >= 0,
          std::format("query:primitives-registry-stats capture-violations >= 0 ({})", viol));
}

} // namespace aura_issue_671_detail

int aura_issue_671_run() {
    using namespace aura_issue_671_detail;

    {
        aura::compiler::CompilerService cs;
        run_ac1_reachable(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac2_seven_fields(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac3_violations_int(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac4_compliance_pct(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac5_contract_version(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac6_recommended_action(cs);
    }
    {
        aura::compiler::CompilerService cs;
        run_ac7_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_671_run();
}
#endif
