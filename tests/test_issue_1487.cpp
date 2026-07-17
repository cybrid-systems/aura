// @category: integration
// @reason: Issue #1487 — ResourceQuota closed-loop parent (Arena + Guard)
//
// Consolidates #1481 / #1546 / #1547 / #1548:
//   AC1: allocate_raw / allocate_checked reject over memory quota
//   AC2: try_acquire rejects over mutation quota; flush samples checks
//   AC3: query:resource-quota-stats schema + counters
//   AC4: exhaustion loops until typed ResourceQuotaExceeded
//   AC5: metrics rejects_total advance on both paths

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

namespace aura_issue_1487_detail {

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

static void ac1_allocate_raw_and_checked() {
    std::println("\n--- AC1: allocate_raw / allocate_checked memory quota ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64);

    const auto used0 = arena.stats().used;
    const auto rej0 = load_u64(m->resource_quota_rejects_total);

    auto r = ev.allocate_checked(4096);
    CHECK(!r.has_value(), "allocate_checked over quota fails");
    if (!r)
        CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "kind == ResourceQuotaExceeded");
    CHECK(arena.try_allocate(4096) == nullptr, "try_allocate / allocate_raw gate");
    CHECK(arena.stats().used == used0, "no allocation on reject");
    CHECK(load_u64(m->resource_quota_rejects_total) > rej0, "rejects advanced");

    // Under limit succeeds
    auto ok = ev.allocate_checked(16);
    CHECK(ok.has_value() && *ok != nullptr, "under-limit allocate_checked ok");
    ev.set_resource_quota_memory(0); // restore unlimited for later eval
}

static void ac2_try_acquire_and_flush() {
    std::println("\n--- AC2: try_acquire mutation quota + flush check sample ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    bool ok = true;
    auto g1 = Guard::try_acquire(ev, 1, &ok);
    CHECK(g1.has_value(), "first try_acquire ok");
    const auto checks0 = load_u64(m->resource_quota_checks_total);
    // Flush while boundary held samples checks (#1487).
    ev.flush_mutation_boundary();
    CHECK(load_u64(m->resource_quota_checks_total) > checks0, "flush bumps checks_total");
    g1 = {}; // release

    auto g2 = Guard::try_acquire(ev, 1, &ok);
    CHECK(!g2.has_value(), "second try_acquire over mutation budget fails");
    if (!g2)
        CHECK(g2.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "mutation reject ResourceQuotaExceeded");
    ev.set_resource_quota_mutations(0);
    ev.reset_mutation_quota_used();
}

static void ac3_primitive_stats() {
    std::println("\n--- AC3: query:resource-quota-stats ---");
    CompilerService cs;
    // Unlimited memory so eval/parse allocate_raw is not gated.
    cs.evaluator().set_resource_quota_memory(0);
    auto r = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(r && is_hash(*r), "engine:metrics returns hash");
    auto schema = cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") 'schema)");
    CHECK(schema && is_int(*schema) && as_int(*schema) == 1481, "schema == 1481");
}

static void ac4_exhaustion_loops() {
    std::println("\n--- AC4: exhaustion loops until ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(512 * 1024);
    ev.set_arena(&arena);

    // Memory path: per-request limit=32; loop large allocs.
    ev.set_resource_quota_memory(32);
    int mem_rejects = 0;
    for (int i = 0; i < 50; ++i) {
        auto r = ev.allocate_checked(256);
        if (!r) {
            CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded, "mem exhaust typed");
            ++mem_rejects;
        }
    }
    CHECK(mem_rejects == 50, "all large mem allocs rejected under limit=32");
    ev.set_resource_quota_memory(0);

    // Mutation path: budget=5, then reject.
    ev.set_resource_quota_mutations(5);
    ev.reset_mutation_quota_used();
    int mut_ok = 0, mut_rej = 0;
    for (int i = 0; i < 20; ++i) {
        bool flag = true;
        auto g = Guard::try_acquire(ev, 1, &flag);
        if (g)
            ++mut_ok;
        else {
            CHECK(g.error().kind == AuraErrorKind::ResourceQuotaExceeded, "mut exhaust typed");
            ++mut_rej;
        }
    }
    CHECK(mut_ok == 5, "exactly 5 mutation acquires succeed");
    CHECK(mut_rej == 15, "remaining 15 mutation acquires reject");
    CHECK(load_u64(m->resource_quota_rejects_total) >=
              static_cast<std::uint64_t>(mem_rejects + mut_rej),
          "rejects_total covers both paths");
    std::println("  mem_rejects={} mut_ok={} mut_rej={} rejects_total={}", mem_rejects, mut_ok,
                 mut_rej, load_u64(m->resource_quota_rejects_total));
    ev.set_resource_quota_mutations(0);
}

static void ac5_metrics_both_paths() {
    std::println("\n--- AC5: metrics advance on arena + mutation reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(64 * 1024);
    ev.set_arena(&arena);

    const auto c0 = load_u64(m->resource_quota_checks_total);
    const auto r0 = load_u64(m->resource_quota_rejects_total);

    ev.set_resource_quota_memory(8);
    (void)ev.allocate_checked(1024);
    ev.set_resource_quota_memory(0);

    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    {
        bool ok = true;
        (void)Guard::try_acquire(ev, 1, &ok);
    }
    {
        bool ok = true;
        (void)Guard::try_acquire(ev, 1, &ok); // reject
    }

    CHECK(load_u64(m->resource_quota_checks_total) > c0, "checks advanced");
    CHECK(load_u64(m->resource_quota_rejects_total) >= r0 + 2, "≥2 rejects (arena+mutation)");
}

} // namespace aura_issue_1487_detail

int main() {
    using namespace aura_issue_1487_detail;
    std::println("=== Issue #1487: ResourceQuota Arena + Guard closed-loop ===");
    ac1_allocate_raw_and_checked();
    ac2_try_acquire_and_flush();
    ac3_primitive_stats();
    ac4_exhaustion_loops();
    ac5_metrics_both_paths();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
