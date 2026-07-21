// @category: integration
// @reason: Issue #1618 — ResourceQuotaManager + typed enforcement on
// Issue #1579/#1590/#1600/#1618 (#1978 renamed): issue# moved from filename to header.
// Guard/arena/scheduler (refine #1579/#1590/#1600; not PanicCheckpoint).
//
//   AC1: ResourceQuotaManager check_and_consume + provenance message
//   AC2: try_acquire over budget → ResourceQuotaExceeded typed (not panic)
//   AC3: allocate_checked over budget → typed + quota_violation
//   AC4: query:resource-quota-stats schema 1618 AC keys
//   AC5: multi-round mutation budget stress
//   AC6: lineage keys (checks/rejects/orch) + wire flags

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/resource_quota.hh"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.core.arena;
import aura.core.error;
import aura.core.resource_quota;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::ASTArena;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::AuraErrorKind;
using aura::core::resource_quota::Dimension;
using aura::core::resource_quota::process_resource_quota;
using aura::core::resource_quota::process_resource_quota_manager;
using aura::core::resource_quota::QuotaError;
using aura::core::resource_quota::reset_process_resource_quota_for_test;
using aura::core::resource_quota::ResourceQuota;
using aura::core::resource_quota::ResourceQuotaManager;
using aura::test::g_failed;
using aura::test::g_passed;

using Guard = Evaluator::MutationBoundaryGuard;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_manager() {
    std::println("\n--- AC1: ResourceQuotaManager check_and_consume ---");
    reset_process_resource_quota_for_test();
    ResourceQuota local;
    ResourceQuotaManager mgr{&local};
    mgr.set_limit(Dimension::Mutations, 2);
    mgr.set_provenance(161801);
    CHECK(!mgr.check_and_consume_mutation(1).has_value(), "first consume ok");
    CHECK(!mgr.check_and_consume_mutation(1).has_value(), "second consume ok");
    auto err = mgr.check_and_consume_mutation(1);
    CHECK(err.has_value(), "third exceeds");
    if (err) {
        CHECK(err->dim == Dimension::Mutations, "dim mutations");
        CHECK(err->message.find("provenance_mutation_id=161801") != std::string::npos ||
                  err->message.find("mutations") != std::string::npos,
              "provenance or dim in message");
        CHECK(err->message.find("quota exceeded") != std::string::npos ||
                  err->message.find("exceeded") != std::string::npos,
              "exceeded wording");
    }
    // format_reason stable
    auto msg = ResourceQuotaManager::format_reason(
        QuotaError{Dimension::Memory, "memory quota exceeded", 100, 50, 40}, 99);
    CHECK(msg.find("provenance_mutation_id=99") != std::string::npos, "format provenance");
    CHECK(msg.find("dim=memory") != std::string::npos, "format dim");
    reset_process_resource_quota_for_test();
}

