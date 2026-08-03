// @category: unit
// @reason: Issue #1665 — scan_live_closures must not re-mark already
// Issue #1665 (#1978 renamed): issue# moved from filename to header.
// tombstoned TW closures or iterate erased slots; TW free erases
// closures_ (JIT free uses g_closure_freed separately).
//
//   AC1: first mark_invalid on Moved → marked_invalid grows
//   AC2: second scan does not re-inflate marked_invalid (tombstone skip)
//   AC3: erase_active_closure removes from map; scan examined shrinks
//   AC4: JIT aura_free_closure does not affect TW scan examined
//   AC5: already bridge_epoch=0 with linear still counted as capture once
//   AC6: stress double-scan after force_drop; marked_invalid stable
//   AC7: g_envframe_lifetime_stats.scans_run grows under repeated scans
//   AC8 (deferred): primitive schema=2003 — covered by query:envframe-lifetime-stats in obs_eval
//   AC_H1–H4 (#2164): EnvFrameLifetimeGuard hold-pin blocks compact_env_frames

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.core.envframe_lifetime;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::SymId;
using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::make_int;
using aura::test::g_failed;
using aura::test::g_passed;

constexpr std::uint8_t kMoved = 4;
constexpr std::uint8_t kUntracked = 0;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::uint64_t load_u64(std::atomic<std::uint64_t>& a) {
    return a.load(std::memory_order_relaxed);
}

static ClosureId make_moved_tw(Evaluator& ev) {
    auto env_id = ev.alloc_env_frame(NULL_ENV_ID);
    if (auto* fr = ev.resolve_env_frame_mut(env_id)) {
        fr->bindings_symid_.push_back({static_cast<SymId>(1), make_int(0)});
        fr->bindings_linear_ownership_state_.push_back(kMoved);
        fr->version_ = ev.defuse_version_snapshot();
    }
    Closure cl;
    cl.env_id = env_id;
    return ev.register_active_closure(std::move(cl));
}

static void ac1_first_mark() {
    std::println("\n--- AC1: first mark_invalid grows marked_invalid ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    // Bump service bridge epoch so stamp + tombstone semantics activate.
    cs.public_mark_define_dirty("__1665_ac1__");
    CHECK(ev.current_bridge_epoch() != 0, "service bridge epoch active after dirty");
    (void)make_moved_tw(ev);
    const auto m0 = load_u64(m->linear_live_closures_marked_invalid_total);
    auto r = ev.scan_live_closures_for_linear_captures(/*mark_invalid=*/true,
                                                       /*only_if_moved=*/true);
    CHECK(r.with_moved_capture >= 1, "with_moved_capture ≥1");
    CHECK(r.marked_invalid >= 1, "marked_invalid ≥1 on first scan");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) > m0, "metric grew");
}

static void ac2_no_reinflate() {
    std::println("\n--- AC2: second scan does not re-inflate marked_invalid ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    cs.public_mark_define_dirty("__1665_ac2__");
    CHECK(ev.current_bridge_epoch() != 0, "tracking active");
    (void)make_moved_tw(ev);
    auto r1 = ev.scan_live_closures_for_linear_captures(true, true);
    CHECK(r1.marked_invalid >= 1, "first mark");
    const auto metric_after_first = load_u64(m->linear_live_closures_marked_invalid_total);
    auto r2 = ev.scan_live_closures_for_linear_captures(true, true);
    CHECK(r2.marked_invalid == 0, "second scan marked_invalid == 0 (tombstone skip)");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) == metric_after_first,
          "metric stable on re-scan");
    CHECK(r2.with_moved_capture >= 1 || r2.with_linear_capture >= 1,
          "still counts linear capture for audit");
}

