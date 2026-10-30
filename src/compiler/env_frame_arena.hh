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
// frame) and the process terminates with an explicit message naming the
// arena, the live frame count and kMaxSlots. There is no third outcome —
// in particular, no silent NULL.
//
// Test seam: g_throw_on_exhaustion makes fail() throw Exhausted instead of
// aborting, so the #4790 test can drive a real Evaluator to kMaxSlots and
// assert the failure without killing the test runner. Production never sets
// it (default false; the switch is flipped before the driving loop starts,
// so relaxed ordering is enough).
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
    std::size_t live_frames = 0;
    std::size_t max_slots = 0;

    Exhausted(std::size_t live, std::size_t max_slots_in)
        : std::runtime_error("aura: fatal: EnvFrame arena exhausted (" + std::to_string(live) +
                             " live capture frames, kMaxSlots=" + std::to_string(max_slots_in) +
                             "); a capture would otherwise be silently dropped")
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
[[noreturn]] inline void fail(std::size_t live_frames, std::size_t max_slots) {
    g_exhausted_total.fetch_add(1, std::memory_order_relaxed);
    if (g_throw_on_exhaustion.load(std::memory_order_relaxed))
        throw Exhausted(live_frames, max_slots);
    // Loud, and specific about the cause: the old behaviour (return
    // NULL_ENV_ID) surfaced as "unbound variable: <name>" at an unrelated
    // call site. This message names the arena instead.
    std::fprintf(stderr,
                 "aura: fatal: EnvFrame arena exhausted (%zu live capture frames, "
                 "kMaxSlots=%zu). Every slot is a live capture frame; the #4459 "
                 "reuse freelist is empty. This is not a silent capture: the "
                 "allocation site refuses.\n",
                 live_frames, max_slots);
    std::fflush(stderr);
    std::abort();
}

} // namespace aura::compiler::env_frame_arena
