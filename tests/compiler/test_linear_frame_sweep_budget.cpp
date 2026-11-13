// @category: unit
// @reason: Issue #5274 — bound the outermost-boundary linear frame sweep.
//
// A long-lived serve program keeps one spawned body parked inside the tree
// walker, so every destructive sweep stands down and the env_frames_ arena
// only ever grows (a slot is reused only through the GC sweeper freelist).
// linear_post_mutate_enforce_all used to walk the whole extent — with one
// shard lock per slot — several times per outermost mutate: 65ms -> 973ms
// holds in the 240s game repro, which then tripped the #2349 hold-SLO
// breaker (#5275).
//
//   AC1: one call examines at most kLinearFrameSweepSlice slots
//   AC2: the walk still covers every slot over ceil(extent/slice) calls
//        (round-robin cursor, no slot starved)
//   AC3: a frame with no linear ownership column is skipped without its
//        shard lock (the exact pre-filter that made the sweep cheap)
//   AC4: the per-closure linear capture scan is untouched (still exact)

#include "test_harness.hpp"

#include "compiler/linear_frame_sweep_budget.hh"

#include <cstddef>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::kLinearFrameSweepSlice;
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

// Allocate env frames: every retained lambda captures its loop variable,
// so each iteration materialises a capture frame.
static void grow_frames(CompilerService& cs, std::size_t want) {
    const auto n = std::max<std::size_t>(want, 1);
    const auto src =
        std::format("(begin (define (mk5274 x) (lambda () x))"
                    " (define (hold5274 xs) (if (null? xs) 0 (begin (car xs) (hold5274 (cdr xs)))))"
                    " (define xs5274 (let loop ((i 0) (acc (quote ())))"
                    "   (if (< i {}) (loop (+ i 1) (cons (mk5274 i) acc)) acc)))"
                    " (hold5274 xs5274))",
                    n);
    (void)cs.eval(src);
}

// Grow until the arena is strictly larger than one slice (the runtime may
// reuse frames, so a single round is not guaranteed to be enough).
static void grow_past_slice(CompilerService& cs) {
    for (int round = 0; round < 40; ++round) {
        if (cs.evaluator().env_frames_size() > kLinearFrameSweepSlice)
            return;
        grow_frames(cs, kLinearFrameSweepSlice);
    }
}

// ── AC1: one call is bounded by the slice ──
static void ac1_slice_bound() {
    std::println("\n--- AC1: one sweep examines at most kLinearFrameSweepSlice slots ---");
    CompilerService cs;
    grow_past_slice(cs);

    const auto extent = cs.evaluator().env_frames_size();
    CHECK(extent > kLinearFrameSweepSlice, "AC1: arena grew past one slice");

    const auto r = cs.evaluator().linear_post_mutate_enforce_all();
    CHECK(r.frames_checked > 0, "AC1: the slice still examines slots");
    CHECK(r.frames_checked <= kLinearFrameSweepSlice,
          "AC1: frames_checked is capped at kLinearFrameSweepSlice");
    CHECK(r.all_safe, "AC1: a clean arena stays safe");
}

// ── AC2: round-robin coverage, no slot starved ──
static void ac2_round_robin_coverage() {
    std::println("\n--- AC2: the cursor covers the whole extent over N calls ---");
    CompilerService cs;
    grow_past_slice(cs);
    const auto extent = cs.evaluator().env_frames_size();

    std::size_t total = 0;
    const std::size_t passes = (extent + kLinearFrameSweepSlice - 1) / kLinearFrameSweepSlice;
    for (std::size_t i = 0; i < passes; ++i) {
        const auto r = cs.evaluator().linear_post_mutate_enforce_all();
        CHECK(r.frames_checked <= kLinearFrameSweepSlice, "AC2: every call stays bounded");
        total += static_cast<std::size_t>(r.frames_checked);
    }
    CHECK(total >= extent, "AC2: ceil(extent/slice) calls examine every live slot");
}

// ── AC3: exact pre-filter + AC4: closure scan untouched (structural) ──
static void ac3_prefilter_and_ac4_scan() {
    std::println("\n--- AC3/AC4: exact pre-filter, closure scan untouched ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("bindings_linear_ownership_state_.empty()") != std::string::npos,
          "AC3: sweep skips frames with no linear ownership column");
    CHECK(env.find("kLinearFrameSweepSlice") != std::string::npos,
          "AC3: sweep is sliced by the shared budget constant");
    CHECK(env.find("linear_frame_sweep_cursor_") != std::string::npos,
          "AC3: slice resumes from the round-robin cursor");
    // AC4: the per-closure scan is not sliced — it is the check that must
    // stay exact on every boundary.
    const auto scan_start = env.find("Evaluator::scan_live_closures_for_linear_captures(");
    CHECK(scan_start != std::string::npos, "AC4: closure scan present");
    if (scan_start != std::string::npos) {
        const auto scan_end = env.find("ClosureId Evaluator::register_active_closure", scan_start);
        CHECK(scan_end != std::string::npos, "AC4: closure scan bounded region found");
        if (scan_end != std::string::npos) {
            const auto body = env.substr(scan_start, scan_end - scan_start);
            CHECK(body.find("kLinearFrameSweepSlice") == std::string::npos,
                  "AC4: closure scan is not sliced / deferred");
        }
    }
}

} // namespace

int run_test_linear_frame_sweep_budget() {
    std::println("=== Issue #5274: bounded outermost boundary linear frame sweep ===");
    ac1_slice_bound();
    ac2_round_robin_coverage();
    ac3_prefilter_and_ac4_scan();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_frame_sweep_budget();
}
#endif
