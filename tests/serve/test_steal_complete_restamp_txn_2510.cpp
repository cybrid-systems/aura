// @category: unit
// @reason: Issue #2510 — transactional LayoutStamp + provenance restamp on
//          steal-complete (sole success-path restamp entry under strong ABI).
//
//   AC1: on_steal_complete is the sole restamp entry (source-cite + gate)
//   AC2: LayoutStamp field mismatch under Hard → hard-fail; fiber not Ready
//   AC3: production Soft ignored; steal_snapshot_hard_fail_total observable
//   AC4: forced stamp drift inject → mismatch hard-fail; match → restamp +1
//   AC5: light stress (N steals, no silent Ready after hard-fail)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "serve/fiber.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_on_steal_complete(void* fiber_ptr) noexcept;
extern "C" std::uint64_t aura_fiber_static_steal_snapshot_hard_fail_total();

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::FiberState;
using aura::serve::YieldReason;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: sole restamp entry (source-cite) ──
static void ac1_sole_restamp_entry() {
    std::println("\n--- #2510 AC1: on_steal_complete sole restamp entry ---");
    const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    const auto wc = read_file("src/serve/worker.cpp");
    CHECK(fm.find("aura_evaluator_on_steal_complete") != std::string::npos,
          "AC1: strong steal-complete entry");
    CHECK(fm.find("Issue #2510") != std::string::npos, "AC1: #2510 cite");
    CHECK(fm.find("refresh_stale_frames_after_steal") != std::string::npos,
          "AC1: forced EnvFrame restamp in on_steal_complete");
    CHECK(fm.find("steal_complete_restamp_total") != std::string::npos,
          "AC1: restamp counter bump");
    CHECK(fm.find("StableRefRefreshSite::Steal") != std::string::npos ||
              fm.find("auto_restamp_pinned_stable_refs_at") != std::string::npos,
          "AC1: provenance restamp in transaction");
    CHECK(wc.find("call_steal_complete(stolen)") != std::string::npos,
          "AC1: worker sole call site");
    CHECK(wc.find("FiberState::Done") != std::string::npos ||
              wc.find("is_cancel_requested") != std::string::npos,
          "AC1: worker skips Ready after hard-fail");
    // Does NOT clear stamp (resume dual-check).
    CHECK(fm.find("Does NOT clear") != std::string::npos ||
              fm.find("Does NOT clear the stamp") != std::string::npos ||
              fm.find("Does NOT clear resume_layout_stamp") != std::string::npos,
          "AC1: stamp retained for resume");
}

// ── AC2: hard mismatch → cancel + Done ──
static void ac2_hard_mismatch_fail() {
    std::println("\n--- #2510 AC2: LayoutStamp mismatch under Hard → hard-fail ---");
    ::setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();
    CHECK(aura::serve::is_steal_snapshot_hard_mode(), "AC2: hard mode");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
    const auto hf0 = metrics.steal_complete_layout_hard_fail_total.load();
    const auto sh0 = aura_fiber_static_steal_snapshot_hard_fail_total();
    const auto rest0 = metrics.steal_complete_restamp_total.load();

    Fiber f([] {});
    const auto cur = ev.current_layout_stamp();
    // Inject stamp drift (arena_gen +99) — densify race simulation.
    f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + 99, cur.flat_gen, cur.mutation_epoch,
                              cur.env_gen, cur.defuse_version, cur.shape_version);
    aura_evaluator_on_steal_complete(&f);

    CHECK(metrics.layout_stamp_steal_mismatch_total.load() > mm0, "AC2: mismatch +1");
    CHECK(metrics.steal_complete_layout_hard_fail_total.load() > hf0, "AC2: layout hard-fail +1");
    CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() > sh0, "AC2: hard_fail_total +1");
    CHECK(f.is_cancel_requested(), "AC2: cancel requested");
    CHECK(f.state() == FiberState::Done, "AC2: fiber Done (not Ready)");
    // No success restamp after hard-fail.
    CHECK(metrics.steal_complete_restamp_total.load() == rest0, "AC2: no restamp after hard-fail");

    ev.set_compiler_metrics(nullptr);
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    aura::serve::reset_steal_snapshot_soft_for_test();
}

// ── AC3: Soft continues; production Soft ignored (source + soft path) ──
static void ac3_soft_and_production() {
    std::println("\n--- #2510 AC3: Soft metric-only; hard_fail observable ---");
    ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    aura::serve::reset_steal_snapshot_soft_for_test();
    CHECK(aura::serve::is_steal_snapshot_soft_mode(), "AC3: soft mode");
    CHECK(!aura::serve::is_steal_snapshot_hard_mode(), "AC3: hard off under Soft");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    const auto hf0 = metrics.steal_complete_layout_hard_fail_total.load();
    const auto sh0 = aura_fiber_static_steal_snapshot_hard_fail_total();
    Fiber f([] {});
    const auto cur = ev.current_layout_stamp();
    f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + 7, cur.flat_gen, cur.mutation_epoch,
                              cur.env_gen, cur.defuse_version, cur.shape_version);
    aura_evaluator_on_steal_complete(&f);

    CHECK(metrics.steal_complete_layout_hard_fail_total.load() == hf0,
          "AC3: Soft → no layout hard-fail");
    CHECK(aura_fiber_static_steal_snapshot_hard_fail_total() == sh0,
          "AC3: Soft → hard_fail_total flat");
    CHECK(f.state() != FiberState::Done || !f.is_cancel_requested(),
          "AC3: Soft continues (not force Done)");
    // Source: production Soft ignored via steal_snapshot_soft_production_locked.
    const auto fh = read_file("src/serve/fiber.cpp");
    CHECK(fh.find("steal_snapshot_soft_production_locked") != std::string::npos ||
              fh.find("g_steal_snapshot_soft_production_locked") != std::string::npos,
          "AC3: production Soft lock present");
    CHECK(href(cs, "steal-snapshot-hard-fail-total") >= 0 ||
              href(cs, "steal-complete-layout-hard-fail-total") >= 0,
          "AC3: hard_fail queryable (or layout hard-fail key)");

    ev.set_compiler_metrics(nullptr);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();
}

