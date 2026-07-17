// @category: unit
// @reason: Issue #1579 — dedicated ResourceQuota module: multi-dimension
// quotas, atomic check_and_consume, fiber RAII, overflow, process quota
// for scheduler spawn, concurrent setter/checker.
//
//   AC1: module exports ResourceQuota + phase constants
//   AC2: multi-dimension limits + check_and_consume / release
//   AC3: try_reserve_fiber RAII + process_resource_quota
//   AC4: overflow guard (uint64 max + 1)
//   AC5: concurrent consume/release races (monotonic counters)
//   AC6: evaluator check_fiber_quota mirrors process fibers limit
//   AC7: query:resource-quota-stats schema 1579 + process fields

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/resource_quota.hh"

#include <atomic>
#include <cstdint>
#include <limits>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.error;
import aura.core.resource_quota;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::core::AuraErrorKind;
using aura::core::resource_quota::Dimension;
using aura::core::resource_quota::process_resource_quota;
using aura::core::resource_quota::reset_process_resource_quota_for_test;
using aura::core::resource_quota::ResourceQuota;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_module_surface() {
    std::println("\n--- AC1: module surface ---");
    CHECK(aura::core::resource_quota::kResourceQuotaPhase >= 1, "phase >= 1");
    CHECK(aura::core::resource_quota::kResourceQuotaIssue == 1579, "issue 1579");
    ResourceQuota q;
    CHECK(q.limit(Dimension::Memory) == 0, "default unlimited memory");
    CHECK(q.limit(Dimension::Fibers) == 0, "default unlimited fibers");
}

static void ac2_multi_dimension_consume() {
    std::println("\n--- AC2: multi-dimension check_and_consume ---");
    ResourceQuota q;
    q.set_limit(Dimension::Memory, 1000);
    q.set_limit(Dimension::Mutations, 5);
    CHECK(!q.check_and_consume(Dimension::Memory, 400).has_value(), "mem 400 ok");
    CHECK(!q.check_and_consume(Dimension::Memory, 400).has_value(), "mem 800 ok");
    auto err = q.check_and_consume(Dimension::Memory, 400);
    CHECK(err.has_value(), "mem 1200 reject");
    CHECK(q.used(Dimension::Memory) == 800, "used stays 800 after reject");
    q.release(Dimension::Memory, 300);
    CHECK(q.used(Dimension::Memory) == 500, "release works");

    for (int i = 0; i < 5; ++i)
        CHECK(!q.check_and_consume(Dimension::Mutations, 1).has_value(), "mutation consume");
    CHECK(q.check_and_consume(Dimension::Mutations, 1).has_value(), "mutation over reject");
    CHECK(q.rejects_total.load() >= 2, "rejects counted");
}

static void ac3_fiber_token_and_process() {
    std::println("\n--- AC3: fiber token + process quota ---");
    reset_process_resource_quota_for_test();
    auto& pq = process_resource_quota();
    pq.set_limit(Dimension::Fibers, 2);

    auto t1 = pq.try_reserve_fiber();
    auto t2 = pq.try_reserve_fiber();
    CHECK(t1.has_value() && t2.has_value(), "reserve 2 fibers");
    CHECK(pq.used(Dimension::Fibers) == 2, "used == 2");
    auto t3 = pq.try_reserve_fiber();
    CHECK(!t3.has_value(), "3rd fiber rejected");
    t1->reset();
    CHECK(pq.used(Dimension::Fibers) == 1, "release via token");
    auto t4 = pq.try_reserve_fiber();
    CHECK(t4.has_value(), "reserve after release");
    // leave tokens to auto-release
}

static void ac4_overflow_guard() {
    std::println("\n--- AC4: overflow guard ---");
    ResourceQuota q;
    q.set_limit(Dimension::Memory, std::numeric_limits<std::uint64_t>::max());
    // Force used near max via unlimited path then set tight... simpler:
    // unlimited consume then check with huge amount after setting used manually
    q.memory_used.store(std::numeric_limits<std::uint64_t>::max() - 5, std::memory_order_relaxed);
    q.set_limit(Dimension::Memory, std::numeric_limits<std::uint64_t>::max());
    auto err = q.check_and_consume(Dimension::Memory, 100);
    CHECK(err.has_value(), "overflow guard rejects");
    CHECK(q.overflow_guards_total.load() >= 1, "overflow_guards_total++");
}

static void ac5_concurrent_race() {
    std::println("\n--- AC5: concurrent consume/release ---");
    ResourceQuota q;
    q.set_limit(Dimension::Mutations, 10000);
    std::atomic<int> ok{0};
    std::atomic<int> rej{0};
    auto worker = [&] {
        for (int i = 0; i < 2000; ++i) {
            if (!q.check_and_consume(Dimension::Mutations, 1)) {
                ok.fetch_add(1, std::memory_order_relaxed);
                q.release(Dimension::Mutations, 1);
            } else {
                rej.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    std::thread t1(worker), t2(worker), t3(worker), t4(worker);
    t1.join();
    t2.join();
    t3.join();
    t4.join();
    CHECK(ok.load() + rej.load() == 8000, "all attempts accounted");
    CHECK(q.checks_total.load() >= 8000, "checks monotonic");
    CHECK(q.used(Dimension::Mutations) <= 10000, "used within limit");
}

static void ac6_evaluator_fiber_quota() {
    std::println("\n--- AC6: evaluator check_fiber_quota ---");
    reset_process_resource_quota_for_test();
    aura::compiler::Evaluator ev;
    aura::compiler::CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    CHECK(!ev.check_fiber_quota().has_value(), "unlimited fibers pass");
    ev.set_resource_quota_fibers(1);
    // Simulate one live fiber on process quota
    process_resource_quota().check_and_consume(Dimension::Fibers, 1);
    auto err = ev.check_fiber_quota();
    CHECK(err.has_value() && err->kind == AuraErrorKind::ResourceQuotaExceeded,
          "fiber quota reject when used >= limit");
    process_resource_quota().release(Dimension::Fibers, 1);
    CHECK(!ev.check_fiber_quota().has_value(), "pass after release");
}

static void ac7_stats_primitive() {
    std::println("\n--- AC7: query:resource-quota-stats schema 1579 ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:resource-quota-stats\")");
    CHECK(r.has_value() && aura::compiler::types::is_hash(*r), "hash");
    auto schema = cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") 'schema)");
    // #1590 bumped schema to 1590; accept 1579 for older agents.
    CHECK(schema.has_value() && aura::compiler::types::is_int(*schema) &&
              (aura::compiler::types::as_int(*schema) == 1590 ||
               aura::compiler::types::as_int(*schema) == 1579),
          "schema == 1590 or 1579");
    auto phase =
        cs.eval("(hash-ref (engine:metrics \"query:resource-quota-stats\") 'module_phase)");
    CHECK(phase.has_value() && aura::compiler::types::is_int(*phase) &&
              aura::compiler::types::as_int(*phase) >= 1,
          "module_phase >= 1");
}

} // namespace

int main() {
    std::println("=== test_resource_quota_module (#1579) ===");
    ac1_module_surface();
    ac2_multi_dimension_consume();
    ac3_fiber_token_and_process();
    ac4_overflow_guard();
    ac5_concurrent_race();
    ac6_evaluator_fiber_quota();
    ac7_stats_primitive();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
