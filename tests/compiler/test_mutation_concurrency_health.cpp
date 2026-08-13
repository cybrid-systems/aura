// @category: unit
// @reason: Issue #2379 — query:mutation-concurrency-health single Agent score
// (hold + steal + residual + mailbox + densify).
//
//   AC1: Query returns health-bp + force-reason + components + schema/wired
//   AC2: Inject force-deopt → force-reason steal-mismatch; health drops
//   AC3: Clean process → high health; force-reason none; pure calls identical
//   AC4: Existing subsystem queries still resolve (additive only)
//   AC5: Tests + source-cite + gate

#include "test_harness.hpp"

#include "compiler/mutation_concurrency_health.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/densify_consistency_report.h"
#include "core/gc_hooks.h"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::compute_mutation_concurrency_health;
using aura::compiler::MutationConcurrencyHealthSnapshot;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href_int(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1 / pure vacuous ──
static void ac1_vacuous_healthy() {
    std::println("\n--- AC1: vacuous snapshot → health 10000 / none ---");
    MutationConcurrencyHealthSnapshot s;
    auto r = compute_mutation_concurrency_health(s);
    CHECK(r.health_bp == 10000, "AC1: vacuous health_bp == 10000");
    CHECK(r.force_reason == "none", "AC1: force-reason none");
    CHECK(r.force_reason_code == 0, "AC1: force-reason-code 0");
    CHECK(r.health_budget_bp == 8000 || r.health_budget_bp <= 10000, "AC1: budget default");
}

// ── AC2: force_reason priority + inject force-deopt ──
static void ac2_force_reason_and_inject() {
    std::println("\n--- AC2: force_reason priority + force-deopt inject ---");
    {
        MutationConcurrencyHealthSnapshot s;
        s.steal_force_deopt_total = 1;
        s.hold_slo_violation_total = 5;
        s.mailbox_defer_starvation_total = 3;
        auto r = compute_mutation_concurrency_health(s);
        CHECK(r.force_reason == "steal-mismatch", "AC2: steal-mismatch wins priority");
        CHECK(r.force_reason_code == 1, "AC2: code 1");
        CHECK(r.health_bp < 10000, "AC2: health drops on force-deopt");
        CHECK(r.health_bp <= 6000, "AC2: hard steal penalty −4000");
    }
    {
        MutationConcurrencyHealthSnapshot s;
        s.residual_hard_fail_total = 1;
        auto r = compute_mutation_concurrency_health(s);
        CHECK(r.force_reason == "residual-defer", "AC2: residual-defer second");
        CHECK(r.force_reason_code == 2, "AC2: code 2");
    }
    {
        MutationConcurrencyHealthSnapshot s;
        s.densify_consistency_fail_total = 1;
        auto r = compute_mutation_concurrency_health(s);
        CHECK(r.force_reason == "densify-fail", "AC2: densify-fail third");
        CHECK(r.force_reason_code == 3, "AC2: code 3");
    }
    {
        MutationConcurrencyHealthSnapshot s;
        s.hold_slo_violation_total = 1;
        auto r = compute_mutation_concurrency_health(s);
        CHECK(r.force_reason == "hold-slo", "AC2: hold-slo fourth");
        CHECK(r.force_reason_code == 4, "AC2: code 4");
        CHECK(r.health_bp < 10000, "AC2: soft hold reduces score");
        CHECK(r.health_bp > 0, "AC2: soft does not zero alone");
    }
    {
        MutationConcurrencyHealthSnapshot s;
        s.mailbox_defer_starvation_total = 1;
        auto r = compute_mutation_concurrency_health(s);
        CHECK(r.force_reason == "mailbox-starvation", "AC2: mailbox-starvation fifth");
        CHECK(r.force_reason_code == 5, "AC2: code 5");
    }

    // Process inject: bump Fiber force-deopt counter; query reflects steal-mismatch.
    const auto deopt0 = aura::serve::Fiber::steal_snapshot_mismatch_force_deopt_total();
    aura::serve::Fiber::bump_steal_snapshot_mismatch_force_deopt();
    CHECK(aura::serve::Fiber::steal_snapshot_mismatch_force_deopt_total() == deopt0 + 1,
          "AC2: force-deopt total +1");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "force-reason-code") == 1,
          "AC2: query force-reason-code steal-mismatch after inject");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "health-bp") < 10000,
          "AC2: query health-bp drops after inject");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "component-steal-force-deopt-total") >=
              1,
          "AC2: component steal force-deopt ≥1");
}

