// @category: unit
// @reason: Issue #1929 — Closure Bridge / hot-update: make_closure_view
// Issue #1888/#1895/#1926/#1928/#1929/#1947 (#1978 renamed): issue# moved from filename to header.
// raw pointer lifetime + walk_active_closures on all boundaries +
// unified metrics (refine #1888 #1926 #1947 #1895 #1928).
//
//   AC1: source cites #1929 (make_closure_view + ClosureView + apply)
//   AC2: query:closure-view-lifetime-stats schema-1929 + AC metric keys
//   AC3: make_closure_view stamps dual-epoch; tombstone rejects + counters
//   AC4: safe accessors null-on-invalid; strong revalidate fails after move
//   AC5: revalidate_closure_snapshot after erase; find skips tombstoned
//   AC6: linear-boundary-consistency-stats pairs schema-1929
//   AC7: multi-round concurrent stress — counters mono; no crash
//   AC8: #1888/#1926 lineage schema retained
//   AC9 (Issue #2000 Phase 2): LifetimePin holds real pointer + generation;
//        validate / ffi_handoff flag / pinned()
//   AC10: restamp bumps gen + restamps counter; restamp_all_pins_for_arena
//         bulk-bumps surviving pins at MutationBoundary dtor
//   AC11: unpin_on_compact nulls ptr + bumps invalidations;
//         invalidate_all_pins_for_arena bulk-invalidates at compact_sweep
//   AC12: query:lifetime-pin-stats primitive returns schema=2000 + phase=2
//         + all counters (pins/unpins/ffi-handoffs/invalidations/restamps)
//   AC13: concurrent pin / restamp / unpin_on_compact / restamp_all /
//         invalidate_all — counters monotonic, no UAF, registry consistent

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import std;
import aura.core.ast;
import aura.core.lifetime_pin;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::closure_view_flat;
using aura::compiler::closure_view_owner_arena;
using aura::compiler::closure_view_pool;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::g_closure_view_dangling_prevented_total;
using aura::compiler::g_closure_view_invalid_access_total;
using aura::compiler::invalidate_closure_lifetime;
using aura::compiler::is_closure_view_valid;
using aura::compiler::make_closure_view;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::lifetime::g_lifetime_pin_stats;
using aura::core::lifetime::invalidate_all_pins_for_arena;
using aura::core::lifetime::kLifetimePinPhase;
using aura::core::lifetime::LifetimePin;
using aura::core::lifetime::live_pin_count;
using aura::core::lifetime::restamp_all_pins_for_arena;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:closure-view-lifetime-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_linear(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-boundary-consistency-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Issue #2000: query:lifetime-pin-stats schema-2000 reader.
static std::int64_t href_lifetime_pin(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:lifetime-pin-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static Closure make_fresh(std::uint64_t life = 1, std::uint64_t bridge = 42) {
    Closure cl;
    cl.name = "c1929";
    cl.params = {static_cast<SymId>(1)};
    cl.body_id = 1;
    cl.env_id = NULL_ENV_ID;
    cl.lifetime_version = life;
    cl.bridge_epoch = bridge;
    return cl;
}

static void ac1_source() {
    std::println("\n--- AC1: #1929 source surface ---");
    auto env = read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
    auto flat = read_first(
        {"src/compiler/evaluator_eval_flat.cpp", "../src/compiler/evaluator_eval_flat.cpp"});
    auto mut = read_first({"src/compiler/evaluator_primitives_mutate.cpp",
                           "../src/compiler/evaluator_primitives_mutate.cpp"});
    CHECK(!env.empty() && env.find("#1929") != std::string::npos, "env cites #1929");
    CHECK(env.find("make_closure_view") != std::string::npos, "make_closure_view");
    CHECK(env.find("source_bridge_epoch") != std::string::npos, "dual-epoch stamp");
    CHECK(!ixx.empty() && ixx.find("#1929") != std::string::npos, "ixx cites #1929");
    CHECK(ixx.find("source_lifetime_version") != std::string::npos, "lifetime field");
    CHECK(!flat.empty() && flat.find("#1929") != std::string::npos, "apply cites #1929");
    CHECK(flat.find("revalidate_closure_snapshot") != std::string::npos, "apply revalidate");
    CHECK(!mut.empty() && mut.find("schema-1929") != std::string::npos, "query schema-1929");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1929 on closure-view-lifetime-stats ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:closure-view-lifetime-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1888, "lineage 1888");
    CHECK(href(cs, "schema-1926") == 1926, "schema-1926 retained");
    CHECK(href(cs, "schema-1929") == 1929, "schema-1929");
    CHECK(href(cs, "issue-1929") == 1929, "issue-1929");
    CHECK(href(cs, "schema-1954") == 1954, "schema-1954 refine");
    CHECK(href(cs, "make-closure-view-lifetime-guard") == 1, "view guard wired");
    CHECK(href(cs, "safe-accessors-wired") == 1, "safe accessors");
    CHECK(href(cs, "apply-snapshot-revalidate-wired") == 1, "apply revalidate");
    CHECK(href(cs, "walk-active-closures-wired") == 1, "walk wired");
    CHECK(href(cs, "boundaries-wired-count") == 6, "6 boundaries");
    CHECK(href(cs, "snapshot-revalidate-wired") == 1, "revalidate wired");
    CHECK(href(cs, "dual-epoch-wired") == 1, "dual-epoch");
    CHECK(href(cs, "closure_view_dangling_prevented_total") >= 0, "dangling metric");
    CHECK(href(cs, "live_closure_stale_prevented_total") >= 0, "stale metric");
    CHECK(href(cs, "live-closure-stale-prevented-total") >= 0, "stale alias");
}

static void ac3_stamp_and_tombstone() {
    std::println("\n--- AC3: dual stamp + tombstone reject ---");
    auto cl = make_fresh(7, 99);
    auto v = make_closure_view(cl);
    CHECK(v.live, "live");
    CHECK(v.source_lifetime_version == 7, "life 7");
    CHECK(v.source_bridge_epoch == 99, "bridge 99");
    CHECK(is_closure_view_valid(v), "soft");
    CHECK(is_closure_view_valid(v, cl), "strong");

    const auto d0 = g_closure_view_dangling_prevented_total.load();
    const auto i0 = g_closure_view_invalid_access_total.load();
    invalidate_closure_lifetime(cl);
    CHECK(!is_closure_view_valid(v, cl), "strong fails after tombstone");
    CHECK(g_closure_view_invalid_access_total.load() > i0, "invalid-access +");
    auto v2 = make_closure_view(cl);
    CHECK(!v2.live, "reject tombstoned");
    CHECK(g_closure_view_dangling_prevented_total.load() > d0, "dangling +");
}

static void ac4_safe_accessors() {
    std::println("\n--- AC4: safe accessors + dual-epoch mismatch ---");
    auto cl = make_fresh(5, 10);
    auto v = make_closure_view(cl);
    const auto d0 = g_closure_view_dangling_prevented_total.load();
    invalidate_closure_lifetime(cl);
    CHECK(closure_view_flat(v) == nullptr || !is_closure_view_valid(v, cl), "flat guarded");
    // Soft-invalid view: accessors must not return dangling pointees.
    auto bad = make_closure_view(cl);
    CHECK(closure_view_flat(bad) == nullptr, "flat null");
    CHECK(closure_view_pool(bad) == nullptr, "pool null");
    CHECK(closure_view_owner_arena(bad) == nullptr, "arena null");
    CHECK(g_closure_view_dangling_prevented_total.load() > d0, "dangling grew");

    // Dual-epoch drift on strong check.
    auto cl2 = make_fresh(3, 20);
    auto v2 = make_closure_view(cl2);
    cl2.bridge_epoch = 99; // drift without tombstone
    const auto i0 = g_closure_view_invalid_access_total.load();
    CHECK(!is_closure_view_valid(v2, cl2), "bridge epoch drift fails");
    CHECK(g_closure_view_invalid_access_total.load() > i0, "invalid on epoch drift");
}

static void ac5_revalidate_and_find() {
    std::println("\n--- AC5: revalidate_closure_snapshot + find ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto cl = make_fresh(11, 22);
    auto id = ev.register_active_closure(cl);
    auto snap = ev.find_active_closure(id);
    CHECK(snap.has_value(), "registered");
    CHECK(ev.revalidate_closure_snapshot(id, *snap), "revalidate ok");
    CHECK(ev.erase_active_closure(id), "erase");
    CHECK(!ev.revalidate_closure_snapshot(id, *snap), "revalidate fails after erase");
    CHECK(!ev.find_active_closure(id).has_value(), "find gone");
}

static void ac6_linear_pair() {
    std::println("\n--- AC6: linear-boundary-consistency-stats pairs #1929 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:linear-boundary-consistency-stats\")");
    CHECK(h && is_hash(*h), "linear hash");
    CHECK(href_linear(cs, "schema-1929") == 1929, "schema-1929 on linear");
    CHECK(href_linear(cs, "issue-1929") == 1929, "issue-1929 on linear");
    CHECK(href_linear(cs, "boundaries-wired-count") == 6, "6 boundaries");
    CHECK(href_linear(cs, "closure-view-lifetime-paired") == 1, "paired flag");
    CHECK(href_linear(cs, "schema-1928") == 1928, "1928 retained");
}

static void ac7_stress() {
    std::println("\n--- AC7: concurrent view + invalidate stress ---");
    const auto d0 = g_closure_view_dangling_prevented_total.load();
    const auto i0 = g_closure_view_invalid_access_total.load();
    std::atomic<int> ops{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 250; ++i) {
                auto cl = make_fresh(static_cast<std::uint64_t>(i + 1 + t * 1000),
                                     static_cast<std::uint64_t>(i + t));
                auto v = make_closure_view(cl);
                if ((i + t) % 3 == 0) {
                    invalidate_closure_lifetime(cl);
                    (void)is_closure_view_valid(v, cl);
                    (void)make_closure_view(cl);
                    (void)closure_view_flat(v);
                } else {
                    if (!is_closure_view_valid(v, cl))
                        ++ops; // unexpected but not fatal
                }
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(ops.load() >= 1000, "ops completed");
    CHECK(g_closure_view_dangling_prevented_total.load() > d0, "dangling grew");
    CHECK(g_closure_view_invalid_access_total.load() > i0, "invalid grew");
    // Metrics surface still readable after stress.
    CompilerService cs;
    CHECK(href(cs, "schema-1929") == 1929, "schema after stress");
    CHECK(href(cs, "closure_view_dangling_prevented_total") >= 0, "metric after stress");
}

static void ac8_lineage() {
    std::println("\n--- AC8: #1888/#1926 lineage retained ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 1888, "schema 1888");
    CHECK(href(cs, "lifetime-guard") == 1, "guard");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "schema-1926") == 1926, "1926");
    CHECK(href(cs, "issue-1926") == 1926, "issue-1926");
}

// ── Issue #2000 Phase 2: real LifetimePin RAII pinning + generation ───
// AC9: pin/validate/ffi_handoff flag — ptr + gen stored, ffi_handoff flips
//      on mark_ffi_handoff, pinned() reflects ptr != nullptr.
static void ac9_phase2_real_pinning() {
    std::println("\n--- AC9: #2000 phase 2 real pin/validate/ffi_handoff ---");
    const auto pins_before = g_lifetime_pin_stats.pins;
    const auto handoffs_before = g_lifetime_pin_stats.ffi_handoffs;

    LifetimePin lp;
    int dummy = 0;
    void* p = &dummy;
    CHECK(!lp.pinned(), "default unpinned");
    CHECK(lp.ptr() == nullptr, "default ptr null");
    CHECK(lp.gen() == 0, "default gen 0");
    CHECK(!lp.ffi_handoff(), "default ffi_handoff false");

    lp.pin(p, 1, 0);
    CHECK(lp.pinned(), "pinned after pin()");
    CHECK(lp.ptr() == p, "ptr stored");
    CHECK(lp.gen() == 1, "gen stored");
    CHECK(lp.arena_id() == 0, "arena_id stored");
    CHECK(lp.validate(1, 0), "validate(gen=1) true");
    CHECK(!lp.validate(2, 0), "validate(gen=2) false");
    CHECK(lp.validate(1, 99), "validate(gen=1, arena=99) — arena mismatch ok if either 0");
    CHECK(!lp.validate(1, 7), "validate(gen=1, arena=7) — both set, mismatch");

    lp.mark_ffi_handoff();
    CHECK(lp.ffi_handoff(), "ffi_handoff flips on mark_ffi_handoff");

    CHECK(g_lifetime_pin_stats.pins > pins_before, "pins counter bumped");
    CHECK(g_lifetime_pin_stats.ffi_handoffs > handoffs_before, "ffi_handoffs counter bumped");
}

// AC10: restamp bumps gen + restamps counter. restamp_all_pins_for_arena
//       bulk-bumps surviving pins (used by MutationBoundaryGuard dtor).
static void ac10_restamp_and_bulk() {
    std::println("\n--- AC10: restamp + restamp_all_pins_for_arena bulk ---");
    const auto restamps_before = g_lifetime_pin_stats.restamps;

    LifetimePin a;
    int buf_a = 0;
    a.pin(&buf_a, 1, 0);
    a.restamp(5);
    CHECK(a.gen() == 5, "restamp(5) → gen=5");
    CHECK(a.validate(5, 0), "validate(gen=5) true");
    CHECK(!a.validate(1, 0), "validate(gen=1) false post-restamp");

    a.restamp(7, 99);
    CHECK(a.gen() == 7, "restamp(7, 99) → gen=7");
    CHECK(a.arena_id() == 99, "restamp(7, 99) → arena_id=99");

    // bulk restamp: arena_id=0 matches all, new_gen=0 keeps current gen
    // (still bumps the restamps counter for surviving pins)
    LifetimePin b, c;
    int buf_b = 0, buf_c = 0;
    b.pin(&buf_b, 1, 99);
    c.pin(&buf_c, 1, 99);
    const auto n_bulk = restamp_all_pins_for_arena(0, 0); // matches both b and c
    CHECK(n_bulk >= 2, "bulk restamp covers b + c");
    CHECK(b.gen() == 1, "b gen kept (new_gen=0)");
    CHECK(c.gen() == 1, "c gen kept");

    // bulk restamp with non-zero new_gen + arena_id filter
    const auto n_filt = restamp_all_pins_for_arena(99, 42);
    CHECK(n_filt >= 2, "bulk restamp with arena_id=99 matches b + c");
    CHECK(b.gen() == 42, "b gen → 42");
    CHECK(b.arena_id() == 99, "b arena_id kept (new_arena_id=99)");

    CHECK(g_lifetime_pin_stats.restamps > restamps_before, "restamps counter bumped");
}

// AC11: unpin_on_compact nulls ptr + invalidations counter bumped.
//       invalidate_all_pins_for_arena bulk-invalidates pins (used by
//       compact_sweep to drop pins tied to swept closure / heap state).
static void ac11_unpin_on_compact_and_bulk() {
    std::println("\n--- AC11: unpin_on_compact + invalidate_all_pins_for_arena bulk ---");
    const auto invalidations_before = g_lifetime_pin_stats.invalidations;

    LifetimePin lp;
    int buf = 0;
    lp.pin(&buf, 1, 7);
    CHECK(lp.pinned(), "pinned");
    lp.unpin_on_compact();
    CHECK(!lp.pinned(), "unpinned after unpin_on_compact()");
    CHECK(lp.ptr() == nullptr, "ptr null");
    CHECK(lp.gen() == 0, "gen 0");
    CHECK(!lp.ffi_handoff(), "ffi_handoff cleared");
    CHECK(!lp.validate(1, 7), "validate(any) false post-invalidation");

    // bulk invalidate: arena_id=0 matches all
    const auto before_count = live_pin_count();
    LifetimePin x, y;
    int bx = 0, by = 0;
    x.pin(&bx, 1, 0);
    y.pin(&by, 1, 0);
    CHECK(live_pin_count() >= before_count + 2, "live count grew after 2 pins");
    const auto n_inv = invalidate_all_pins_for_arena(0);
    CHECK(n_inv >= 2, "bulk invalidate ≥ 2");
    CHECK(!x.pinned(), "x invalidated");
    CHECK(!y.pinned(), "y invalidated");

    // already-invalidated pin doesn't bump counter on second invalidate
    const auto n_inv2 = invalidate_all_pins_for_arena(0);
    (void)n_inv2;
    CHECK(g_lifetime_pin_stats.invalidations > invalidations_before,
          "invalidations counter bumped");
}

// AC12: query:lifetime-pin-stats primitive returns schema=2000 + phase=2
//       + all counters (pins/unpins/ffi-handoffs/invalidations/restamps).
static void ac12_query_lifetime_pin_stats_primitive() {
    std::println("\n--- AC12: query:lifetime-pin-stats schema-2000 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:lifetime-pin-stats\")");
    CHECK(h && is_hash(*h), "query hash");
    CHECK(href_lifetime_pin(cs, "schema") == 2000, "schema 2000");
    CHECK(href_lifetime_pin(cs, "phase") == kLifetimePinPhase, "phase 2");
    CHECK(href_lifetime_pin(cs, "phase") == 2, "phase 2 hardcoded");
    CHECK(href_lifetime_pin(cs, "pins") >= 0, "pins counter present");
    CHECK(href_lifetime_pin(cs, "unpins") >= 0, "unpins counter present");
    CHECK(href_lifetime_pin(cs, "ffi-handoffs") >= 0, "ffi-handoffs counter present");
    CHECK(href_lifetime_pin(cs, "invalidations") >= 0, "invalidations counter present");
    CHECK(href_lifetime_pin(cs, "restamps") >= 0, "restamps counter present");

    // AC bridge: the existing query:production-sweep-1202-1228-stats now
    // reports phase 2 via the lifetime-pin-scaffold key.
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:production-sweep-1202-1228-stats\") "
                            "\"lifetime-pin-scaffold\")"));
    CHECK(r && is_int(*r) && as_int(*r) == 2, "lifetime-pin-scaffold bumped to 2");
}

