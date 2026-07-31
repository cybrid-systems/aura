// post_compact_lifecycle.hh — Issue #2436: canonical Arena × IR SoA × Shape ×
// fiber post-compact lifecycle (single ordered invariant).
//
// ── Why this file exists ─────────────────────────────────────────────────
// Individual pieces are strong (#2256 Moving densify + pin-or-remap,
// #2139 finish_dirty_sync, #1521/#2255 ShapeProfiler on_arena_compact,
// #2170/#2432 LayoutStamp fiber resume). Under concurrent AI multi-round
// self-mod they were distributed across arena.ixx / ir_soa.ixx /
// shape_profiler / mutation boundary without a single ordered contract.
// Transient windows (stamp captured *before* compact, IR dirty without
// finish_dirty_sync, shape_version advance not on the resume stamp) allow
// silent-stale or over-invalidate.
//
// ── Canonical post-compact lifecycle (do not reorder) ────────────────────
// After Moving densify (`live_compact(Moving)` / `compact_all_moving_pinned`):
//
//   densify core (already #2365/#2368 densify_consistency_report.h):
//     1. RootRemapPass            (inside densify via RootRemapCallback)
//     2. pin-or-remap verify      (verify_pins_under_moving_compact — hard)
//     3. EnvFrame live-ref xfer   (force_densify_remap_pairing step)
//     4. closure remount scan
//     5. dual-epoch restamp
//     6. DensifyConsistencyReport axes (pin/linear/type/root/closure/env)
//
//   lifecycle close (#2436 — this header + Phase 5 / compact hooks):
//     7. IR SoA finish_dirty_sync  (block→instr dirty; pin-or-remap already
//                                  held from step 2; generation fence bumps
//                                  via mark_block_dirty if any cascade)
//     8. ShapeProfiler on_arena_compact
//        (arena on_compact_hook chain — soft version bump, preserve is_stable)
//     9. LayoutStamp re-publish on current fiber
//        (shape_version + ir_soa_generation + arena gen MUST be captured
//         *after* steps 7–8 so resume fences see post-compact truth)
//    10. Fiber resume safe point
//        (resume/steal hard-compare LayoutStamp; mismatch → dual-path scan)
//
// Soft / empty densify / no Moving:
//   Steps 1–6 vacuous ok; steps 7–8 skipped (zero extra work); step 9 still
//   publishes current stamp (cheap POD copy — same as pre-#2436 Phase 5).
//
// Pin-or-remap violation (step 2 false): hard-fail under
// AURA_MOVING_PIN_CONTRACT=hard (default production security); soft mode
// suppresses Phase 5 success metrics (#2266). Never resume with a success
// stamp when pin_contract_held is false.
//
// Ownership:
//   steps 1–2   — ASTArena::live_compact(Moving) + lifetime_pin
//   steps 3–6   — Evaluator::force_densify_remap_pairing + Phase 5 driver
//   step 7      — CompilerService compact hook / finish_cascade_soa_dirty_sync_
//   step 8      — ShapeProfiler via arena on_compact_hook chain
//   steps 9–10  — MutationBoundaryGuard Phase 5 + Fiber resume fences
//
// Observability: post_compact_lifecycle_* counters below + schema-2436.

#ifndef AURA_CORE_POST_COMPACT_LIFECYCLE_HH
#define AURA_CORE_POST_COMPACT_LIFECYCLE_HH

#include <atomic>
#include <cstdint>

namespace aura::core::post_compact_lifecycle {

// Issue stamp for Agents / query surface.
inline constexpr int kPostCompactLifecycleIssue = 2436;

// Step ids for force-reason / debug (match lifecycle comment above).
inline constexpr std::uint8_t kStepNone = 0;
inline constexpr std::uint8_t kStepRootRemap = 1;
inline constexpr std::uint8_t kStepPinVerify = 2;
inline constexpr std::uint8_t kStepEnvframe = 3;
inline constexpr std::uint8_t kStepClosure = 4;
inline constexpr std::uint8_t kStepDualEpoch = 5;
inline constexpr std::uint8_t kStepReport = 6;
inline constexpr std::uint8_t kStepIrDirtySync = 7;
inline constexpr std::uint8_t kStepShapeCompact = 8;
inline constexpr std::uint8_t kStepLayoutStampPublish = 9;
inline constexpr std::uint8_t kStepFiberSafe = 10;

// ── Counters (process-global; soft path only bumps soft_skip) ───────────
// Invocations that ran the full post-compact close (Moving + pin held).
inline std::atomic<std::uint64_t> post_compact_lifecycle_runs_total{0};
// Soft / no-moving / empty densify early exits (AC5 zero-cost path).
inline std::atomic<std::uint64_t> post_compact_lifecycle_soft_skip_total{0};
// Step 7 completed (finish_dirty_sync called at least once this close).
inline std::atomic<std::uint64_t> post_compact_lifecycle_ir_sync_total{0};
// Step 9 completed (LayoutStamp re-published after compact).
inline std::atomic<std::uint64_t> post_compact_lifecycle_stamp_publish_total{0};
// Pin-or-remap hard-fail observed at lifecycle close (mirrors #2266).
inline std::atomic<std::uint64_t> post_compact_lifecycle_pin_fail_total{0};
// Wired sentinel (query surface).
inline std::atomic<std::uint64_t> post_compact_lifecycle_wired{1};

inline void note_lifecycle_soft_skip() noexcept {
    post_compact_lifecycle_soft_skip_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_lifecycle_run() noexcept {
    post_compact_lifecycle_runs_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_lifecycle_ir_sync() noexcept {
    post_compact_lifecycle_ir_sync_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_lifecycle_stamp_publish() noexcept {
    post_compact_lifecycle_stamp_publish_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_lifecycle_pin_fail() noexcept {
    post_compact_lifecycle_pin_fail_total.fetch_add(1, std::memory_order_relaxed);
}

// Result snapshot for tests / Agents (not required on soft path).
struct PostCompactLifecycleResult {
    bool ran = false;                    // true only when Moving densify closed fully
    bool soft_skip = true;               // true when no compact work (AC5)
    bool pin_ok = true;                  // step 2
    bool ir_dirty_synced = true;         // step 7
    bool layout_stamp_published = false; // step 9
    std::uint64_t shape_version = 0;     // post-compact (step 8 visible)
    std::uint64_t ir_soa_generation = 0;
};

} // namespace aura::core::post_compact_lifecycle

#endif // AURA_CORE_POST_COMPACT_LIFECYCLE_HH
