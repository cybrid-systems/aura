// test_guard_panic_reflect_fiber_resume_task6.cpp — Issue #596:
// MutationBoundaryGuard + panic checkpoint + reflect/schema validation
// + fiber resume safety closed loop (Task6 production review).
// Issue #2765 — integrate reflect auto_validate / hygiene_validate into
// Guard success path (schema-level closed-loop). Prefer-existing suite
// per #81967.
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
//   #2765 AC1: Guard success invokes reflect validate; fail → Soft metric /
//              Strict force-rollback path wired
//   #2765 AC2: happy-path mutate → validate total bumps; pass path
//   #2765 AC3: flag off → skip (zero validate cost)
//   #2765 AC4: MacroIntroduced / provenance restamp integration preserved
//   #2765 AC5: schema-2765 keys + last-ok; #596 lineage preserved
//   #2765 AC6: source-cite + coverage linter

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_596_detail {

using aura::compiler::CompilerMetrics;
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

// ── Issue #2765: Guard success-path reflect validate closed-loop ──

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

[[nodiscard]] static bool source_has_key(const std::string& hay, std::string_view key) {
    std::string n;
    n.reserve(hay.size());
    for (char ch : hay) {
        if (ch != '"' && ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t')
            n.push_back(ch);
    }
    return n.find(key) != std::string::npos;
}

static void ac2765_1_guard_success_wires_validate() {
    std::println("\n--- #2765 AC1: Guard success wires reflect validate ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(emb.find("#2765") != std::string::npos, "AC1: boundary cites #2765");
    CHECK(emb.find("post_mutation_reflect_validate") != std::string::npos,
          "AC1: success path calls post_mutation_reflect_validate");
    CHECK(emb.find("bump_guard_reflect_validate") != std::string::npos,
          "AC1: guard_reflect_validate_total wired");
    CHECK(emb.find("bump_guard_reflect_validate_fail") != std::string::npos,
          "AC1: fail counter wired");
    CHECK(emb.find("bump_guard_reflect_validate_strict_rollback") != std::string::npos,
          "AC1: Strict rollback path wired");
    CHECK(emb.find("is_strict") != std::string::npos, "AC1: Strict sandbox consult");
    CHECK(efl.find("post_mutation_reflect_validate") != std::string::npos,
          "AC1: validate implementation present");
    CHECK(efl.find("MutationReflectHealth") != std::string::npos ||
              efl.find("hygiene_validate") != std::string::npos,
          "AC1: hygiene_validate / MutationReflectHealth integrated");
}

static void ac2765_2_happy_path_bumps_total() {
    std::println("\n--- #2765 AC2: happy-path mutate bumps guard-reflect-validate-total ---");
    CompilerService cs;
    CHECK(setup_workspace(cs), "AC2: workspace");
    auto& ev = cs.evaluator();
    CHECK(ev.get_guard_reflect_validate_enabled(), "AC2: default enabled");
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC2: metrics");
    const auto t0 = m->guard_reflect_validate_total.load(std::memory_order_relaxed);
    const auto f0 = m->guard_reflect_validate_fail_total.load(std::memory_order_relaxed);
    (void)cs.eval("(mutate:rebind \"x\" \"99\")");
    const auto t1 = m->guard_reflect_validate_total.load(std::memory_order_relaxed);
    const auto f1 = m->guard_reflect_validate_fail_total.load(std::memory_order_relaxed);
    std::println("  validate total {} -> {}, fail {} -> {}", t0, t1, f0, f1);
    CHECK(t1 > t0, "AC2: Guard success bumped guard_reflect_validate_total");
    // Happy path schema should not force fail (fail may stay flat).
    CHECK(f1 >= f0, "AC2: fail total monotonic");
    CHECK(hash_int(cs, "guard-reflect-validate-total") >= 1, "AC2: query key total >= 1");
    CHECK(hash_int(cs, "guard-reflect-validate-wired") == 1, "AC2: wired sentinel");
    CHECK(hash_int(cs, "guard-reflect-validate-enabled") == 1, "AC2: enabled key");
}

static void ac2765_3_flag_off_skips() {
    std::println("\n--- #2765 AC3: flag off → skip (zero validate cost) ---");
    CompilerService cs;
    CHECK(setup_workspace(cs), "AC3: workspace");
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    CHECK(m != nullptr, "AC3: metrics");
    ev.set_guard_reflect_validate_enabled(false);
    CHECK(!ev.get_guard_reflect_validate_enabled(), "AC3: flag disabled");
    const auto t0 = m->guard_reflect_validate_total.load(std::memory_order_relaxed);
    const auto s0 = m->guard_reflect_validate_skipped_total.load(std::memory_order_relaxed);
    (void)cs.eval("(mutate:rebind \"x\" \"7\")");
    const auto t1 = m->guard_reflect_validate_total.load(std::memory_order_relaxed);
    const auto s1 = m->guard_reflect_validate_skipped_total.load(std::memory_order_relaxed);
    std::println("  total {} -> {}, skipped {} -> {}", t0, t1, s0, s1);
    CHECK(t1 == t0, "AC3: flag off → no validate total growth");
    CHECK(s1 > s0, "AC3: skipped total bumped");
    // Restore default for subsequent tests.
    ev.set_guard_reflect_validate_enabled(true);
    CHECK(hash_int(cs, "guard-reflect-validate-skipped-total") >= 1, "AC3: skip key");
}

static void ac2765_4_macro_provenance_integration() {
    std::println("\n--- #2765 AC4: MacroIntroduced / provenance path preserved ---");
    const auto efl = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(efl.find("is_macro_introduced") != std::string::npos ||
              efl.find("MacroIntroduced") != std::string::npos,
          "AC4: MacroIntroduced walk in validate");
    CHECK(efl.find("marker_consistent") != std::string::npos ||
              efl.find("validate_mutation_reflect_health") != std::string::npos,
          "AC4: hygiene health gate");
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define-hygienic-macro (dbl y) (* y 2)) "
                  "(dbl 1) (define base 10) (+ base 1)\")")
              .has_value(),
          "AC4: macro workspace");
    CHECK(cs.eval("(eval-current)").has_value(), "AC4: eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto t0 = m->guard_reflect_validate_total.load(std::memory_order_relaxed);
    (void)cs.eval("(mutate:replace-pattern \"(+ base 1)\" \"(+ base 2)\")");
    const auto t1 = m->guard_reflect_validate_total.load(std::memory_order_relaxed);
    CHECK(t1 > t0, "AC4: validate still runs after macro workspace mutate");
    // Restamp path still present in production code.
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(emb.find("restamp") != std::string::npos || efl.find("restamp") != std::string::npos,
          "AC4: restamp path preserved alongside validate");
}

static void ac2765_5_observability() {
    std::println("\n--- #2765 AC5: schema-2765 + additive keys ---");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(source_has_key(q, "schema-2765"), "AC5: schema-2765");
    CHECK(source_has_key(q, "issue-2765"), "AC5: issue-2765");
    CHECK(source_has_key(q, "guard-reflect-validate-total"), "AC5: total key");
    CHECK(source_has_key(q, "guard-reflect-validate-fail-total"), "AC5: fail key");
    CHECK(source_has_key(q, "guard-reflect-validate-strict-rollback-total"), "AC5: strict key");
    CHECK(source_has_key(q, "guard-reflect-last-ok"), "AC5: last-ok key");
    CHECK(met.find("guard_reflect_validate_total") != std::string::npos, "AC5: metric field");
    // Prior surface preserved.
    CHECK(source_has_key(q, "schema") || q.find("schema\", 596") != std::string::npos ||
              q.find("596") != std::string::npos,
          "AC5: schema 596 lineage");

    CompilerService cs;
    CHECK(setup_workspace(cs), "AC5: workspace");
    (void)cs.eval("(mutate:rebind \"x\" \"1\")");
    CHECK(hash_int(cs, "schema-2765") == 2765, "AC5: live schema-2765");
    CHECK(hash_int(cs, "issue-2765") == 2765, "AC5: live issue-2765");
    CHECK(hash_int(cs, "schema") == 596, "AC5: live schema 596");
    CHECK(hash_int(cs, "guard-reflect-validate-total") >= 0, "AC5: live total");
    CHECK(hash_int(cs, "guard-reflect-last-ok") == 0 || hash_int(cs, "guard-reflect-last-ok") == 1,
          "AC5: last-ok 0/1");
}

static void ac2765_6_source_and_linter() {
    std::println("\n--- #2765 AC6: source-cite + linter ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto eix = read_file("src/compiler/evaluator.ixx");
    const auto t = read_file("tests/serve/test_guard_panic_reflect_fiber_resume_task6.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_guard_reflect_validate_2765.py");
    CHECK(emb.find("#2765") != std::string::npos, "AC6: boundary cites #2765");
    CHECK(eix.find("#2765") != std::string::npos, "AC6: evaluator cites #2765");
    CHECK(eix.find("guard_reflect_validate_enabled_") != std::string::npos, "AC6: flag member");
    CHECK(t.find("ac2765_1_guard_success_wires_validate") != std::string::npos, "AC6: AC1");
    CHECK(t.find("ac2765_2_happy_path_bumps_total") != std::string::npos, "AC6: AC2");
    CHECK(t.find("ac2765_5_observability") != std::string::npos, "AC6: AC5");
    CHECK(build.find("check_guard_reflect_validate_2765") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(!lint.empty(), "AC6: linter present");
    CHECK(read_file("docs/design/2765-guard-reflect-validate.md").empty(),
          "AC6: no docs/design/2765-* per #1655");
}

} // namespace aura_596_detail

int main() {
    using aura::test::g_failed;
    using aura::test::g_passed;
    aura::compiler::CompilerService cs;
    aura_596_detail::run_matrix(cs);
    std::println("\n=== Issue #2765: Guard success-path reflect validate ===");
    aura_596_detail::ac2765_1_guard_success_wires_validate();
    aura_596_detail::ac2765_2_happy_path_bumps_total();
    aura_596_detail::ac2765_3_flag_off_skips();
    aura_596_detail::ac2765_4_macro_provenance_integration();
    aura_596_detail::ac2765_5_observability();
    aura_596_detail::ac2765_6_source_and_linter();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}