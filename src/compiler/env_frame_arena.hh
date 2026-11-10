// env_frame_arena.hh — Issue #4790: EnvFrame slot exhaustion fails AT THE
// ALLOCATION SITE, never as a silently captureless closure.
//
// EnvFrameStore caps at kMaxSlots (1,048,576 slots). Before this header the
// cap was SILENT: EnvFrameStore::push_back returned NULL_ENV_ID,
// Evaluator::alloc_env_frame propagated it, and every capture caller
// (cl.env_id = alloc_env_frame_from_env(env)) built a closure with NO
// capture at all. A free variable of that closure then read unbound
// hundreds of calls later ("8:32: unbound variable: cap", #4787) with
// nothing pointing at the arena. #4787 removed the leak that made the cap
// reachable in minutes; the cap itself still has to be loud.
//
// The contract: an Evaluator never hands a capture caller NULL_ENV_ID
// because the arena is full. The #4459 freelist is consulted first; when it
// is empty the arena is genuinely exhausted (every slot is a live capture
// frame) and fail() raises the typed Exhausted error. The evaluation
// boundary turns that into an OutOfMemory diagnostic naming the arena, the
// live frame count and kMaxSlots (#5057), so an Aura handler can catch it.
// There is no third outcome — in particular, no silent NULL and no abort().
//
// g_throw_on_exhaustion is retained for the #4790 regression test's explicit
// switch; #5057 made the throw unconditional, so production takes the same
// path.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace aura::compiler::env_frame_arena {

// Issue #4790 AC2: the typed failure the allocation site raises when the
// arena is full and the #4459 freelist is empty. Carries the numbers so the
// test can assert the exact cap was reached instead of matching text.
struct Exhausted : std::runtime_error {
    std::size_t allocated_frames = 0;
    // Slots not currently on the reuse freelist (the same number
    // engine:metrics reports as env_frames_live_total). Equal to
    // allocated_frames when the freelist is empty.
    std::size_t live_frames = 0;
    std::size_t max_slots = 0;

    Exhausted(std::size_t allocated, std::size_t live, std::size_t max_slots_in)
        : std::runtime_error("EnvFrame arena exhausted (" + std::to_string(allocated) +
                             " allocated capture frames, " + std::to_string(live) +
                             " live, kMaxSlots=" + std::to_string(max_slots_in) +
                             "); no capture slot can be reclaimed - a program that keeps" +
                             " this many live closures must release some or use a larger arena")
        , allocated_frames(allocated)
        , live_frames(live)
        , max_slots(max_slots_in) {}
};

// Test-only switch: false in production, so fail() is fatal.
inline std::atomic<bool> g_throw_on_exhaustion{false};

// Process-wide count of exhaustion deaths/throws. Observability only; the
// fatal path runs once per process in production.
inline std::atomic<std::uint64_t> g_exhausted_total{0};

// Issue #4790: the single exhaustion exit for the EnvFrame arena. Called
// from Evaluator::alloc_env_frame after the #4459 reuse freelist came back
// empty and the store refused the append. Never returns.
[[noreturn]] inline void fail(std::size_t allocated_frames, std::size_t live_frames,
                                 std::size_t max_slots) {
    g_exhausted_total.fetch_add(1, std::memory_order_relaxed);
    // Issue #5057: the exhaustion exit is CATCHABLE in production, not an
    // abort(). #4790 made the silent captureless closure loud by killing the
    // process; that left the host no way to handle a legitimate program whose
    // live capture-frame set is larger than kMaxSlots (a list holding >1M
    // closures). Exhausted unwinds to the evaluation boundary, which turns it
    // into an ordinary language-level error (see eval_flat), so an Aura
    // (try ... (catch (e) ...)) can observe it and a top-level program gets a
    // diagnostic instead of SIGABRT. The allocation site still refuses: no
    // caller ever receives NULL_ENV_ID, so the #4787 captureless closure
    // cannot come back.
    //
    // g_throw_on_exhaustion is kept for the #4790 regression test;
    // production now takes the same throw path.
    throw Exhausted(allocated_frames, live_frames, max_slots);
}

} // namespace aura::compiler::env_frame_arena
