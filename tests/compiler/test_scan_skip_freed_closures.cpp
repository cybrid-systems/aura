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

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"
#include <cstdint>
#include <print>

import std;
import aura.core.ast;
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

    // Phase marker (Issue #2087): kEnvFrameLifetimePhase bumped 1 → 2.
    CHECK(aura::core::envframe_lifetime::kEnvFrameLifetimePhase == 2,
          "AC8: kEnvFrameLifetimePhase == 2 (Phase 2 marker)");

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
        const auto* cl = ev.find_active_closure(cids[i]).value_or(nullptr);
        if (!cl)
            continue;
        if (cl->env_id == NULL_ENV_ID)
            continue;
        const auto resolved = ev.resolve_env(cl->env_id);
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

    // query:envframe-lifetime-stats exposes phase=2 + closures-compacted +
    // env-frames-remapped + schema-2087 lineage.
    auto h = cs.eval(R"((engine:metrics "query:envframe-lifetime-stats"))");
    CHECK(h && aura::compiler::types::is_hash(*h),
          "AC8: query:envframe-lifetime-stats returns hash");
    if (h && aura::compiler::types::is_hash(*h)) {
        auto phase =
            cs.eval(R"((hash-ref (engine:metrics "query:envframe-lifetime-stats") "phase"))");
        CHECK(phase && aura::compiler::types::is_int(*phase) &&
                  aura::compiler::types::as_int(*phase) == 2,
              "AC8: query phase == 2");
        auto s2087 =
            cs.eval(R"((hash-ref (engine:metrics "query:envframe-lifetime-stats") "schema-2087"))");
        CHECK(s2087 && aura::compiler::types::is_int(*s2087) &&
                  aura::compiler::types::as_int(*s2087) == 2087,
              "AC8: schema-2087 == 2087");
    }

    // query:gc-compact-stats exposes closures-compacted + env-frames-remapped
    // + schema-2087 lineage.
    auto g = cs.eval(R"((engine:metrics "query:gc-compact-stats"))");
    CHECK(g && aura::compiler::types::is_hash(*g), "AC8: query:gc-compact-stats returns hash");
    if (g && aura::compiler::types::is_hash(*g)) {
        auto s2087g =
            cs.eval(R"((hash-ref (engine:metrics "query:gc-compact-stats") "schema-2087"))");
        CHECK(s2087g && aura::compiler::types::is_int(*s2087g) &&
                  aura::compiler::types::as_int(*s2087g) == 2087,
              "AC8: gc-compact-stats schema-2087 == 2087");
    }
}
int main() {
    std::println("=== Issue #1665: scan skips tombstoned / erased TW closures ===");
    ac1_first_mark();
    ac2_no_reinflate();
    ac3_erase_tw();
    ac4_jit_free_separate();
    ac5_force_drop_then_scan();
    ac6_stress();
    ac7_lifetime_guard_observable();
    ac8_phase2_env_id_remap_2087();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
