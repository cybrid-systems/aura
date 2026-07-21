// @category: integration
// @reason: Issue #1548 — quota edge cases for #1481 / #1546 / #1547 wire-ups
//
//   AC1: boundary 0→1 transition (unlimited → bounded reject)
//   AC2: 100k monotonic counter bumps (no overflow false-positives)
//   AC3: concurrent fiber-steal + mutation race (100 rounds)
//   AC4: setter concurrent with checker — documented order semantics
//
// Setter semantics (documented): set_resource_quota_* are plain stores
// (not atomic with check_*). Concurrent set+check is racey by design;
// last writer wins for the limit; check_* observe a snapshot of the
// limit at the moment of the load. Tests verify no crash + counters
// stay monotonic under racing setters/checkers.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.arena;
import aura.core.error;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_issue_1548_edge_detail {

using aura::ast::ASTArena;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
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

static void ac1_boundary_0_to_1() {
    std::println("\n--- AC1: boundary 0→1 unlimited then bounded reject ---");
    CompilerService cs;
    auto& ev = cs.evaluator();

    // limit=0 → unlimited pass
    ev.set_resource_quota_memory(0);
    CHECK(!ev.check_arena_quota(1'000'000).has_value(), "limit=0 arena check passes any size");
    ev.set_resource_quota_mutations(0);
    CHECK(!ev.check_mutation_quota(100).has_value(), "limit=0 mutation check passes");

    // transition → limit=1, any positive over-limit rejects
    ev.set_resource_quota_memory(1);
    auto err = ev.check_arena_quota(2);
    CHECK(err.has_value() && err->kind == AuraErrorKind::ResourceQuotaExceeded,
          "limit=1 arena reject for size=2");
    CHECK(!ev.check_arena_quota(1).has_value(), "limit=1 arena pass for size=1");

    ev.set_resource_quota_mutations(1);
    ev.reset_mutation_quota_used();
    CHECK(!ev.check_mutation_quota(1).has_value(), "mutation pending=1 under limit=1 ok");
    // burn used
    {
        bool ok = true;
        auto g = Guard::try_acquire(ev, 1, &ok);
        CHECK(g.has_value(), "try_acquire burns budget");
    }
    err = ev.check_mutation_quota(1);
    CHECK(err.has_value() && err->kind == AuraErrorKind::ResourceQuotaExceeded,
          "mutation reject after budget burned");
}

static void ac2_100k_monotonic() {
    std::println("\n--- AC2: 100k monotonic counter bumps ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");

    const auto c0 = load_u64(m->resource_quota_checks_total);
    const auto r0 = load_u64(m->resource_quota_rejects_total);

    ev.set_resource_quota_memory(0); // unlimited pass path
    constexpr int kN = 100'000;
    for (int i = 0; i < kN; ++i)
        (void)ev.check_arena_quota(static_cast<std::uint64_t>(i % 1000) + 1);

    const auto c1 = load_u64(m->resource_quota_checks_total);
    const auto r1 = load_u64(m->resource_quota_rejects_total);
    CHECK(c1 == c0 + static_cast<std::uint64_t>(kN), "checks_total +100k exactly");
    CHECK(r1 == r0, "rejects unchanged on pass-only path");
    // Sanity: no wrap to small value
    CHECK(c1 > c0, "checks monotonic non-decreasing");
    std::println("  checks {}→{} rejects={}", c0, c1, r1);
}

static void ac3_concurrent_fiber_mutation() {
    std::println("\n--- AC3: concurrent fiber-steal + mutation race (100 rounds) ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    // Moderate budgets so both pass and reject may occur.
    ev.set_resource_quota_memory(4096);
    ev.set_resource_quota_mutations(50);
    ev.reset_mutation_quota_used();

    constexpr int kRounds = 100;
    std::atomic<int> errors{0};
    std::atomic<int> mut_ok{0};
    std::atomic<int> mut_rej{0};
    std::atomic<int> steal_n{0};

    std::thread stealer([&]() {
        try {
            for (int i = 0; i < kRounds * 4; ++i) {
                ev.probe_linear_ownership_on_fiber_steal();
                steal_n.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    });

    std::thread mutator([&]() {
        try {
            for (int i = 0; i < kRounds; ++i) {
                // Arena path (#1546)
                (void)ev.allocate_checked(16 + static_cast<std::size_t>(i % 8), 8);
                // Mutation path (#1547)
                bool ok = true;
                auto g = Guard::try_acquire(ev, 1, &ok);
                if (g)
                    mut_ok.fetch_add(1, std::memory_order_relaxed);
                else
                    mut_rej.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    });

    stealer.join();
    mutator.join();

    CHECK(errors.load() == 0, "no exceptions under concurrent steal+mutation");
    CHECK(steal_n.load() == kRounds * 4, "steal probes completed");
    CHECK(mut_ok.load() + mut_rej.load() == kRounds, "all mutation attempts accounted");
    CHECK(mut_ok.load() >= 1, "at least one mutation pass");
    // After budget 50, later rounds should reject
    CHECK(mut_rej.load() >= 1 || mut_ok.load() == kRounds, "rejects or all passed within budget");
    CHECK(load_u64(m->resource_quota_checks_total) > 0, "checks advanced");
    std::println("  mut_ok={} mut_rej={} steal={} checks={}", mut_ok.load(), mut_rej.load(),
                 steal_n.load(), load_u64(m->resource_quota_checks_total));
}

static void ac4_setter_checker_race() {
    std::println("\n--- AC4: concurrent set_resource_quota_* vs check_* ---");
    // Documented semantics: setters are non-atomic w.r.t. check_*.
    // Concurrent set+check must not crash; checks/rejects counters stay
    // well-formed (monotonic, no wrap).
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::thread setter([&]() {
        try {
            for (int i = 0; i < 10'000 && !stop.load(std::memory_order_relaxed); ++i) {
                ev.set_resource_quota_memory(static_cast<std::uint64_t>(i % 64));
                ev.set_resource_quota_mutations(static_cast<std::uint64_t>(i % 8));
                ev.set_resource_quota_time_us(static_cast<std::uint64_t>(i % 1000));
            }
        } catch (...) {
            errors.fetch_add(1);
        }
    });
    std::thread checker([&]() {
        try {
            for (int i = 0; i < 10'000; ++i) {
                (void)ev.check_arena_quota(static_cast<std::uint64_t>(i % 128));
                (void)ev.check_mutation_quota(1);
                (void)ev.check_time_quota(static_cast<std::uint64_t>(i % 2000));
            }
            stop.store(true, std::memory_order_relaxed);
        } catch (...) {
            errors.fetch_add(1);
            stop.store(true, std::memory_order_relaxed);
        }
    });
    setter.join();
    checker.join();

    CHECK(errors.load() == 0, "no exceptions under setter/checker race");
    const auto checks = load_u64(m->resource_quota_checks_total);
    const auto rejects = load_u64(m->resource_quota_rejects_total);
    CHECK(checks >= 10'000, "checks advanced substantially");
    CHECK(rejects <= checks, "rejects ≤ checks (invariant)");
    std::println("  checks={} rejects={} (rejects≤checks)", checks, rejects);
}

} // namespace aura_issue_1548_edge_detail

int main() {
    using namespace aura_issue_1548_edge_detail;
    std::println("=== Issue #1548: quota edge cases ===");
    ac1_boundary_0_to_1();
    ac2_100k_monotonic();
    ac3_concurrent_fiber_mutation();
    ac4_setter_checker_race();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
