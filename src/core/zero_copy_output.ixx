// zero_copy_output.ixx — module re-export of zero-copy / frame-arena SSOT.
//
// Full FrameBumpArena + metrics live in zero_copy_output.hh.
// This unit only re-exports for C++ module consumers (resource_quota /
// lifetime_pin pattern). Prior module scaffold kept a separate
// ZeroCopyFramebuffer with non-atomic counters — not the production path.
//
// Do NOT reintroduce a second ZeroCopyFramebuffer metrics body here.

module;
#include "core/zero_copy_output.hh"

export module aura.core.zero_copy_output;

import std;

export namespace aura::core::zero_copy {

using ::aura::core::zero_copy::FrameBumpArena;
using ::aura::core::zero_copy::g_render_frame_arena;
using ::aura::core::zero_copy::g_zero_copy_fb;
using ::aura::core::zero_copy::g_zero_copy_metrics;
using ::aura::core::zero_copy::kZeroCopyOutputIssue;
using ::aura::core::zero_copy::kZeroCopyOutputPhase;
using ::aura::core::zero_copy::reset_zero_copy_metrics_for_test;
using ::aura::core::zero_copy::snapshot_zero_copy_stats;
using ::aura::core::zero_copy::ZeroCopyFramebuffer;
using ::aura::core::zero_copy::ZeroCopyMetrics;
using ::aura::core::zero_copy::ZeroCopyStatsSnapshot;

} // namespace aura::core::zero_copy
