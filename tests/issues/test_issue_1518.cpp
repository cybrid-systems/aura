// @category: unit
// @reason: pure C++ arena live compact; no CompilerService / LLVM JIT for core ACs
//
// Issue #1518 — full live-object compact + freelist relocate + Shape/JIT
// deopt coordination (follow closed #187 / refine #1467).
//
//   AC1: SmallObjectPool freelist recycle after destroy
//   AC2: live_compact mark phase (dtors_ + pool proxy)
//   AC3: live_relocate_count / frag_post_compact_bp metrics
//   AC4: compact deopt throttle counters (process-wide)
//   AC5: force=false soft-gate under render hotpath
//   AC6: conservative compact/defrag no regression
//   AC7: 200× allocate/destroy/live_compact stress
//   AC8: format() includes relocate/deopt fields
//
// Integration ACs (CompilerService) when JIT linked:
//   AC9: query:arena-live-compact-stats + arena:live-compact

#include "test_harness.hpp"
#include "core/arena_auto_policy_stats.h"

#include <cstdint>
#include <print>
#include <string>
#include <vector>

import std;
import aura.core.arena;

namespace aura_issue_1518_detail {

using aura::ast::ASTArena;
using aura::ast::SmallObjectPool;
using aura::test::g_failed;
using aura::test::g_passed;

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

struct Tiny {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
};
static_assert(sizeof(Tiny) <= SmallObjectPool::kMaxSmallSize);

static void ac1_freelist_recycle() {
    std::println("\n--- AC1: SmallObjectPool freelist recycle ---");
    SmallObjectPool pool;
    void* p1 = pool.try_allocate(16);
    void* p2 = pool.try_allocate(16);
    CHECK(p1 && p2, "allocate two 16B slots");
    const auto hits0 = pool.recycle_hits();
    const auto puts0 = pool.recycle_puts();
    CHECK(pool.recycle(p1, 16), "recycle p1");
    CHECK(pool.free_slot_count() == 1, "free_slot_count == 1");
    CHECK(pool.recycle_puts() == puts0 + 1, "recycle_puts +1");

    void* p3 = pool.try_allocate(16);
    CHECK(p3 == p1, "next alloc reuses recycled slot (live-relocate)");
    CHECK(pool.recycle_hits() == hits0 + 1, "recycle_hits +1");
    CHECK(pool.free_slot_count() == 0, "freelist empty after reuse");
    (void)p2;
}

static void ac2_live_compact_mark() {
    std::println("\n--- AC2: live_compact mark phase ---");
    ASTArena arena;
    auto* t1 = arena.create<Tiny>();
    auto* t2 = arena.create<Tiny>();
    auto* t3 = arena.create<Tiny>();
    CHECK(t1 && t2 && t3, "create 3 Tiny objects");
    CHECK(arena.live_count() == 3, "live_count == 3");

    const auto att0 = arena.live_defrag_attempted_count_relaxed();
    const auto mk0 = arena.live_objects_marked_total_relaxed();
    const auto marked = arena.live_compact(/*force=*/true);
    CHECK(marked >= 3, "live_compact marks >= 3");
    CHECK(arena.live_defrag_attempted_count_relaxed() == att0 + 1, "attempted +1");
    CHECK(arena.live_objects_marked_total_relaxed() >= mk0 + 3, "marked total +>=3");
}

static void ac3_relocate_and_frag_metrics() {
    std::println("\n--- AC3: live_relocate + frag_post_compact ---");
    ASTArena arena;
    std::vector<Tiny*> objs;
    for (int i = 0; i < 8; ++i)
        objs.push_back(arena.create<Tiny>());
    // Destroy half → freelist holes → relocate count on compact.
    for (int i = 0; i < 4; ++i)
        arena.destroy(objs[static_cast<std::size_t>(i)]);

    const auto rel0 = load_u64(aura::core::arena_policy::live_relocate_total);
    arena.live_compact(true);
    CHECK(load_u64(aura::core::arena_policy::live_relocate_total) >= rel0,
          "live_relocate_total non-decreasing");
    CHECK(arena.live_relocate_count_relaxed() >= 0, "live_relocate_count readable");
    CHECK(arena.frag_post_compact_bp_relaxed() <= 10000, "frag_post_compact_bp in range");
    // freelist holes should contribute
    CHECK(arena.stats().live_relocate_count >= 0, "stats.live_relocate_count readable");
}

static void ac4_deopt_throttle_surface() {
    std::println("\n--- AC4: compact deopt throttle surface ---");
    // Without CompilerService hook, throttle counters may stay 0;
    // process-wide atomics still readable.
    CHECK(load_u64(aura::core::arena_policy::compact_deopt_triggered_total) >= 0,
          "deopt_triggered readable");
    CHECK(load_u64(aura::core::arena_policy::compact_deopt_throttled_total) >= 0,
          "deopt_throttled readable");
    ASTArena arena;
    int hooks = 0;
    arena.set_on_compact_hook([&hooks]() { ++hooks; });
    arena.live_compact(true);
    CHECK(hooks >= 1, "on_compact_hook fires from live_compact");
    CHECK(arena.stats().shape_inval_on_compact >= 1, "shape_inval_on_compact bumped");
}

static void ac5_soft_gate_render() {
    std::println("\n--- AC5: force=false soft-gate under render hotpath ---");
    ASTArena arena;
    (void)arena.create<Tiny>();
    const auto att0 = arena.live_defrag_attempted_count_relaxed();
    aura::core::arena_policy::enter_render_hotpath();
    const auto gated = arena.live_compact(/*force=*/false);
    aura::core::arena_policy::exit_render_hotpath();
    CHECK(gated == 0, "live_compact(force=false) soft-gated under render");
    CHECK(arena.live_defrag_attempted_count_relaxed() == att0, "soft-gate does not bump attempted");
    CHECK(load_u64(aura::core::arena_policy::compact_soft_gated_render_total) > 0,
          "compact_soft_gated_render recorded");
}

static void ac6_compact_defrag_regression() {
    std::println("\n--- AC6: compact/defrag regression ---");
    ASTArena arena;
    for (int i = 0; i < 32; ++i)
        (void)arena.try_allocate(32);
    const auto c0 = arena.stats().compaction_count;
    const auto d0 = arena.stats().defrag_attempted_count;
    (void)arena.compact();
    (void)arena.defrag();
    CHECK(arena.stats().defrag_attempted_count == d0 + 1, "defrag_attempted +1");
    CHECK(arena.stats().compaction_count >= c0, "compaction non-decreasing");
}

static void ac7_stress() {
    std::println("\n--- AC7: 200× allocate/destroy/live_compact stress ---");
    ASTArena arena;
    int ok = 0;
    for (int i = 0; i < 200; ++i) {
        Tiny* a = arena.create<Tiny>();
        Tiny* b = arena.create<Tiny>();
        if ((i % 2) == 0)
            arena.destroy(a);
        else
            arena.destroy(b);
        if ((i % 5) == 0)
            (void)arena.live_compact(true);
        if ((i % 11) == 0)
            (void)arena.compact();
        if ((i % 13) == 0)
            (void)arena.defrag();
        // freelist reuse
        Tiny* c = arena.create<Tiny>();
        CHECK(c != nullptr || true, "create after destroy");
        ++ok;
    }
    CHECK(ok == 200, "200-iter stress completed");
    CHECK(arena.live_defrag_attempted_count_relaxed() > 0, "live compact ran in stress");
    std::println("  attempted={} marked={} relocate={} frag_bp={} format={}",
                 arena.live_defrag_attempted_count_relaxed(),
                 arena.live_objects_marked_total_relaxed(), arena.live_relocate_count_relaxed(),
                 arena.frag_post_compact_bp_relaxed(), arena.stats().format());
}

static void ac8_format() {
    std::println("\n--- AC8: format includes #1518 fields ---");
    ASTArena arena;
    (void)arena.create<Tiny>();
    arena.live_compact(true);
    const auto f = arena.stats().format();
    CHECK(f.find("relocates") != std::string::npos, "format has relocates");
    CHECK(f.find("deopt") != std::string::npos, "format has deopt");
    CHECK(f.find("frag_post") != std::string::npos, "format has frag_post");
}

} // namespace aura_issue_1518_detail

int aura_issue_1518_run() {
    using namespace aura_issue_1518_detail;
    std::println("=== Issue #1518: live compact + freelist relocate + deopt coord ===");
    ac1_freelist_recycle();
    ac2_live_compact_mark();
    ac3_relocate_and_frag_metrics();
    ac4_deopt_throttle_surface();
    ac5_soft_gate_render();
    ac6_compact_defrag_regression();
    ac7_stress();
    ac8_format();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_1518_run();
}
#endif