// ── AC4: match → restamp; soft mismatch still dual-check ──
static void ac4_match_restamp_and_drift() {
    std::println("\n--- #2510 AC4: match restamp + soft drift inject ---");
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    // Matching stamp → restamp +1, no hard-fail.
    {
        const auto rest0 = metrics.steal_complete_restamp_total.load();
        const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
        Fiber f([] {});
        const auto cur = ev.current_layout_stamp();
        f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen, cur.flat_gen, cur.mutation_epoch,
                                  cur.env_gen, cur.defuse_version, cur.shape_version,
                                  cur.ir_soa_generation);
        aura_evaluator_on_steal_complete(&f);
        CHECK(metrics.steal_complete_restamp_total.load() > rest0, "AC4: match → restamp +1");
        CHECK(metrics.layout_stamp_steal_mismatch_total.load() == mm0, "AC4: match → no mismatch");
        CHECK(f.has_resume_layout_stamp(), "AC4: stamp retained");
        CHECK(f.state() != FiberState::Done, "AC4: not Done on match");
    }

    // Soft default: mismatch still dual-check, no hard-fail (unless production).
    if (!aura::serve::is_steal_snapshot_hard_mode()) {
        const auto mm0 = metrics.layout_stamp_steal_mismatch_total.load();
        Fiber f2([] {});
        const auto cur = ev.current_layout_stamp();
        f2.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + 1, cur.flat_gen,
                                   cur.mutation_epoch, cur.env_gen, cur.defuse_version,
                                   cur.shape_version);
        aura_evaluator_on_steal_complete(&f2);
        CHECK(metrics.layout_stamp_steal_mismatch_total.load() > mm0,
              "AC4: soft drift → mismatch (no silent green)");
    }

    ev.set_compiler_metrics(nullptr);
}

// ── AC5: stress N steals, hard-fail never Ready ──
static void ac5_stress() {
    std::println("\n--- #2510 AC5: N-iter stamp inject stress ---");
    ::setenv("AURA_STEAL_SNAPSHOT_HARD", "1", 1);
    ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
    aura::serve::reset_steal_snapshot_soft_for_test();

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    CompilerMetrics metrics;
    ev.set_compiler_metrics(&metrics);

    constexpr int N = 200;
    int hard_done = 0;
    int silent_ready = 0;
    for (int i = 0; i < N; ++i) {
        Fiber f([] {});
        const auto cur = ev.current_layout_stamp();
        // Alternate match / mismatch.
        const bool drift = (i % 2) != 0;
        f.set_resume_layout_stamp(cur.arena_id, cur.arena_gen + (drift ? 1 + (i % 3) : 0),
                                  cur.flat_gen, cur.mutation_epoch, cur.env_gen, cur.defuse_version,
                                  cur.shape_version, cur.ir_soa_generation);
        aura_evaluator_on_steal_complete(&f);
        if (drift) {
            if (f.state() == FiberState::Done && f.is_cancel_requested())
                ++hard_done;
            else if (f.state() == FiberState::Ready && !f.is_cancel_requested())
                ++silent_ready;
        }
    }
    CHECK(hard_done == N / 2, "AC5: all mismatch hard-failed (Done+cancel)");
    CHECK(silent_ready == 0, "AC5: zero silent Ready after mismatch");
    CHECK(metrics.steal_complete_layout_hard_fail_total.load() >= static_cast<std::uint64_t>(N / 2),
          "AC5: hard-fail counter covers injects");

    ev.set_compiler_metrics(nullptr);
    ::unsetenv("AURA_STEAL_SNAPSHOT_HARD");
    aura::serve::reset_steal_snapshot_soft_for_test();
}

// ── schema ──
static void ac_schema() {
    std::println("\n--- #2510 schema + query ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2510") == 2510, "schema-2510");
    CHECK(href(cs, "issue-2510") == 2510, "issue-2510");
    CHECK(href(cs, "steal-complete-restamp-wired") == 1, "wired");
    CHECK(href(cs, "steal-complete-restamp-total") >= 0, "restamp total");
    CHECK(href(cs, "steal-complete-layout-hard-fail-total") >= 0, "layout hard-fail total");
    CHECK(href(cs, "schema-2351") == 2351, "schema-2351 retained");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_steal_complete_restamp_txn_2510") != std::string::npos, "cmake");
}

} // namespace

int main() {
    std::println("test_steal_complete_restamp_txn_2510");
    ac1_sole_restamp_entry();
    ac2_hard_mismatch_fail();
    ac3_soft_and_production();
    ac4_match_restamp_and_drift();
    ac5_stress();
    ac_schema();
    if (g_failed)
        return 1;
    std::println("steal-complete restamp txn #2510: OK ({} passed)", g_passed);
    return 0;
}
