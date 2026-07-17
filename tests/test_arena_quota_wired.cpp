// @category: unit
// @reason: Issue #1546 — Arena::allocate_raw ↔ check_arena_quota wiring
//
//   AC1: set_arena installs arena_owner_ + quota callback
//   AC2: allocate_checked over limit → ResourceQuotaExceeded; rejects+1; no used bump
//   AC3: try_allocate over limit → nullptr (allocate_raw gate)
//   AC4: orphan arena (no owner) still allocates large sizes
//   AC5: under-limit allocate_checked succeeds when arena set
//   AC6: create<T> over limit → nullptr (quota-bound family)

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <print>

import std;
import aura.core.arena;
import aura.core.error;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace aura_arena_quota_wired_detail {

using aura::ast::ASTArena;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::core::AuraErrorKind;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static void ac1_set_arena_installs_owner() {
    std::println("\n--- AC1: set_arena installs arena_owner ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(64 * 1024);
    CHECK(!arena.has_arena_owner(), "fresh arena has no owner");
    ev.set_arena(&arena);
    CHECK(arena.has_arena_owner(), "set_arena installs owner");
    CHECK(arena.arena_owner() == static_cast<void*>(&ev), "owner is Evaluator*");
}

static void ac2_allocate_checked_quota_reject() {
    std::println("\n--- AC2: allocate_checked over limit → ResourceQuotaExceeded ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(64); // max 64 bytes per request

    const auto used0 = arena.stats().used;
    const auto rejects0 = load_u64(m->resource_quota_rejects_total);
    const auto checks0 = load_u64(m->resource_quota_checks_total);

    auto r = ev.allocate_checked(/*size=*/1024, /*align=*/8);
    CHECK(!r.has_value(), "allocate_checked over quota fails");
    if (!r) {
        CHECK(r.error().kind == AuraErrorKind::ResourceQuotaExceeded,
              "kind == ResourceQuotaExceeded");
    }
    CHECK(load_u64(m->resource_quota_rejects_total) == rejects0 + 1, "rejects_total +1");
    CHECK(load_u64(m->resource_quota_checks_total) > checks0, "checks_total advanced");
    CHECK(arena.stats().used == used0, "no allocation (used unchanged)");
}

static void ac3_try_allocate_quota_gate() {
    std::println("\n--- AC3: try_allocate over limit → nullptr ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(32);

    const auto used0 = arena.stats().used;
    const auto rejects0 = load_u64(m->resource_quota_rejects_total);

    void* p = arena.try_allocate(4096);
    CHECK(p == nullptr, "try_allocate over quota → nullptr");
    CHECK(load_u64(m->resource_quota_rejects_total) == rejects0 + 1,
          "allocate_raw path bumps rejects");
    CHECK(arena.stats().used == used0, "used unchanged after reject");
}

static void ac4_orphan_unlimited() {
    std::println("\n--- AC4: orphan arena (no owner) allocates large ---");
    ASTArena orphan(256 * 1024);
    CHECK(!orphan.has_arena_owner(), "orphan has no owner");
    void* p = orphan.try_allocate(8192);
    CHECK(p != nullptr, "orphan try_allocate large succeeds");
    CHECK(orphan.stats().used >= 8192, "orphan used advanced");
}

static void ac5_under_limit_succeeds() {
    std::println("\n--- AC5: under-limit allocate_checked succeeds ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(4096);

    auto r = ev.allocate_checked(/*size=*/128, /*align=*/8);
    CHECK(r.has_value(), "under-limit allocate_checked ok");
    if (r)
        CHECK(*r != nullptr, "pointer non-null");
    CHECK(arena.stats().used >= 128, "used advanced on success");
}

static void ac6_create_over_limit() {
    std::println("\n--- AC6: create<T> over limit → nullptr ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ASTArena arena(256 * 1024);
    ev.set_arena(&arena);
    ev.set_resource_quota_memory(8); // smaller than sizeof(std::uint64_t)*N payload

    // Large POD to force size > 8.
    struct Big {
        char buf[256];
    };
    const auto used0 = arena.stats().used;
    Big* b = arena.create<Big>();
    CHECK(b == nullptr, "create<Big> over quota → nullptr");
    CHECK(arena.stats().used == used0, "create reject: used unchanged");
}

} // namespace aura_arena_quota_wired_detail

int main() {
    using namespace aura_arena_quota_wired_detail;
    std::println("=== Issue #1546: arena allocate_raw quota wiring ===");
    ac1_set_arena_installs_owner();
    ac2_allocate_checked_quota_reject();
    ac3_try_allocate_quota_gate();
    ac4_orphan_unlimited();
    ac5_under_limit_succeeds();
    ac6_create_over_limit();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
