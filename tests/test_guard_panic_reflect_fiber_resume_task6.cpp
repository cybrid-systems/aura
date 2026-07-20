// test_guard_panic_reflect_fiber_resume_task6.cpp — Issue #596:
// MutationBoundaryGuard + panic checkpoint + reflect/schema validation
// + fiber resume safety closed loop (Task6 production review).
//
// Non-duplicative with #592 (panic checkpoint fiber resume matrix),
// #594 (reflection-selfmod-stats), #595 (self-evolution-loop-stats),
// #548 (panic-checkpoint-lifecycle-stats), #588 (per-fiber stack sync).
//
//   - AC1:  query:guard-panic-reflect-stats reachable (schema 596)
//   - AC2:  Guard mutate bumps validate hook + commit counters
//   - AC3:  schema_validation pass/fail counters observable
//   - AC4:  panic-checkpoint-fiber-stats regression (resume transport)
//   - AC5:  boundary-violation-prevented counter observable
//   - AC6:  multi-round Guard+mutate — stats monotonic
//   - AC7:  query regression (reflection-selfmod, self-evo loop, lifecycle)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_596_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r =
        cs.eval("(hash-ref (engine:metrics \"query:guard-panic-reflect-stats\") \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto commits = hash_int(cs, "checkpoints-committed");
    const auto restores = hash_int(cs, "restores-on-resume");
    const auto pass = hash_int(cs, "validation-pass");
    const auto fail = hash_int(cs, "validation-fail");
    const auto prevented = hash_int(cs, "boundary-violation-prevented");
    if (commits < 0 || restores < 0 || pass < 0 || fail < 0 || prevented < 0)
        return -1;
    return commits + restores + pass + fail + prevented;
}

static bool setup_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \"(define x 1) (define y 2) (let ((z 3)) (+ x y z))\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:guard-panic-reflect-stats (schema 596) ---");
    CHECK(setup_workspace(cs), "reflectable workspace setup");
    auto h = cs.eval("(engine:metrics \"query:guard-panic-reflect-stats\")");
    CHECK(h && is_hash(*h), "guard-panic-reflect-stats returns hash");
    CHECK(hash_int(cs, "schema") == 596, "schema == 596");
    const auto s0 = stats_sum(cs);
    std::println("  guard-panic-reflect-stats sum = {}", s0);
    CHECK(s0 >= 0, "guard-panic-reflect-stats non-negative");

    std::println("\n--- AC2: Guard mutate bumps validate + commit ---");
    const auto commits0 = hash_int(cs, "checkpoints-committed");
    const auto pass0 = hash_int(cs, "validation-pass");
    const auto validate0 = cs.evaluator().get_schema_validation_pass_count();
    (void)cs.eval("(mutate:rebind \"x\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto commits1 = hash_int(cs, "checkpoints-committed");
    const auto pass1 = hash_int(cs, "validation-pass");
    const auto validate1 = cs.evaluator().get_schema_validation_pass_count();
    std::println("  commits: {} -> {} validation-pass: {} -> {}", commits0, commits1, pass0, pass1);
    CHECK(commits1 > commits0, "checkpoints-committed bumped after Guard success");
    CHECK(pass1 >= pass0, "validation-pass monotonic after reflect validate hook");
    CHECK(validate1 >= validate0, "schema_validation_pass_count observable");

    std::println("\n--- AC3: schema_validation counters observable ---");
    cs.evaluator().bump_schema_validation_pass_count();
    cs.evaluator().bump_schema_validation_fail_count();
    const auto pass = cs.evaluator().get_schema_validation_pass_count();
    const auto fail = cs.evaluator().get_schema_validation_fail_count();
    std::println("  schema_pass={} schema_fail={}", pass, fail);
    CHECK(pass > 0, "schema_validation_pass_count observable");
    CHECK(fail > 0, "schema_validation_fail_count observable");

    std::println("\n--- AC4: panic-checkpoint-fiber-stats regression ---");
    auto hook = cs.eval("(engine:metrics \"query:panic-checkpoint-fiber-stats\")");
    CHECK(hook.has_value() && is_hash(*hook), "panic-checkpoint-fiber-stats regression for resume");

    std::println("\n--- AC5: boundary-violation-prevented observable ---");
    const auto prevented0 = hash_int(cs, "boundary-violation-prevented");
    cs.evaluator().bump_guard_panic_reflect_boundary_violation_prevented();
    const auto prevented1 = hash_int(cs, "boundary-violation-prevented");
    std::println("  boundary-violation-prevented: {} -> {}", prevented0, prevented1);
    CHECK(prevented1 > prevented0, "boundary-violation-prevented bumped");

    std::println("\n--- AC6: multi-round Guard+mutate stats monotonic ---");
    const auto stats6a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"y\" \"" + std::to_string(10 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:reflect-node-members 0)");
    }
    const auto stats6b = stats_sum(cs);
    std::println("  guard-panic-reflect sum: {} -> {}", stats6a, stats6b);
    CHECK(stats6b >= stats6a, "guard-panic-reflect stats monotonic over matrix");

    std::println("\n--- AC7: query regression ---");
    auto rsm = cs.eval("(engine:metrics \"query:reflection-selfmod-stats\")");
    auto sel = cs.eval("(engine:metrics \"query:self-evolution-loop-stats\")");
    auto pcl = cs.eval("(engine:metrics \"query:panic-checkpoint-lifecycle-stats\")");
    CHECK(rsm && is_int(*rsm), "reflection-selfmod-stats regression");
    // #1883: self-evolution-loop-stats is a structured hash (legacy int sum in "total").
    CHECK(sel && (is_int(*sel) || is_hash(*sel)), "self-evolution-loop-stats regression");
    CHECK(pcl && is_int(*pcl), "panic-checkpoint-lifecycle-stats regression");
}

} // namespace aura_596_detail

int main() {
    aura::compiler::CompilerService cs;
    aura_596_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}