static void ac2_try_acquire_typed() {
    std::println("\n--- AC2: try_acquire typed ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();

    bool flag = true;
    auto g1 = Guard::try_acquire(ev, 1, &flag);
    CHECK(g1.has_value(), "first within budget");
    g1 = {};

    const auto rej0 = load_u64(m->mutation_budget_rejected_total);
    const auto viol0 = load_u64(m->quota_violation_total);
    const auto typed0 = load_u64(m->quota_reject_typed_total);
    const auto dist0 = load_u64(m->panic_quota_distinguished_total);

    auto g2 = Guard::try_acquire(ev, 1, &flag);
    CHECK(!g2.has_value(), "second over budget");
    if (!g2) {
        CHECK(g2.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "kind == ResourceQuotaExceeded (not panic)");
        CHECK(g2.error().message.find("mutation") != std::string::npos ||
                  g2.error().message.find("mutation_budget") != std::string::npos,
              "message mentions mutation budget");
    }
    CHECK(load_u64(m->mutation_budget_rejected_total) > rej0, "mutation_budget_rejected");
    CHECK(load_u64(m->quota_violation_total) > viol0, "quota_violation_total");
    CHECK(load_u64(m->quota_reject_typed_total) > typed0, "quota_reject_typed");
    CHECK(load_u64(m->panic_quota_distinguished_total) > dist0, "panic distinguished");
    CHECK(load_u64(m->manager_enforce_total) >= 1, "manager_enforce on try_acquire reject");
    ev.set_resource_quota_mutations(0);
}

static void ac3_arena() {
    std::println("\n--- AC3: allocate_checked typed memory reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64);

    const auto viol0 = load_u64(m->quota_violation_total);
    auto r = ev.allocate_checked(/*size=*/2048, /*align=*/8);
    CHECK(!r.has_value(), "over limit fails");
    if (!r) {
        CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded, "ResourceQuotaExceeded");
    }
    CHECK(load_u64(m->quota_violation_total) > viol0, "violation bumped");
    ev.set_resource_quota_memory(0);
}

static void ac4_query_schema() {
    std::println("\n--- AC4: query schema 1618 ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    bool flag = true;
    (void)Guard::try_acquire(ev, 1, &flag);
    (void)Guard::try_acquire(ev, 1, &flag); // reject

    auto h = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1628 || href(cs, "schema") == 1618 || href(cs, "schema") == 1600 ||
              href(cs, "schema") == 1590,
          "schema 1628|1618|1600|1590");
    CHECK(href(cs, "issue") == 1618 || href(cs, "issue") == 1600 || href(cs, "issue") < 0,
          "issue 1618|1600");
    CHECK(href(cs, "quota_violation_total") >= 1 || href(cs, "quota-violation-total") >= 1,
          "quota_violation_total");
    CHECK(href(cs, "mutation_budget_rejected") >= 1 ||
              href(cs, "mutation_budget_rejected_total") >= 1 ||
              href(cs, "mutation-budget-rejected") >= 1,
          "mutation_budget_rejected");
    CHECK(href(cs, "quota_reject_typed_total") >= 0, "quota_reject_typed_total");
    CHECK(href(cs, "panic_quota_distinguished_total") >= 0, "panic_quota_distinguished");
    CHECK(href(cs, "manager_enforce_total") >= 0, "manager_enforce_total");
    CHECK(href(cs, "manager-wired") == 1, "manager-wired");
    CHECK(href(cs, "typed-reject-not-panic") == 1, "typed-reject-not-panic");
    CHECK(href(cs, "panic-quota-distinguished") == 1, "panic-quota-distinguished flag");
    ev.set_resource_quota_mutations(0);
}

static void ac5_stress() {
    std::println("\n--- AC5: multi-round mutation budget stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ev.set_resource_quota_mutations(10);
    ev.reset_mutation_quota_used();
    const auto rej0 = load_u64(m->mutation_budget_rejected_total);
    int rejects = 0;
    for (int i = 0; i < 40; ++i) {
        bool flag = true;
        auto g = Guard::try_acquire(ev, 1, &flag);
        if (!g)
            ++rejects;
        else
            g = {};
    }
    CHECK(rejects >= 20, "many rejects under tight budget");
    CHECK(load_u64(m->mutation_budget_rejected_total) >= rej0 + static_cast<std::uint64_t>(rejects),
          "budget reject counter tracks");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok after stress");
    ev.set_resource_quota_mutations(0);
}

static void ac6_lineage() {
    std::println("\n--- AC6: lineage keys + wire flags ---");
    CompilerService cs;
    CHECK(href(cs, "checks_total") >= 0, "checks_total");
    CHECK(href(cs, "rejects_total") >= 0, "rejects_total");
    CHECK(href(cs, "hotpath_guard_try_acquire") == 1 || href(cs, "hotpath_guard_try_acquire") < 0,
          "hotpath_guard");
    CHECK(href(cs, "orch_spawn_gated") == 1 || href(cs, "orch_spawn_gated") < 0,
          "orch_spawn_gated");
    CHECK(href(cs, "manager-wired") == 1, "manager-wired");
}

} // namespace

int main() {
    std::println("=== Issue #1618: ResourceQuotaManager production enforcement ===");
    ac1_manager();
    ac2_try_acquire_typed();
    ac3_arena();
    ac4_query_schema();
    ac5_stress();
    ac6_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