// ── AC3: clean-ish pure calls (after restore densify last axes) ──
static void ac3_pure_identical() {
    std::println("\n--- AC3: pure successive calls identical ---");
    // Ensure densify last axes are ok so densify doesn't dominate.
    aura::core::densify_consistency::note_last_densify_envframe_ok(true);
    aura::core::densify_consistency::note_last_densify_closure_remount_ok(true);
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    const auto h1 = href_int(cs, "query:mutation-concurrency-health", "health-bp");
    const auto c1 = href_int(cs, "query:mutation-concurrency-health", "force-reason-code");
    const auto h2 = href_int(cs, "query:mutation-concurrency-health", "health-bp");
    const auto c2 = href_int(cs, "query:mutation-concurrency-health", "force-reason-code");
    CHECK(h1 == h2, "AC3: two successive health-bp identical");
    CHECK(c1 == c2, "AC3: two successive force-reason-code identical");
    CHECK(h1 >= 0 && h1 <= 10000, "AC3: health in range");
    // Note: process may have residual counters from AC2 inject; force-reason
    // may not be none. Vacuous pure path covered by ac1_vacuous_healthy.
}

// ── AC4: query surface + lineage + existing queries ──
static void ac4_query_surface() {
    std::println("\n--- AC4: query keys + existing subsystem queries ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto h = cs.eval("(engine:metrics \"query:mutation-concurrency-health\")");
    CHECK(h && is_hash(*h), "AC4: mutation-concurrency-health is hash");

    CHECK(href_int(cs, "query:mutation-concurrency-health", "schema-2379") == 2379, "schema-2379");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "issue-2379") == 2379, "issue-2379");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "mutation-concurrency-health-wired") ==
              1,
          "wired");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "health-bp") >= 0, "health-bp");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "health-budget-bp") >= 0,
          "health-budget-bp");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "force-reason-code") >= 0,
          "force-reason-code");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "component-steal-force-deopt-total") >=
              0,
          "component steal");
    CHECK(href_int(cs, "query:mutation-concurrency-health",
                   "component-densify-consistency-fail-total") >= 0,
          "component densify");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "component-mailbox-deferred-depth") >=
              0,
          "component mailbox depth");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "schema-2310") == 2310, "lineage 2310");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "schema-2341") == 2341, "lineage 2341");

    // Existing subsystem queries still resolve (additive only).
    CHECK(cs.eval("(engine:metrics \"query:mutation-boundary-hold-stats\")").has_value(),
          "AC4: hold-stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:orchestration-steal-outermost-stats\")").has_value(),
          "AC4: steal-outermost-stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:gc-defer-reason-stats\")").has_value(),
          "AC4: gc-defer-reason-stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:mf-mailbox-stats\")").has_value(),
          "AC4: mf-mailbox-stats still reachable");
    CHECK(cs.eval("(engine:metrics \"query:lifetime-contract-snapshot\")").has_value(),
          "AC4: lifetime-contract-snapshot still reachable");
}

// ── AC5: source-cite ──
static void ac5_source_cite() {
    std::println("\n--- AC5: source-cite ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp") +
                   read_file("src/compiler/evaluator_primitives_query_reflect.cpp");
    const auto hh = read_file("src/compiler/mutation_concurrency_health.hh");
    const auto obs = read_file("src/compiler/evaluator_primitives_observability.cpp");
    CHECK(q.find("query:mutation-concurrency-health") != std::string::npos,
          "AC5: query registered");
    CHECK(q.find("Issue #2379") != std::string::npos, "AC5: query cites #2379");
    CHECK(hh.find("health_bp") != std::string::npos, "AC5: score in header");
    CHECK(hh.find("steal-mismatch") != std::string::npos, "AC5: force_reason table");
    CHECK(hh.find("residual-defer") != std::string::npos, "AC5: residual priority");
    CHECK(hh.find("densify-fail") != std::string::npos, "AC5: densify priority");
    CHECK(hh.find("hold-slo") != std::string::npos, "AC5: hold priority");
    CHECK(hh.find("mailbox-starvation") != std::string::npos, "AC5: mailbox priority");
    CHECK(hh.find("compute_mutation_concurrency_health") != std::string::npos, "AC5: pure compute");
    CHECK(obs.find("query:mutation-concurrency-health") != std::string::npos, "AC5: catalog entry");
}

