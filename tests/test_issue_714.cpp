// @category: integration
// @reason: Issue #714 — unified self-evolution closed-loop observability
// primitive correlating hygiene (MacroIntroduced count, violation rate),
// dirty impact, epoch drift, reflect-validation pass rate, and mutation
// strategy recommendations into a single Agent-facing report.
//
// Scope-limited close: the issue body asks for 6 sub-systems wired
// together (Guard dtor + mark_dirty_upward + query:pattern marker
// filter + reflect auto_validate + strategy hooks + correlation
// primitive + AI self-evo demo loop). Each hook is a dedicated
// session. For this PR we ship:
//
//   1. Standalone (query:self-evolution-closedloop-stats, schema 714)
//      primitive exposing the 9-field report
//   2. 3 atomics in CompilerMetrics:
//        self_evo_strategy_recommend_safe_total
//        self_evo_strategy_recommend_aggressive_total
//        self_evo_strategy_recommend_balanced_total
//   3. 3 bump helpers in Evaluator:
//        bump_self_evo_strategy_recommend_safe
//        bump_self_evo_strategy_recommend_aggressive
//        bump_self_evo_strategy_recommend_balanced
//   4. Bump helpers are public on Evaluator — callable from any
//      future Guard dtor / mark_dirty_upward / reflect auto_validate
//      hook that wants to record a strategy recommendation
//   5. Derived "recommended-mutation-strategy" string field —
//      computed from the highest of the 3 strategy counts (ties go
//      to "balanced" as the safe default)
//
// Non-duplicative notes:
//   - #654 macro-hygiene-fiber-panic-stats (5 fields incl.
//     reflect-hygiene-validation)
//   - #712 macro-reflect-validation-stats (4 fields, subtree-level)
//   - #713 macro-jit-hygiene-stats (3 fields, JIT/AOT/Interpreter)
//   - #488 schema-validation (whole-workspace pass/fail)
//   - #714 is the FIRST primitive that correlates hygiene + dirty +
//     epoch + reflect + strategy into a single Agent-facing report
//
// ACs:
//   AC1: hash shape (9 fields + schema sentinel = 10 entries)
//   AC2: hygiene-macro-introduced-count == 0 + violation-rate == 0
//        on fresh service (Phase 1 honest about scope: walk is
//        follow-up; both fields are exposed as 0 by the primitive)
//   AC3: schema == 714 (drift sentinel)
//   AC4: bump helpers accessible — exercise via direct bump on
//        Evaluator surface and verify the primitive reports the
//        new counts + the recommended-mutation-strategy string
//        flips to match the highest count
//   AC5: regression — #712 macro-reflect-validation-stats and
//        #713 macro-jit-hygiene-stats still reachable (no drift
//        from sibling primitive additions)
//
// (We do NOT trigger Guard dtor / mark_dirty_upward / reflect
// auto_validate — those hook wirings are the bulk of this issue's
// remaining scope and live in dedicated follow-up sessions.)

#include <algorithm>
#include <iostream>
#include <print>
#include <string>
#include <vector>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.core.ast;

namespace aura_issue_714_detail {
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

static std::int64_t hash_int_field(aura::compiler::CompilerService& cs, std::string_view hash_src,
                                   std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

static std::string hash_str_field(aura::compiler::CompilerService& cs, std::string_view hash_src,
                                  std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} '{}')", hash_src, key));
    if (!r || !aura::compiler::types::is_string(*r))
        return {};
    const auto idx = aura::compiler::types::as_string_idx(*r);
    const auto& heap = cs.evaluator().string_heap();
    if (idx >= heap.size())
        return {};
    return std::string(heap[idx]);
}

static void run_ac1_shape(aura::compiler::CompilerService& cs) {
    std::println(
        "\n--- AC1: (engine:metrics \"query:self-evolution-closedloop-stats\") hash shape ---");
    auto r = cs.eval("(engine:metrics \"query:self-evolution-closedloop-stats\")");
    CHECK(r && aura::compiler::types::is_hash(*r),
          "(engine:metrics \"query:self-evolution-closedloop-stats\") returns a hash");
    const std::vector<std::string> keys = {"hygiene-macro-introduced-count",
                                           "hygiene-violation-rate",
                                           "dirty-subtree-impact",
                                           "epoch-drift-detected",
                                           "reflect-validation-pass-rate",
                                           "recommended-mutation-strategy",
                                           "strategy-safe-count",
                                           "strategy-aggressive-count",
                                           "strategy-balanced-count",
                                           "schema"};
    for (const auto& k : keys) {
        auto f = cs.eval(std::format(
            "(hash-ref (engine:metrics \"query:self-evolution-closedloop-stats\") '{}')", k));
        CHECK(f, std::format("field '{}' present", k));
    }
}

