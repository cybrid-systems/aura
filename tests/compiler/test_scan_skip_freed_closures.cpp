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

} // namespace

int main() {
    std::println("=== Issue #1665: scan skips tombstoned / erased TW closures ===");
    ac1_first_mark();
    ac2_no_reinflate();
    ac3_erase_tw();
    ac4_jit_free_separate();
    ac5_force_drop_then_scan();
    ac6_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