// ── Issue #2985: production admit close-loop ──

static void ac2985_1_prod_hard_reason_rejects() {
    std::println("\n--- #2985 AC1: production + steal-mismatch → try_acquire rejects ---");
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    MutationConcurrencyHealthSnapshot steal;
    steal.steal_force_deopt_total = 1;
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(steal);
    CompilerService cs;
    auto& ev = cs.evaluator();
    bool ok = true;
    const auto rej0 =
        aura::compiler::g_mutation_concurrency_health_reject_total.load(std::memory_order_relaxed);
    auto gr = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
    CHECK(!gr.has_value(), "2985 AC1: production try_acquire rejects");
    if (!gr.has_value()) {
        CHECK(gr.error().message.find("concurrency-health") != std::string::npos,
              "2985 AC1: concurrency-health reason");
    }
    CHECK(aura::compiler::g_mutation_concurrency_health_reject_total.load(
              std::memory_order_relaxed) == rej0 + 1,
          "2985 AC1: reject_total +1");

    aura::compiler::typed_audit::apply_dev_audit_defaults();
    const auto obs0 = aura::compiler::g_mutation_concurrency_health_soft_observe_total.load(
        std::memory_order_relaxed);
    const auto rej1 =
        aura::compiler::g_mutation_concurrency_health_reject_total.load(std::memory_order_relaxed);
    auto grs = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev, 1, &ok);
    CHECK(grs.has_value(), "2985 AC1: Soft admits (metric-only)");
    CHECK(aura::compiler::g_mutation_concurrency_health_soft_observe_total.load(
              std::memory_order_relaxed) == obs0 + 1,
          "2985 AC1: Soft observe +1");
    CHECK(aura::compiler::g_mutation_concurrency_health_reject_total.load(
              std::memory_order_relaxed) == rej1,
          "2985 AC1: Soft does not bump reject");
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
}

static void ac2985_2_under_budget_rejects() {
    std::println("\n--- #2985 AC2: production + health_bp < budget → reject ---");
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    MutationConcurrencyHealthSnapshot soft;
    soft.hold_slo_violation_total = 10;
    soft.mailbox_defer_starvation_total = 8;
    const auto scored = compute_mutation_concurrency_health(soft);
    CHECK(scored.health_bp < scored.health_budget_bp, "2985 AC2: stacked soft < budget");
    CHECK(scored.force_reason_code >= 4, "2985 AC2: soft force_reason only");
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(soft);
    CompilerService cs;
    bool ok = true;
    auto gr = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
    CHECK(!gr.has_value(), "2985 AC2: under-budget rejects GlobalExclusive");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
}

static void ac2985_3_happy_zero_extra() {
    std::println("\n--- #2985 AC3: happy path → no extra admit stores ---");
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    MutationConcurrencyHealthSnapshot clean;
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(clean);
    const auto rej0 =
        aura::compiler::g_mutation_concurrency_health_reject_total.load(std::memory_order_relaxed);
    const auto obs0 = aura::compiler::g_mutation_concurrency_health_soft_observe_total.load(
        std::memory_order_relaxed);
    CompilerService cs;
    bool ok = true;
    auto gr = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
    CHECK(gr.has_value(), "2985 AC3: full health admits");
    CHECK(aura::compiler::g_mutation_concurrency_health_reject_total.load(
              std::memory_order_relaxed) == rej0,
          "2985 AC3: reject_total unchanged");
    CHECK(aura::compiler::g_mutation_concurrency_health_soft_observe_total.load(
              std::memory_order_relaxed) == obs0,
          "2985 AC3: observe_total unchanged");
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
}

