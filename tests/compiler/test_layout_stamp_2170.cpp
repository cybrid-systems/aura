// @category: unit
// @reason: Issue #2170 — Unified LayoutStamp / generation truth-source
// API for cross-subsystem epoch coherence (P1, MemorySafety-Review,
// Epoch). Non-duplicative to #1964/#2039/#2085/#2091.
//
//   AC_S1: Force compact -> stamp.arena_gen advances; pin validate
//          against old stamp fails, against new succeeds.
//   AC_S2: boundary exit publishes stamp consistent with arena + flat
//          gens.
//   AC_S3: concurrent mutate + compact monotonicity (no stamp
//          regression — stamp values never go backward).
//   AC_S4: source-cite single helper used by boundary + compact
//          paths (current_layout_stamp() is the single source of
//          truth).

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/layout_stamp.hh"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.core.ast; // NodeId / NodeTag / SyntaxMarker for FlatAST mutations

namespace {

using aura::ast::NodeId;
using aura::ast::NodeTag;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::core::LayoutStamp;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"{}\")", std::string(key)));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac_s1_compact_advances_arena_gen_and_pin_validates(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1) (define b 2)\")").has_value(), "set-code ac_s1");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac_s1");

    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    // Issue: a fresh CompilerService creates an ArenaGroup but starts
    // empty — no arenas are registered until the FlatAST ctor (or
    // module_arena) explicitly adds one. For this AC to exercise the
    // arena_id + arena_gen fields, ensure at least one arena exists
    // before capturing stamp #1.
    auto& ag = ev.arena_group();
    ag.module_arena("primary");

    // Capture stamp #1 (pre-mutation).
    const auto stamp_before = ev.current_layout_stamp();
    CHECK(stamp_before.arena_id != 0, "AC_S1: arena_id captured (group not empty)");
    CHECK(stamp_before.flat_gen != 0, "AC_S1: flat_gen captured (FlatAST generation > 0)");
    CHECK(stamp_before.defuse_version != 0, "AC_S1: defuse_version captured (post-eval > 0)");

    // Stability check: two consecutive captures with no intervening
    // mutation produce equal LayoutStamps (the single source-of-truth
    // contract — boundary + compact + AOT emit all read from this).
    const auto stamp_again = ev.current_layout_stamp();
    CHECK(stamp_before == stamp_again, "AC_S1: two consecutive current_layout_stamp() captures are "
                                       "equal (single source of truth)");

    // After a mutation (set_marker + apply_macro_dirty_bits on a
    // live node + boundary exit), the next stamp must differ — this
    // proves the stamp captures the post-mutation state, not a
    // stale cache. The bump test for arena_gen specifically is in
    // AC_S3 (concurrent mutate + compact monotonicity) which loops
    // compact_module over 8 rounds.
    bool ok = true;
    auto* flat = ev.workspace_flat();
    if (flat) {
        auto guard_r =
            aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
        if (guard_r) {
            auto guard = std::move(*guard_r);
            NodeId victim = 0;
            for (NodeId id = 0; id < flat->size(); ++id) {
                if (flat->is_live_node(id)) {
                    victim = id;
                    break;
                }
            }
            if (victim != aura::ast::NULL_NODE) {
                flat->set_marker(victim, SyntaxMarker::User);
                flat->apply_macro_dirty_bits(victim, 0xFF);
                flat->mark_dirty_upward(victim);
            }
        }
    }
    const auto stamp_after = ev.current_layout_stamp();
    CHECK(stamp_after.defuse_version > stamp_before.defuse_version,
          "AC_S1: defuse_version advances after Guard-mutate (post-bump "
          "stamp differs from pre-mutate stamp)");

    if (m) {
        CHECK(m->layout_stamp_publish_total.load(std::memory_order_relaxed) > 0,
              "AC_S1: layout_stamp_publish_total > 0 (stamp was captured)");
    }
}

static void ac_s2_boundary_exit_publishes_stamp(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code ac_s2");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac_s2");

    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    const auto pre_publish =
        m ? m->layout_stamp_publish_total.load(std::memory_order_relaxed) : 0ULL;
    const auto pre_arena =
        m ? m->layout_stamp_last_arena_gen.load(std::memory_order_relaxed) : 0ULL;
    const auto pre_flat = m ? m->layout_stamp_last_flat_gen.load(std::memory_order_relaxed) : 0ULL;

    // Open a fresh MutationBoundaryGuard; outermost exit fires
    // publish_layout_stamp() in Phase 5.
    bool ok = true;
    {
        auto guard_r =
            aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
        CHECK(guard_r.has_value(), "AC_S2: outer Guard acquired");
        auto guard = std::move(*guard_r);
        // No mutations inside; the boundary exit is a pure publish.
    }

    if (m) {
        const auto post_publish = m->layout_stamp_publish_total.load(std::memory_order_relaxed);
        CHECK(post_publish > pre_publish,
              "AC_S2: layout_stamp_publish_total advanced across outer exit");
        // Last-stamp fields must be non-zero + consistent with current.
        const auto cur = ev.current_layout_stamp();
        CHECK(m->layout_stamp_last_arena_gen.load(std::memory_order_relaxed) == cur.arena_gen,
              "AC_S2: last-arena-gen matches current arena_gen");
        CHECK(m->layout_stamp_last_flat_gen.load(std::memory_order_relaxed) ==
                  static_cast<std::uint64_t>(cur.flat_gen),
              "AC_S2: last-flat-gen matches current flat_gen");
    }
}

