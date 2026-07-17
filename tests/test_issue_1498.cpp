// @category: integration
// @reason: Issue #1498 — ResourceQuota production closed-loop
// (Arena allocate + MutationBoundaryGuard + stats; refine #1481/#1487).
//
//   AC1: allocate_raw / allocate_checked + Guard try_acquire typed reject
//   AC2: query:resource-quota-stats has current_usage / memory_quota /
//        exceeded_count / schema 1498
//   AC3: cumulative memory_total quota rejects when used+request exceeds
//   AC4: exhaustion loops until ResourceQuotaExceeded
//   AC5: registry-registered eval under unlimited quota still works
//   AC6: metrics checks/rejects advance on both paths

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

static void ac1_allocate_and_guard() {
    std::println("\n--- AC1: allocate + Guard typed ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64);

    auto r = ev.allocate_checked(4096);
    CHECK(!r.has_value(), "allocate_checked over per-request quota fails");
    if (!r)
        CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded, "arena typed error");
    CHECK(arena.try_allocate(4096) == nullptr, "allocate_raw gate");
    ev.set_resource_quota_memory(0);

    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    bool ok = true;
    auto g1 = Guard::try_acquire(ev, 1, &ok);
    CHECK(g1.has_value(), "first try_acquire ok");
    g1 = {};
    auto g2 = Guard::try_acquire(ev, 1, &ok);
    CHECK(!g2.has_value(), "second try_acquire rejects");
    if (!g2)
        CHECK(g2.error().kind == AuraErrorKind::ResourceQuotaExceeded, "guard typed error");
    CHECK(load_u64(m->resource_quota_rejects_total) >= 2, "rejects advanced");
    ev.set_resource_quota_mutations(0);
    ev.reset_mutation_quota_used();
}

static void ac2_stats_fields() {
    std::println("\n--- AC2: query:resource-quota-stats production fields ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(64 * 1024);
    ev.set_arena(&arena);
    // Touch usage.
    (void)ev.allocate_checked(128);
    ev.set_resource_quota_memory(0); // unlimited for eval parse

    auto r = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(r && is_hash(*r), "stats hash");
    auto schema = cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") 'schema)");
    // Schema 1498 → 1554 (#1554 temp/group wiring fields); accept both.
    CHECK(schema && is_int(*schema) && (as_int(*schema) == 1554 || as_int(*schema) == 1498),
          "schema == 1554 (or legacy 1498)");

    for (const auto* k : {"current_usage", "memory_quota", "memory_quota_total", "exceeded_count",
                          "checks_total", "rejects_total", "mutations_used"}) {
        auto f = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:resource-quota-stats\") '{})", k));
        CHECK(f && is_int(*f), std::format("field '{}' present as int", k));
    }
    auto usage =
        cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") 'current_usage)");
    CHECK(usage && is_int(*usage) && as_int(*usage) >= 0, "current_usage >= 0");
}

static void ac3_cumulative_total() {
    std::println("\n--- AC3: cumulative memory_total quota ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    // Per-request unlimited; cumulative total = 200 bytes.
    ev.set_resource_quota_memory(0);
    ev.set_resource_quota_memory_total(200);

    auto a1 = ev.allocate_checked(100);
    CHECK(a1.has_value() && *a1 != nullptr, "first 100 bytes under total=200");
    const auto used_after = ev.resource_quota_current_usage();
    CHECK(used_after >= 100, "usage advanced");

    const auto rej0 = load_u64(m->resource_quota_rejects_total);
    auto a2 = ev.allocate_checked(150); // used + 150 likely > 200
    CHECK(!a2.has_value(), "second alloc over cumulative total fails");
    if (!a2) {
        CHECK(a2.error().kind == AuraErrorKind::ResourceQuotaExceeded, "cumulative typed");
        CHECK(a2.error().message.find("cumulative") != std::string::npos ||
                  a2.error().message.find("quota") != std::string::npos,
              "message mentions quota");
    }
    CHECK(load_u64(m->resource_quota_rejects_total) > rej0, "rejects on cumulative");
    CHECK(arena.try_allocate(150) == nullptr, "allocate_raw respects cumulative via owner");
    ev.set_resource_quota_memory_total(0);
}

static void ac4_exhaustion_loops() {
    std::println("\n--- AC4: exhaustion until ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(512 * 1024);
    ev.set_arena(&arena);

    ev.set_resource_quota_memory(32);
    int mem_rej = 0;
    for (int i = 0; i < 40; ++i) {
        auto r = ev.allocate_checked(256);
        if (!r) {
            CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded, "mem typed");
            ++mem_rej;
        }
    }
    CHECK(mem_rej == 40, "all large mem allocs rejected");
    ev.set_resource_quota_memory(0);

    ev.set_resource_quota_mutations(3);
    ev.reset_mutation_quota_used();
    int mut_ok = 0, mut_rej = 0;
    for (int i = 0; i < 12; ++i) {
        bool flag = true;
        auto g = Guard::try_acquire(ev, 1, &flag);
        if (g)
            ++mut_ok;
        else {
            CHECK(g.error().kind == AuraErrorKind::ResourceQuotaExceeded, "mut typed");
            ++mut_rej;
        }
    }
    CHECK(mut_ok == 3, "exactly 3 mutation acquires");
    CHECK(mut_rej == 9, "9 mutation rejects");
    ev.set_resource_quota_mutations(0);
    ev.reset_mutation_quota_used();
}

static void ac5_registry_eval_under_unlimited() {
    std::println("\n--- AC5: registry primitives work under unlimited quota ---");
    CompilerService cs;
    cs.evaluator().set_resource_quota_memory(0);
    cs.evaluator().set_resource_quota_memory_total(0);
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto r = cs.eval("(+ 20 22)");
    CHECK(r.has_value(), "eval ok");
    // Stats primitive itself is registry-registered via register_stats_impl.
    auto s = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(s && is_hash(*s), "resource-quota-stats callable");
}

static void ac6_metrics_both_paths() {
    std::println("\n--- AC6: metrics on arena + mutation paths ---");
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
        (void)Guard::try_acquire(ev, 1, &ok);
    }

    CHECK(load_u64(m->resource_quota_checks_total) > c0, "checks advanced");
    CHECK(load_u64(m->resource_quota_rejects_total) >= r0 + 2, "≥2 rejects");
}

} // namespace

int main() {
    std::println("test_issue_1498: ResourceQuota production closed-loop (#1498)");
    ac1_allocate_and_guard();
    ac2_stats_fields();
    ac3_cumulative_total();
    ac4_exhaustion_loops();
    ac5_registry_eval_under_unlimited();
    ac6_metrics_both_paths();
    std::println("\n#1498: {} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
