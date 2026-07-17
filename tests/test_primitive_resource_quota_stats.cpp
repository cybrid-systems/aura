// @category: unit
// @reason: Issue #1548 — Aura-side coverage for query:resource-quota-stats (#1481)
//
//   AC1: primitive returns hash with 5 integer fields (incl. schema)
//   AC2: checks_total matches metrics after bounded check_arena_quota calls
//   AC3: rejects_total matches metrics after a reject
//   AC4: schema == 1481
//
// Note: set_resource_quota_memory(N>0) gates ALL ASTArena::allocate_raw via
// arena_owner_ (#1546). Always restore limit=0 before CompilerService::eval
// so parse/create is not OOM-null crashed.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_1548_prim_detail {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static constexpr const char* kPrim = "(engine:metrics \"query:resource-quota-stats\")";

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static std::int64_t hash_int_field(CompilerService& cs, std::string_view key) {
    // Ensure unlimited arena for eval parse.
    cs.evaluator().set_resource_quota_memory(0);
    auto r = cs.eval(std::format("(hash-ref {} '{}')", kPrim, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_primitive_shape(CompilerService& cs) {
    std::println("\n--- AC1: query:resource-quota-stats hash shape (5 int fields) ---");
    auto r = cs.eval(kPrim);
    CHECK(r && is_hash(*r), "(engine:metrics \"query:resource-quota-stats\") returns a hash");
    for (const auto& k : std::vector<std::string>{"checks_total", "rejects_total", "max_fibers",
                                                  "max_mutations", "schema"}) {
        auto f = cs.eval(std::format("(hash-ref {} '{}')", kPrim, k));
        CHECK(f && is_int(*f), std::format("field '{}' present as int", k));
    }
}

static void ac2_checks_total_matches(CompilerService& cs) {
    std::println("\n--- AC2: checks_total matches metrics after bounded checks ---");
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics available");

    const auto c0 = load_u64(m->resource_quota_checks_total);
    ev.set_resource_quota_memory(1024);
    (void)ev.check_arena_quota(100);
    (void)ev.check_arena_quota(200);
    (void)ev.check_time_quota(0);
    const auto after_checks = load_u64(m->resource_quota_checks_total);
    CHECK(after_checks >= c0 + 3, "at least 3 new checks on metrics");
    // Restore unlimited before Aura eval (arena_owner gates allocate_raw).
    ev.set_resource_quota_memory(0);

    // engine:metrics itself may bump checks (parse allocate_raw → quota
    // allow_fn). Primitive must mirror metrics *at query time*.
    const auto via_prim = hash_int_field(cs, "checks_total");
    const auto now = load_u64(m->resource_quota_checks_total);
    CHECK(via_prim == static_cast<std::int64_t>(now),
          std::format("checks_total prim={} metrics_now={}", via_prim, now));
    CHECK(now >= after_checks, "checks monotonic through engine:metrics");
}

static void ac3_rejects_total_matches(CompilerService& cs) {
    std::println("\n--- AC3: rejects_total matches after reject ---");
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    CHECK(m != nullptr, "metrics");

    ev.set_resource_quota_memory(64);
    (void)ev.check_arena_quota(4096); // reject
    const auto expect = load_u64(m->resource_quota_rejects_total);
    ev.set_resource_quota_memory(0);

    CHECK(expect >= 1, "metrics rejects >= 1");
    const auto via_prim = hash_int_field(cs, "rejects_total");
    CHECK(via_prim == static_cast<std::int64_t>(expect),
          std::format("rejects_total prim={} metrics={}", via_prim, expect));
}

static void ac4_schema_1481(CompilerService& cs) {
    std::println("\n--- AC4: schema == 1498 (legacy 1481 ok) ---");
    const auto schema = hash_int_field(cs, "schema");
    CHECK(schema == 1498 || schema == 1481, std::format("schema == 1498 or 1481 (got {})", schema));
}

} // namespace aura_issue_1548_prim_detail

int main() {
    using namespace aura_issue_1548_prim_detail;
    std::println("=== Issue #1548: query:resource-quota-stats primitive ===");
    CompilerService cs;
    ac1_primitive_shape(cs);
    ac2_checks_total_matches(cs);
    ac3_rejects_total_matches(cs);
    ac4_schema_1481(cs);
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