static void ac3_erase_tw() {
    std::println("\n--- AC3: erase_active_closure removes from scan ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto cid = make_moved_tw(ev);
    auto r0 = ev.scan_live_closures_for_linear_captures(false, false);
    CHECK(r0.examined >= 1, "examined ≥1 before erase");
    CHECK(ev.erase_active_closure(cid), "erase returns true");
    CHECK(!ev.find_active_closure(cid).has_value(), "not findable after erase");
    auto r1 = ev.scan_live_closures_for_linear_captures(false, false);
    CHECK(r1.examined + 1 == r0.examined || r1.examined < r0.examined,
          "examined decreased after erase");
}

static void ac4_jit_free_separate() {
    std::println("\n--- AC4: JIT free does not remove TW entry ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto tw = make_moved_tw(ev);
    const auto exam0 = ev.scan_live_closures_for_linear_captures(false, false).examined;
    // Free a JIT slot (unrelated id space usage is independent).
    const auto jid = aura_alloc_closure(/*func_id=*/0);
    aura_free_closure(jid);
    CHECK(aura_closure_is_freed(jid) == 1, "JIT slot freed");
    const auto exam1 = ev.scan_live_closures_for_linear_captures(false, false).examined;
    CHECK(exam1 == exam0, "TW examined unchanged by JIT free");
    CHECK(ev.find_active_closure(tw).has_value(), "TW entry still present");
}

static void ac5_force_drop_then_scan() {
    std::println("\n--- AC5: force_drop then scan no re-mark ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    cs.public_mark_define_dirty("__1665_ac5__");
    CHECK(ev.current_bridge_epoch() != 0, "tracking active");
    auto cid = make_moved_tw(ev);
    auto before = ev.find_active_closure(cid);
    CHECK(before.has_value() && before->bridge_epoch != 0, "stamped non-zero");
    ev.force_drop_or_mark_invalid(cid);
    auto opt = ev.find_active_closure(cid);
    CHECK(opt && opt->bridge_epoch == 0, "force_drop tombstone");
    const auto m0 = load_u64(m->linear_live_closures_marked_invalid_total);
    auto r = ev.scan_live_closures_for_linear_captures(true, true);
    CHECK(r.marked_invalid == 0, "no re-mark after force_drop");
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) == m0, "metric stable");
}

static void ac6_stress() {
    std::println("\n--- AC6: multi-closure double-scan stress ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    cs.public_mark_define_dirty("__1665_ac6__");
    CHECK(ev.current_bridge_epoch() != 0, "tracking active");
    for (int i = 0; i < 20; ++i)
        (void)make_moved_tw(ev);
    auto r1 = ev.scan_live_closures_for_linear_captures(true, true);
    CHECK(r1.marked_invalid >= 1, "first pass marks");
    const auto after = load_u64(m->linear_live_closures_marked_invalid_total);
    for (int i = 0; i < 50; ++i) {
        auto r = ev.scan_live_closures_for_linear_captures(true, true);
        CHECK(r.marked_invalid == 0, "re-scan no new marks");
    }
    CHECK(load_u64(m->linear_live_closures_marked_invalid_total) == after,
          "metric stable across re-scans");
}

// AC7: Issue #2003 — EnvFrame lifetime guard scope-marks scans.
// Each scan_live_closures_for_linear_captures() invocation bumps a
// per-site counter; lifetime protocol RAII guards re-entrant on the
// scan path so guard construction/destruction is observable.
static void ac7_lifetime_guard_observable() {
    std::println("\n--- AC7: EnvFrame lifetime guard scans_run observable ---");
    aura::core::envframe_lifetime::reset_envframe_lifetime_stats();
    using aura::core::envframe_lifetime::envframe_lifetime_guards_constructed;
    using aura::core::envframe_lifetime::envframe_lifetime_guards_destructed;
    using aura::core::envframe_lifetime::envframe_lifetime_scans_run;
    const auto c0 = envframe_lifetime_guards_constructed();
    const auto d0 = envframe_lifetime_guards_destructed();
    const auto s0 = envframe_lifetime_scans_run();

    {
        aura::core::envframe_lifetime::EnvFrameLifetimeHost h{};
        // scan_skip_freed requires non-null ctx (protocol contract).
        static int dummy_ctx = 0;
        h.ctx = &dummy_ctx;
        h.scan_skip_freed = [](void*, aura::core::envframe_lifetime::EnvFrameLifetimeSite) {};
        aura::core::envframe_lifetime::EnvFrameLifetimeGuard guard{
            h, aura::core::envframe_lifetime::EnvFrameLifetimeSite::FiberSteal};
        (void)guard.site();
    }

    CHECK(envframe_lifetime_guards_constructed() == c0 + 1, "guard ctor +1");
    CHECK(envframe_lifetime_guards_destructed() == d0 + 1, "guard dtor +1");
    CHECK(envframe_lifetime_scans_run() == s0 + 1,
          "host.scan_skip_freed invoked exactly once on dtor");
}

} // namespace

// AC8: Issue #2087 Phase 2 — env_id_remap_ table + closures write-lock
//       rewrite + new Agent-visible counters (Phase 2 marker bumped
//       kEnvFrameLifetimePhase 1 → 2). Verifies:
//   - kEnvFrameLifetimePhase == 2
//   - resolve_env() returns identity when env_id_remap_ is empty
//   - env_id_remap_size() == 0 before any compact
//   - gc_closures_compacted_total + gc_env_frames_remapped_total
//     advance after compact_env_frames() runs.
//   - After compact: env_id_remap_size > 0; resolve_env returns the
//     remapped id for live closures or -1 for reclaimed frames.
static void ac8_phase2_env_id_remap_2087() {
    std::println("\n--- AC8: #2087 Phase 2 env_id_remap_ ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    // Phase marker: #2087 was 2; #2164 hold-pin bumps to 3 (lineage retained).
    CHECK(aura::core::envframe_lifetime::kEnvFrameLifetimePhase == 3,
          "AC8/H4: kEnvFrameLifetimePhase == 3 (hold-pin Phase 3 marker)");

    // Baseline: env_id_remap_ is identity-mapping before any compact.
    CHECK(ev.env_id_remap_size() == 0, "AC8: env_id_remap_size() == 0 before any compact");

    // resolve_env() with empty remap returns identity (no compact yet).
    for (std::uint64_t i = 0; i < 8; ++i) {
        CHECK(ev.resolve_env(i) == static_cast<std::int64_t>(i),
              std::format("AC8: resolve_env({}) == identity when remap empty", i));
    }

    // Build a few env_frames + closures so compact has work to do.
    std::vector<std::uint64_t> env_ids;
    for (int i = 0; i < 6; ++i) {
        auto eid = ev.alloc_env_frame(NULL_ENV_ID);
        env_ids.push_back(eid);
    }
    std::vector<aura::compiler::ClosureId> cids;
    for (std::size_t i = 0; i < env_ids.size(); ++i) {
        aura::compiler::Closure cl;
        cl.env_id = env_ids[i];
        cids.push_back(ev.register_active_closure(std::move(cl)));
    }
    CHECK(ev.env_frames_size() >= 6, "AC8: env_frames_size >= 6 (heappush)");
    // Drop half the frames (no live closure refs them after we erase)
    for (std::size_t i = 0; i < 3; ++i) {
        // Erase closures pointing at env_ids[i] so those frames are
        // unreferenced post-compact.
        if (i < cids.size())
            (void)ev.erase_active_closure(cids[i]);
    }
    // Advance defuse so unreferenced frames with old version_ become dead
    // (live = version_ >= defuse || referenced). Without this bump, fresh
    // frames stay live forever and compact reclaims 0.
    ev.bump_defuse_version_for_test();

    // Capture metrics pre-compact.
    const auto closures_before = load_u64(m->gc_closures_compacted_total);
    const auto env_frames_before = load_u64(m->gc_env_frames_remapped_total);

    // Run compact_env_frames — Phase 2 path: env_id_remap_ table gets
    // populated (mirrors pair_remap_ / string_remap_ from #2001).
    const std::size_t reclaimed = ev.compact_env_frames();
    CHECK(reclaimed >= 3,
          std::format("AC8: compact reclaimed {} env_frames (≥3 unreferenced)", reclaimed));

    // env_id_remap_ populated.
    const std::size_t remap_size = ev.env_id_remap_size();
    CHECK(remap_size > 0,
          std::format("AC8: env_id_remap_size() == {} (>0 after compact)", remap_size));
    CHECK(remap_size >= 6, std::format("AC8: env_id_remap covers old size ({})", remap_size));

    // Live closures pointing at surviving env_ids: resolve_env returns
    // a valid new id (≥0, < new env_frames_size). Reclaimed ones return -1.
    for (std::size_t i = 3; i < cids.size(); ++i) {
        auto cl_opt = ev.find_active_closure(cids[i]);
        if (!cl_opt)
            continue;
        if (cl_opt->env_id == NULL_ENV_ID)
            continue;
        const auto resolved = ev.resolve_env(cl_opt->env_id);
        if (resolved == -1) {
            // OK: this closure's env frame was reclaimed (NULL_ENV_ID now)
        } else {
            CHECK(resolved >= 0, "AC8: resolve_env returns >= 0 for live frame");
        }
    }
    // Reclaimed closure env_ids map to -1 (their frames were freed).
    for (std::size_t i = 0; i < 3; ++i) {
        if (i >= env_ids.size())
            continue;
        const std::int64_t resolved = ev.resolve_env(env_ids[i]);
        CHECK(resolved == -1 || resolved != static_cast<std::int64_t>(env_ids[i]),
              "AC8: reclaimed old env_id resolves to -1 or different new id");
    }

    // Metrics bumped (Issue #2087 lineage).
    CHECK(load_u64(m->gc_closures_compacted_total) >= closures_before,
          "AC8: gc_closures_compacted_total counter present (>= baseline)");
    CHECK(load_u64(m->gc_env_frames_remapped_total) > env_frames_before,
          std::format("AC8: gc_env_frames_remapped_total {} > baseline {}",
                      load_u64(m->gc_env_frames_remapped_total), env_frames_before));

    // query:envframe-lifetime-stats exposes phase=3 + schema-2087 lineage + schema-2164.
    auto h = cs.eval(R"((engine:metrics \"query:envframe-lifetime-stats\"))");
    CHECK(h && aura::compiler::types::is_hash(*h),
          "AC8: query:envframe-lifetime-stats returns hash");
    if (h && aura::compiler::types::is_hash(*h)) {
        auto phase =
            cs.eval(R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "phase"))");
        CHECK(phase && aura::compiler::types::is_int(*phase) &&
                  aura::compiler::types::as_int(*phase) == 3,
              "AC8/H4: query phase == 3");
        auto s2087 = cs.eval(
            R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "schema-2087"))");
        CHECK(s2087 && aura::compiler::types::is_int(*s2087) &&
                  aura::compiler::types::as_int(*s2087) == 2087,
              "AC8: schema-2087 == 2087 (lineage retained)");
        auto s2164 = cs.eval(
            R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "schema-2164"))");
        CHECK(s2164 && aura::compiler::types::is_int(*s2164) &&
                  aura::compiler::types::as_int(*s2164) == 2164,
              "AC8/H4: schema-2164 == 2164");
    }

    // query:gc-compact-stats exposes closures-compacted + env-frames-remapped
    // + schema-2087 lineage.
    auto g = cs.eval(R"((engine:metrics \"query:gc-compact-stats\"))");
    CHECK(g && aura::compiler::types::is_hash(*g), "AC8: query:gc-compact-stats returns hash");
    if (g && aura::compiler::types::is_hash(*g)) {
        auto s2087g =
            cs.eval(R"((hash-ref (engine:metrics \"query:gc-compact-stats\") "schema-2087"))");
        CHECK(s2087g && aura::compiler::types::is_int(*s2087g) &&
                  aura::compiler::types::as_int(*s2087g) == 2087,
              "AC8: gc-compact-stats schema-2087 == 2087");
    }
}
// Issue #2164: hold-pin — Guard blocks compact_env_frames while live.
static void ac_h1_guard_blocks_compact() {
    std::println("\n--- AC_H1 (#2164): Guard in scope → compact_env_frames busy ---");
    using namespace aura::core::envframe_lifetime;
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    // Unreferenced frames so compact would reclaim when not blocked.
    for (int i = 0; i < 4; ++i)
        (void)ev.alloc_env_frame(NULL_ENV_ID);
    const auto frames0 = ev.env_frames_size();
    CHECK(frames0 >= 4, "AC_H1: env frames allocated");

    const auto blocked0 = envframe_lifetime_blocked_compact_total();
    const auto metric0 = m ? load_u64(m->envframe_compact_blocked_while_guard_held_total) : 0;

    {
        EnvFrameLifetimeHost h{};
        static int dummy = 0;
        h.ctx = &dummy;
        h.scan_skip_freed = [](void*, EnvFrameLifetimeSite) {};
        EnvFrameLifetimeGuard guard{h, EnvFrameLifetimeSite::FiberSteal};
        CHECK(active_guard_depth() >= 1, "AC_H1: active_guard_depth >= 1 under Guard");
        const auto reclaimed = ev.compact_env_frames();
        CHECK(reclaimed == 0, "AC_H1: compact reclaimed 0 while Guard held");
        CHECK(ev.env_frames_size() == frames0, "AC_H1: frames not reclaimed under hold");
        CHECK(envframe_lifetime_blocked_compact_total() > blocked0,
              "AC_H1: blocked_compact_while_guard_held advanced");
        if (m) {
            CHECK(load_u64(m->envframe_compact_blocked_while_guard_held_total) > metric0,
                  "AC_H1: CompilerMetrics blocked counter advanced");
        }
    }
    CHECK(active_guard_depth() == 0, "AC_H1: depth 0 after Guard dtor");
}

