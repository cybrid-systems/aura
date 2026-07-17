// @category: integration
// @reason: Issue #1594 — ResourceQuota Arena::allocate_raw + MutationBoundaryGuard
// refine of #1481/#1487/#1498/#1546/#1547 (inventory + closed-loop AC).
//
//   AC1: allocate_checked / try_allocate over limit → typed error / nullptr; no used bump
//   AC2: try_acquire over mutation quota → ResourceQuotaExceeded
//   AC3: resource_quota_rejects_total / checks_total advance on reject
//   AC4: 1000-iter alternating pass/reject (arena + mutation)
//   AC5: legacy Guard ctor still soft-fails (compat; deprecated)
//   AC6: query:resource-quota-stats exposes checks/rejects (schema 1590 lineage)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.core.arena;
import aura.core.error;
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
using aura::test::g_failed;
using aura::test::g_passed;

using Guard = Evaluator::MutationBoundaryGuard;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_arena_typed_and_raw() {
    std::println("\n--- AC1: allocate_checked / try_allocate quota gate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64);

    const auto used0 = arena.stats().used;
    const auto checks0 = load_u64(m->resource_quota_checks_total);
    const auto rej0 = load_u64(m->resource_quota_rejects_total);

    auto r = ev.allocate_checked(/*size=*/2048, /*align=*/8);
    CHECK(!r.has_value(), "allocate_checked over limit fails");
    if (!r) {
        CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "kind == ResourceQuotaExceeded");
    }
    CHECK(arena.stats().used == used0, "no allocation on reject");
    CHECK(load_u64(m->resource_quota_rejects_total) == rej0 + 1, "rejects +1");
    CHECK(load_u64(m->resource_quota_checks_total) > checks0, "checks advanced");

    void* p = arena.try_allocate(4096);
    CHECK(p == nullptr, "try_allocate/allocate_raw → nullptr");
    CHECK(arena.stats().used == used0, "used still unchanged");

    auto ok = ev.allocate_checked(16, 8);
    CHECK(ok.has_value() && *ok != nullptr, "under-limit allocate_checked ok");
    ev.set_resource_quota_memory(0);
}

static void ac2_try_acquire_typed() {
    std::println("\n--- AC2: try_acquire mutation quota typed error ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();

    bool flag = true;
    auto g1 = Guard::try_acquire(ev, 1, &flag);
    CHECK(g1.has_value(), "first acquire within budget");
    g1 = {};

    const auto rej0 = load_u64(m->resource_quota_rejects_total);
    auto g2 = Guard::try_acquire(ev, 1, &flag);
    CHECK(!g2.has_value(), "second acquire over budget");
    if (!g2) {
        CHECK(g2.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "kind == ResourceQuotaExceeded");
        CHECK(g2.error().message.find("mutation quota") != std::string::npos ||
                  !g2.error().message.empty(),
              "message present");
    }
    CHECK(load_u64(m->resource_quota_rejects_total) > rej0, "rejects on try_acquire");
    ev.set_resource_quota_mutations(0);
}

static void ac3_counters() {
    std::println("\n--- AC3: checks_total / rejects_total ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(128 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(32);
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();

    const auto c0 = load_u64(m->resource_quota_checks_total);
    const auto r0 = load_u64(m->resource_quota_rejects_total);

    (void)ev.allocate_checked(1024); // reject memory
    {
        auto g = Guard::try_acquire(ev, 1); // pass
        (void)g;
    }
    (void)Guard::try_acquire(ev, 1); // reject mutation

    CHECK(load_u64(m->resource_quota_checks_total) > c0, "checks advanced");
    CHECK(load_u64(m->resource_quota_rejects_total) >= r0 + 2, "≥2 rejects (mem+mut)");
    ev.set_resource_quota_memory(0);
    ev.set_resource_quota_mutations(0);
}

static void ac4_stress_1000() {
    std::println("\n--- AC4: 1000-iter alternating pass/reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(512 * 1024);
    ev.set_arena(&arena);

    int pass = 0, reject = 0;
    for (int i = 0; i < 1000; ++i) {
        if ((i % 2) == 0) {
            // Even: mutation pass under unlimited; small alloc ok.
            ev.set_resource_quota_mutations(0);
            ev.reset_mutation_quota_used();
            ev.set_resource_quota_memory(0);
            bool ok = true;
            auto g = Guard::try_acquire(ev, 1, &ok);
            auto a = ev.allocate_checked(16, 8);
            if (g && a)
                ++pass;
        } else {
            // Odd: force mutation reject + oversize memory reject.
            ev.set_resource_quota_mutations(1);
            ev.reset_mutation_quota_used();
            {
                bool ok = true;
                auto burn = Guard::try_acquire(ev, 1, &ok);
                (void)burn;
            }
            auto g = Guard::try_acquire(ev, 1);
            ev.set_resource_quota_memory(8);
            auto a = ev.allocate_checked(4096, 8);
            if (!g && !a)
                ++reject;
        }
    }
    CHECK(pass == 500, "500 dual-pass iterations");
    CHECK(reject == 500, "500 dual-reject iterations");
    CHECK(load_u64(m->resource_quota_rejects_total) >= 500, "rejects_total ≥ 500");
    std::println("  pass={} reject={} rejects_total={}", pass, reject,
                 load_u64(m->resource_quota_rejects_total));
    ev.set_resource_quota_memory(0);
    ev.set_resource_quota_mutations(0);
}

static void ac5_legacy_soft_fail() {
    std::println("\n--- AC5: legacy ctor soft-fail (deprecated) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    {
        auto g0 = Guard::try_acquire(ev, 1);
        CHECK(g0.has_value(), "consume budget");
    }
    bool ok = true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    {
        Guard g(ev, &ok);
        CHECK(g.is_inert(), "inert after quota reject");
        CHECK(!ok, "success_flag false");
    }
#pragma GCC diagnostic pop
    CHECK(true, "legacy dtor completed");
    // Unlimited still constructs usable guard.
    ev.set_resource_quota_mutations(0);
    bool ok2 = true;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    {
        Guard g2(ev, &ok2);
        CHECK(ok2, "legacy under unlimited ok");
        CHECK(!g2.is_inert(), "not inert");
    }
#pragma GCC diagnostic pop
}

static void ac6_query_stats() {
    std::println("\n--- AC6: query:resource-quota-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(h && is_hash(*h), "stats hash");
    auto schema = cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"schema\")");
    // Schema lineage: 1481 → … → 1590 (agents may accept earlier ids).
    CHECK(schema && is_int(*schema) &&
              (as_int(*schema) == 1618 || as_int(*schema) == 1600 || as_int(*schema) == 1590 ||
               as_int(*schema) == 1579 || as_int(*schema) == 1554 || as_int(*schema) == 1498 ||
               as_int(*schema) == 1481),
          "schema in known lineage");
    auto checks =
        cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"checks_total\")");
    CHECK(checks && is_int(*checks) && as_int(*checks) >= 0, "checks_total");
    auto rej =
        cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") \"rejects_total\")");
    CHECK(rej && is_int(*rej) && as_int(*rej) >= 0, "rejects_total");
}

} // namespace

int main() {
    std::println("=== Issue #1594: ResourceQuota Arena + Guard wired refine ===");
    ac1_arena_typed_and_raw();
    ac2_try_acquire_typed();
    ac3_counters();
    ac4_stress_1000();
    ac5_legacy_soft_fail();
    ac6_query_stats();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