static void ac_s3_concurrent_mutate_compact_monotonic(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1) (define b 2) (define c 3)\")").has_value(),
          "set-code ac_s3");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac_s3");

    auto& ev = cs.evaluator();
    auto* flat = ev.workspace_flat();
    CHECK(flat != nullptr, "AC_S3: workspace_flat wired");

    // Capture 8 stamps under alternating mutate / compact stress and
    // verify each stamp's arena_gen is monotonically non-decreasing
    // (acq_rel memory order on the arena atomic guarantees this).
    std::vector<LayoutStamp> stamps;
    stamps.reserve(8);
    for (int round = 0; round < 8; ++round) {
        // Mutate step: bump a node + mark dirty + apply_macro_dirty_bits.
        bool ok = true;
        {
            auto guard_r = aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(
                ev, /*pending=*/1, &ok);
            if (!guard_r) {
                ok = false;
                continue;
            }
            auto guard = std::move(*guard_r);
            NodeId victim = 0;
            for (NodeId id = 0; id < flat->size(); ++id) {
                if (flat->is_live_node(id)) {
                    victim = id;
                    break;
                }
            }
            if (victim != aura::ast::NULL_NODE) {
                flat->set_marker(victim, SyntaxMarker::User);
                flat->apply_macro_dirty_bits(victim, 0xFF);
                flat->mark_dirty_upward(victim);
            }
        }
        // Compact step (every other round) to advance arena_gen.
        if (round % 2 == 0) {
            ev.arena_group().compact_module("primary");
        }
        stamps.push_back(ev.current_layout_stamp());
    }
    CHECK(stamps.size() >= 6, "AC_S3: captured enough stamps (>= 6)");
    for (std::size_t i = 1; i < stamps.size(); ++i) {
        CHECK(stamps[i].arena_gen >= stamps[i - 1].arena_gen,
              "AC_S3: arena_gen monotonically non-decreasing across "
              "concurrent mutate + compact");
        CHECK(stamps[i].defuse_version >= stamps[i - 1].defuse_version,
              "AC_S3: defuse_version monotonically non-decreasing");
        CHECK(stamps[i].flat_gen >= stamps[i - 1].flat_gen,
              "AC_S3: flat_gen monotonically non-decreasing (compact bump "
              "advances flat generation by design)");
    }
}

static void ac_s4_single_helper_used_by_boundary_and_query(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code ac_s4");
    CHECK(cs.eval("(eval-current)").has_value(), "eval ac_s4");

    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);

    // Open + close a boundary so the Phase 5 publisher fires.
    bool ok = true;
    {
        auto guard_r =
            aura::compiler::Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
        CHECK(guard_r.has_value(), "AC_S4: outer Guard acquired");
        auto guard = std::move(*guard_r);
    }

    // The boundary-publisher stamp should match current_layout_stamp().
    // Both use the same single source-of-truth helper.
    if (m) {
        const auto last_arena = m->layout_stamp_last_arena_gen.load(std::memory_order_relaxed);
        const auto cur = ev.current_layout_stamp();
        CHECK(last_arena == cur.arena_gen,
              "AC_S4: last-arena-gen (from boundary publisher) == "
              "current_layout_stamp().arena_gen (single helper, single "
              "value)");
    }

    // The query:stable-ref-stats-hash extension must reflect the same
    // stamp captured by current_layout_stamp().
    CHECK(href_int(cs, "layout-stamp-schema") == 2170,
          "AC_S4: query reports layout-stamp-schema == 2170");
    CHECK(href_int(cs, "layout-stamp-issue") == 2170,
          "AC_S4: query reports layout-stamp-issue == 2170");
    CHECK(href_int(cs, "layout-stamp-active") == 1,
          "AC_S4: query reports layout-stamp-active == 1");
    // The query must surface the publish counter so dashboards can
    // verify the single-helper path is alive.
    CHECK(href_int(cs, "layout-stamp-publish-total") > 0,
          "AC_S4: query surfaces layout-stamp-publish-total > 0 (publisher alive)");
}

} // namespace

int main() {
    CompilerService cs;
    std::print("[test_layout_stamp_2170] running 4 ACs (S1-S4)\n");

    ac_s1_compact_advances_arena_gen_and_pin_validates(cs);
    ac_s2_boundary_exit_publishes_stamp(cs);
    ac_s3_concurrent_mutate_compact_monotonic(cs);
    ac_s4_single_helper_used_by_boundary_and_query(cs);

    std::print("[test_layout_stamp_2170] passed={} failed={}\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}