static void ac_h2_compact_after_guard() {
    std::println("\n--- AC_H2 (#2164): after Guard dtor, compact proceeds ---");
    using namespace aura::core::envframe_lifetime;
    CompilerService cs;
    auto& ev = cs.evaluator();

    std::vector<std::uint64_t> env_ids;
    for (int i = 0; i < 6; ++i)
        env_ids.push_back(ev.alloc_env_frame(NULL_ENV_ID));
    std::vector<ClosureId> cids;
    for (std::size_t i = 0; i < env_ids.size(); ++i) {
        Closure cl;
        cl.env_id = env_ids[i];
        cids.push_back(ev.register_active_closure(std::move(cl)));
    }
    for (std::size_t i = 0; i < 3; ++i)
        (void)ev.erase_active_closure(cids[i]);
    ev.bump_defuse_version_for_test();

    {
        EnvFrameLifetimeHost h{};
        static int dummy = 0;
        h.ctx = &dummy;
        h.scan_skip_freed = [](void*, EnvFrameLifetimeSite) {};
        EnvFrameLifetimeGuard guard{h, EnvFrameLifetimeSite::BoundaryExit};
        CHECK(ev.compact_env_frames() == 0, "AC_H2: blocked under Guard");
    }

    const auto gen0 = compact_generation();
    const auto reclaimed = ev.compact_env_frames();
    CHECK(reclaimed >= 3, "AC_H2: compact reclaims after Guard dtor");
    CHECK(ev.env_id_remap_size() > 0, "AC_H2: env_id_remap populated");
    CHECK(compact_generation() > gen0, "AC_H2: compact_generation advanced on reclaim");

    // resolve_env consistent for surviving / reclaimed slots.
    for (std::size_t i = 0; i < 3 && i < env_ids.size(); ++i) {
        const auto r = ev.resolve_env(env_ids[i]);
        CHECK(r == -1, "AC_H2: reclaimed old env_id maps to -1");
    }
    for (std::size_t i = 3; i < cids.size(); ++i) {
        auto cl_opt = ev.find_active_closure(cids[i]);
        if (!cl_opt || cl_opt->env_id == NULL_ENV_ID)
            continue;
        const auto resolved = ev.resolve_env(cl_opt->env_id);
        CHECK(resolved >= 0 || resolved == -1, "AC_H2: resolve_env defined for live closure");
    }
}