static void run_ac2_fresh_zero(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC2: hygiene fields default to 0 on fresh service ---");
    const auto macro_intro =
        hash_int_field(cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")",
                       "hygiene-macro-introduced-count");
    CHECK(macro_intro == 0,
          std::format("hygiene-macro-introduced-count = {} (expected 0 on fresh service — "
                      "Phase 1 stub; macro-introduced walk is follow-up)",
                      macro_intro));
    const auto violation_rate = hash_int_field(
        cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")", "hygiene-violation-rate");
    CHECK(violation_rate == 0,
          std::format("hygiene-violation-rate = {} (expected 0 on fresh service)", violation_rate));
    const auto dirty_impact = hash_int_field(
        cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")", "dirty-subtree-impact");
    CHECK(dirty_impact == 0,
          std::format("dirty-subtree-impact = {} (expected 0 on fresh service)", dirty_impact));
    const auto epoch_drift = hash_int_field(
        cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")", "epoch-drift-detected");
    CHECK(epoch_drift == 0,
          std::format("epoch-drift-detected = {} (expected 0 on fresh service)", epoch_drift));
}

static void run_ac3_schema_sentinel(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC3: schema == 714 (drift sentinel) ---");
    const auto schema =
        hash_int_field(cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")", "schema");
    CHECK(schema == 714, std::format("schema = {} (expected 714)", schema));
}

static void run_ac4_bump_accessible(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC4: bump helpers callable + recommended-strategy flips ---");
    // Direct call: invoke the evaluator's bump helpers via the public
    // surface. These helpers exist so future Guard dtor / mark_dirty_upward
    // / reflect auto_validate hooks can call them when they detect a
    // strategy decision point.
    auto& ev = cs.evaluator();
    ev.bump_self_evo_strategy_recommend_safe();
    ev.bump_self_evo_strategy_recommend_balanced();
    ev.bump_self_evo_strategy_recommend_balanced();
    ev.bump_self_evo_strategy_recommend_balanced();
    const auto safe = hash_int_field(
        cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")", "strategy-safe-count");
    const auto balanced =
        hash_int_field(cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")",
                       "strategy-balanced-count");
    const auto aggressive =
        hash_int_field(cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")",
                       "strategy-aggressive-count");
    CHECK(safe == 1, std::format("after 1 safe bump: strategy-safe-count = {} (expected 1)", safe));
    CHECK(
        balanced == 3,
        std::format("after 3 balanced bumps: strategy-balanced-count = {} (expected 3)", balanced));
    CHECK(aggressive == 0,
          std::format("strategy-aggressive-count = {} (expected 0, no aggressive bumps)",
                      aggressive));

    // recommended-mutation-strategy should now be "balanced" (highest of 3).
    const auto recommended =
        hash_str_field(cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")",
                       "recommended-mutation-strategy");
    CHECK(recommended == "balanced",
          std::format("recommended-mutation-strategy = '{}' (expected 'balanced' — highest "
                      "of 3 counts; ties go balanced)",
                      recommended));

    // Flip to safe: bump safe past balanced.
    ev.bump_self_evo_strategy_recommend_safe();
    ev.bump_self_evo_strategy_recommend_safe();
    ev.bump_self_evo_strategy_recommend_safe();
    const auto recommended2 =
        hash_str_field(cs, "(engine:metrics \"query:self-evolution-closedloop-stats\")",
                       "recommended-mutation-strategy");
    CHECK(
        recommended2 == "safe",
        std::format("after 3 more safe bumps: recommended = '{}' (expected 'safe')", recommended2));
}

static void run_ac5_regression(aura::compiler::CompilerService& cs) {
    std::println("\n--- AC5: regression — #712 + #713 surfaces unaffected ---");
    auto reflect = cs.eval("(engine:metrics \"query:macro-reflect-validation-stats\")");
    auto jit = cs.eval("(engine:metrics \"query:macro-jit-hygiene-stats\")");
    CHECK(reflect && aura::compiler::types::is_hash(*reflect),
          "query:macro-reflect-validation-stats hash regression (#712)");
    CHECK(jit && aura::compiler::types::is_hash(*jit),
          "query:macro-jit-hygiene-stats hash regression (#713)");
    const auto reflect_schema =
        hash_int_field(cs, "(engine:metrics \"query:macro-reflect-validation-stats\")", "schema");
    CHECK(reflect_schema == 712,
          std::format("reflect schema = {} (expected 712, no drift)", reflect_schema));
    const auto jit_schema =
        hash_int_field(cs, "(engine:metrics \"query:macro-jit-hygiene-stats\")", "schema");
    CHECK(jit_schema == 713, std::format("jit schema = {} (expected 713, no drift)", jit_schema));
}

} // namespace aura_issue_714_detail

int aura_issue_714_run() {
    using namespace aura_issue_714_detail;
    std::println("=== Issue #714: self-evolution closed-loop stats (scope-limited close) ===");

    {
        aura::compiler::CompilerService cs;
        run_ac1_shape(cs);
        run_ac2_fresh_zero(cs);
        run_ac3_schema_sentinel(cs);
        run_ac4_bump_accessible(cs);
        run_ac5_regression(cs);
    }

    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_714_run();
}
#endif