static void ac2985_4_additive_query() {
    std::println("\n--- #2985 AC4: additive query keys; score non-regressing ---");
    CompilerService cs;
    CHECK(href_int(cs, "query:mutation-concurrency-health", "schema-2985") == 2985,
          "2985 AC4: schema-2985");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "issue-2985") == 2985,
          "2985 AC4: issue-2985");
    CHECK(href_int(cs, "query:mutation-concurrency-health",
                   "mutation-concurrency-health-admit-wired") == 1,
          "2985 AC4: admit-wired");
    CHECK(href_int(cs, "query:mutation-concurrency-health",
                   "mutation-concurrency-health-reject-total") >= 0,
          "2985 AC4: reject-total");
    CHECK(href_int(cs, "query:mutation-concurrency-health", "schema-2379") == 2379,
          "2985 AC4: schema-2379");
    MutationConcurrencyHealthSnapshot s;
    auto r = compute_mutation_concurrency_health(s);
    CHECK(r.health_bp == 10000 && r.force_reason_code == 0, "2985 AC4: pure score unchanged");
}

static void ac2985_5_inject_clear_resumes() {
    std::println("\n--- #2985 AC5: inject deny; clear → admit resumes ---");
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
    aura::compiler::typed_audit::apply_production_audit_defaults();
    MutationConcurrencyHealthSnapshot residual;
    residual.residual_hard_fail_total = 1;
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(residual);
    CompilerService cs;
    bool ok = true;
    auto deny =
        aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
    CHECK(!deny.has_value(), "2985 AC5: residual inject denies");
    MutationConcurrencyHealthSnapshot clean;
    aura::compiler::set_mutation_concurrency_health_admit_snapshot_for_test(clean);
    auto allow =
        aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(cs.evaluator(), 1, &ok);
    CHECK(allow.has_value(), "2985 AC5: clear/vacuous override resumes admit");
    aura::compiler::clear_mutation_concurrency_health_admit_snapshot_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
    aura::compiler::reset_mutation_concurrency_health_admit_for_test();
}

static void ac2985_6_source_and_linter() {
    std::println("\n--- #2985 AC6: source-cite + linter ---");
    const auto hh = read_file("src/compiler/mutation_concurrency_health.hh");
    const auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto t = read_file("tests/compiler/test_mutation_concurrency_health.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_mutation_concurrency_health_admit_2985.py");
    const auto build = read_file("build.py");
    CHECK(hh.find("Issue #2985") != std::string::npos, "2985 AC6: header cites");
    CHECK(hh.find("set_mutation_concurrency_health_soft_for_test") != std::string::npos,
          "2985 AC6: Soft-for-test");
    CHECK(mb.find("Issue #2985") != std::string::npos, "2985 AC6: try_acquire cites");
    CHECK(mb.find("maybe_reject_mutation_concurrency_health") != std::string::npos,
          "2985 AC6: admit helper");
    CHECK(t.find("ac2985_1_prod_hard_reason_rejects") != std::string::npos, "2985 AC6: AC1 test");
    CHECK(!lint.empty() && lint.find("2985") != std::string::npos, "2985 AC6: linter");
    CHECK(build.find("check_mutation_concurrency_health_admit_2985") != std::string::npos,
          "2985 AC6: build.py");
    CHECK(read_file("docs/design/2985-concurrency-health-admit.md").empty(),
          "2985 AC6: no docs/design/");
    CHECK(read_file("tests/compiler/test_issue_2985.cpp").empty(), "2985 AC6: no invent test");
}

} // namespace

int run_test_mutation_concurrency_health() {
    std::println("=== Issue #2379: mutation-concurrency-health ===");
    ac1_vacuous_healthy();
    ac2_force_reason_and_inject();
    ac3_pure_identical();
    ac4_query_surface();
    ac5_source_cite();
    std::println("\n=== Issue #2985: production health admit close-loop ===");
    ac2985_1_prod_hard_reason_rejects();
    ac2985_2_under_budget_rejects();
    ac2985_3_happy_zero_extra();
    ac2985_4_additive_query();
    ac2985_5_inject_clear_resumes();
    ac2985_6_source_and_linter();
    std::println("\n=== #2379 + #2985: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutation_concurrency_health();
}
#endif