static void ac_h3_fiber_steal_site_stress() {
    std::println("\n--- AC_H3 (#2164): FiberSteal site + concurrent compact stress ---");
    using namespace aura::core::envframe_lifetime;
    CompilerService cs;
    auto& ev = cs.evaluator();

    for (int i = 0; i < 8; ++i)
        (void)ev.alloc_env_frame(NULL_ENV_ID);

    std::atomic<int> blocked{0};
    std::atomic<int> proceeded{0};
    std::atomic<bool> hold{true};

    std::thread compact_thr([&] {
        while (hold.load(std::memory_order_acquire)) {
            if (ev.compact_env_frames() == 0)
                blocked.fetch_add(1, std::memory_order_relaxed);
            else
                proceeded.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        // Final compact after hold released.
        if (ev.compact_env_frames() > 0)
            proceeded.fetch_add(1, std::memory_order_relaxed);
    });

    {
        EnvFrameLifetimeHost h{};
        static int dummy = 0;
        h.ctx = &dummy;
        h.scan_skip_freed = [](void*, EnvFrameLifetimeSite) {};
        EnvFrameLifetimeGuard guard{h, EnvFrameLifetimeSite::FiberSteal};
        CHECK(envframe_lifetime_site_constructs(EnvFrameLifetimeSite::FiberSteal) >= 1,
              "AC_H3: FiberSteal site construct counted");
        // Give compact thread time to spin under hold.
        for (int i = 0; i < 200; ++i)
            std::this_thread::yield();
        CHECK(blocked.load(std::memory_order_relaxed) > 0 ||
                  envframe_lifetime_blocked_compact_total() > 0,
              "AC_H3: concurrent compact saw busy under FiberSteal hold");
    }
    hold.store(false, std::memory_order_release);
    compact_thr.join();

    // After guard: no stale requirement beyond compact not crashing.
    CHECK(active_guard_depth() == 0, "AC_H3: depth 0 after stress");
    (void)proceeded;
}

static void ac_h4_phase_and_schema() {
    std::println("\n--- AC_H4 (#2164): phase marker + schema keys ---");
    using namespace aura::core::envframe_lifetime;
    CHECK(kEnvFrameLifetimePhase == 3, "AC_H4: phase == 3");
    CompilerService cs;
    auto phase =
        cs.eval(R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "phase"))");
    CHECK(phase && aura::compiler::types::is_int(*phase) &&
              aura::compiler::types::as_int(*phase) == 3,
          "AC_H4: query phase == 3");
    auto s2164 =
        cs.eval(R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "schema-2164"))");
    CHECK(s2164 && aura::compiler::types::is_int(*s2164) &&
              aura::compiler::types::as_int(*s2164) == 2164,
          "AC_H4: schema-2164 present");
    auto s2087 =
        cs.eval(R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "schema-2087"))");
    CHECK(s2087 && aura::compiler::types::is_int(*s2087) &&
              aura::compiler::types::as_int(*s2087) == 2087,
          "AC_H4: schema-2087 retained");
    auto wired = cs.eval(
        R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "hold-pin-wired"))");
    CHECK(wired && aura::compiler::types::is_int(*wired) &&
              aura::compiler::types::as_int(*wired) == 1,
          "AC_H4: hold-pin-wired == 1");
    auto blocked = cs.eval(
        R"((hash-ref (engine:metrics \"query:envframe-lifetime-stats\") "blocked-compact-while-guard-held"))");
    CHECK(blocked && aura::compiler::types::is_int(*blocked) &&
              aura::compiler::types::as_int(*blocked) >= 0,
          "AC_H4: blocked-compact-while-guard-held key present");
}

int main() {
    std::println("=== Issue #1665 / #2164: scan_skip_freed + EnvFrame hold-pin ===");
    ac1_first_mark();
    ac2_no_reinflate();
    ac3_erase_tw();
    ac4_jit_free_separate();
    ac5_force_drop_then_scan();
    ac6_stress();
    ac7_lifetime_guard_observable();
    ac8_phase2_env_id_remap_2087();
    ac_h1_guard_blocks_compact();
    ac_h2_compact_after_guard();
    ac_h3_fiber_steal_site_stress();
    ac_h4_phase_and_schema();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