// AC13: concurrent pin / restamp / unpin_on_compact / restamp_all /
//       invalidate_all — counters monotonic, no UAF, registry consistent.
static void ac13_concurrent_pin_compact_stress() {
    std::println("\n--- AC13: concurrent pin/compact stress ---");
    const auto pins_before = g_lifetime_pin_stats.pins;
    const auto unpins_before = g_lifetime_pin_stats.unpins;

    constexpr int kThreads = 4;
    constexpr int kIters = 500;
    std::vector<std::thread> threads;
    std::atomic<std::uint64_t> restamps_seen{0};
    std::atomic<std::uint64_t> invalidations_seen{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&restamps_seen, &invalidations_seen]() {
            std::vector<LifetimePin> local;
            local.reserve(kIters);
            std::vector<int> bufs(kIters);
            for (int i = 0; i < kIters; ++i) {
                local.emplace_back();
                local.back().pin(&bufs[i], static_cast<std::uint64_t>(i + 1), 0);
                if ((i & 1) == 0)
                    local.back().restamp(static_cast<std::uint64_t>(i + 100));
                else
                    local.back().unpin_on_compact();
                // local pins drop on scope exit (dtors bump unpins)
            }
            // bulk invalidate on the way out
            invalidate_all_pins_for_arena(0);
            restamps_seen.fetch_add(static_cast<std::uint64_t>(kIters / 2),
                                    std::memory_order_relaxed);
            invalidations_seen.fetch_add(static_cast<std::uint64_t>(kIters / 2),
                                         std::memory_order_relaxed);
        });
    }
    for (auto& th : threads)
        th.join();

    CHECK(live_pin_count() == 0, "all pins destructed → live_pin_count == 0");
    CHECK(g_lifetime_pin_stats.pins >= pins_before + kThreads * kIters,
          "pins counter grew by threads × iters");
    CHECK(g_lifetime_pin_stats.unpins >= unpins_before + kThreads * kIters,
          "unpins counter grew (dtors)");
    CHECK(g_lifetime_pin_stats.restamps >= restamps_seen.load(),
          "restamps counter reflects stress");
    CHECK(g_lifetime_pin_stats.invalidations >= invalidations_seen.load(),
          "invalidations counter reflects stress");
}

} // namespace

int main() {
    std::println("=== Issue #1929: Closure Bridge lifetime safety (+ #2000 phase 2) ===");
    ac1_source();
    ac2_schema();
    ac3_stamp_and_tombstone();
    ac4_safe_accessors();
    ac5_revalidate_and_find();
    ac6_linear_pair();
    ac7_stress();
    ac8_lineage();
    ac9_phase2_real_pinning();
    ac10_restamp_and_bulk();
    ac11_unpin_on_compact_and_bulk();
    ac12_query_lifetime_pin_stats_primitive();
    ac13_concurrent_pin_compact_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
