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
//   lifecycle close (#2436 — run_post_compact_close orchestrator):
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
//                 (often already done before Phase 5; pass ir_sync_already_done)
//   step 8      — ShapeProfiler via arena on_compact_hook chain
//                 (often already done; pass shape_already_done)
//   steps 9–10  — run_post_compact_close via MutationBoundaryGuard Phase 5
//
// Architecture: Core owns the ordered protocol (this header). Compiler wires
// type-erased PostCompactCloseHooks so Core does not import Evaluator /
// CompilerService / serve::Fiber. Prefer one call to run_post_compact_close
// over ad-hoc note_lifecycle_* scatter (wrappers retained for compact-hook
// step 7 telemetry).
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
// Orchestrator invocations (full close path entry, any kind).
inline std::atomic<std::uint64_t> post_compact_lifecycle_orchestrator_total{0};
// Last completed step id (best-effort; concurrent closes may race — telemetry).
inline std::atomic<std::uint8_t> post_compact_lifecycle_last_step{kStepNone};

inline void note_lifecycle_soft_skip() noexcept {
    post_compact_lifecycle_soft_skip_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_lifecycle_run() noexcept {
    post_compact_lifecycle_runs_total.fetch_add(1, std::memory_order_relaxed);
}

inline void note_lifecycle_ir_sync() noexcept {
    post_compact_lifecycle_ir_sync_total.fetch_add(1, std::memory_order_relaxed);
    post_compact_lifecycle_last_step.store(kStepIrDirtySync, std::memory_order_relaxed);
}

inline void note_lifecycle_stamp_publish() noexcept {
    post_compact_lifecycle_stamp_publish_total.fetch_add(1, std::memory_order_relaxed);
    post_compact_lifecycle_last_step.store(kStepLayoutStampPublish, std::memory_order_relaxed);
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
    std::uint8_t last_step = kStepNone; // highest step advanced this close
};

// ── Executable close orchestrator (steps 7–10) ─────────────────────────
//
// Densify core steps 1–6 stay in arena / Phase 5 densify pairing. This
// API is the single ordered close for IR dirty × shape × LayoutStamp ×
// fiber resume after densify (or soft skip).
//
// Type-erased hooks keep Core free of compiler/serve imports
// (same pattern as TransactionGuardHost / PanicCheckpointHost).

struct PostCompactCloseInput {
    // Moving densify relocated (or attempted with pin held path).
    bool had_moving_densify = false;
    // Step 2 pin-or-remap contract held (false → pin_fail path).
    bool pin_contract_held = true;
    // DensifyConsistencyReport::overall_ok (and other Phase 5 gates).
    // When false but pin held: still re-publish LayoutStamp, but do not
    // bump run/soft/pin path counters (matches pre-orchestrator Phase 5).
    bool densify_gate_ok = true;
    // Compact hook already ran force_soa_instruction_dirty_sync + note.
    bool ir_sync_already_done = false;
    // Compact hook already ran ShapeProfiler on_arena_compact.
    bool shape_already_done = false;
};

struct PostCompactCloseHooks {
    void* ctx = nullptr;
    // Step 7 — only called when had_moving && pin_ok && !ir_sync_already_done.
    void (*finish_ir_dirty_sync)(void* ctx) noexcept = nullptr;
    // Step 8 — only called when had_moving && pin_ok && !shape_already_done.
    void (*on_shape_compact)(void* ctx) noexcept = nullptr;
    // Step 9 — always invoked when non-null (post-compact stamp re-publish).
    void (*publish_layout_stamp)(void* ctx) noexcept = nullptr;
    // Step 10 — returns true if a live fiber received the resume stamp
    // (matches pre-orchestrator "only note_stamp_publish when g_current_fiber").
    bool (*set_fiber_resume_stamp)(void* ctx) noexcept = nullptr;
    // Optional result enrichment.
    std::uint64_t (*read_shape_version)(void* ctx) noexcept = nullptr;
    std::uint64_t (*read_ir_soa_generation)(void* ctx) noexcept = nullptr;
};

// Single ordered close. Call once at outermost MutationBoundary Phase 5
// after densify core (steps 1–6) completes. Do not reorder hook bodies.
//
// Pre: densify core (1–6) already finished or soft-vacuous.
// Post: counters updated; steps 9–10 hooks ran when provided; result filled.
// Safety Class: P1 (post-compact freshness / fiber resume fences)
// Issue: #2436
[[nodiscard]] inline PostCompactLifecycleResult
run_post_compact_close(const PostCompactCloseInput& in,
                       const PostCompactCloseHooks& hooks) noexcept {
    PostCompactLifecycleResult out{};
    post_compact_lifecycle_orchestrator_total.fetch_add(1, std::memory_order_relaxed);
    out.pin_ok = in.pin_contract_held;

    // ── Path classification (counters) ─────────────────────────────
    // pin_fail  > densify-gate fail (stamp only) > soft_skip > full run
    if (!in.pin_contract_held) {
        note_lifecycle_pin_fail();
        out.soft_skip = false;
        out.ran = false;
        out.ir_dirty_synced = false;
    } else if (!in.densify_gate_ok) {
        // Gate failed: no run/soft path counters; still publish stamp.
        out.soft_skip = false;
        out.ran = false;
        out.ir_dirty_synced = in.ir_sync_already_done;
    } else if (!in.had_moving_densify) {
        note_lifecycle_soft_skip();
        out.soft_skip = true;
        out.ran = false;
        out.ir_dirty_synced = true; // vacuous on soft
    } else {
        note_lifecycle_run();
        out.soft_skip = false;
        out.ran = true;
        out.ir_dirty_synced = in.ir_sync_already_done;
    }

    std::uint8_t last = kStepNone;

    // ── Steps 7–8: only on full Moving + pin held + densify gate ok ─
    if (in.had_moving_densify && in.pin_contract_held && in.densify_gate_ok) {
        if (in.ir_sync_already_done) {
            out.ir_dirty_synced = true;
            last = kStepIrDirtySync;
        } else if (hooks.finish_ir_dirty_sync) {
            hooks.finish_ir_dirty_sync(hooks.ctx);
            note_lifecycle_ir_sync();
            out.ir_dirty_synced = true;
            last = kStepIrDirtySync;
        }
        if (in.shape_already_done) {
            last = kStepShapeCompact;
        } else if (hooks.on_shape_compact) {
            hooks.on_shape_compact(hooks.ctx);
            last = kStepShapeCompact;
        }
    }

    // ── Steps 9–10: always attempt stamp publish when hooks wired ──
    // (matches pre-orchestrator: stamp re-publish even after pin_fail /
    // soft so resume fences see latest available truth).
    if (hooks.publish_layout_stamp) {
        hooks.publish_layout_stamp(hooks.ctx);
        last = kStepLayoutStampPublish;
        out.layout_stamp_published = true;
    }
    if (hooks.set_fiber_resume_stamp) {
        if (hooks.set_fiber_resume_stamp(hooks.ctx)) {
            note_lifecycle_stamp_publish();
            last = kStepFiberSafe;
            out.layout_stamp_published = true;
        }
    }

    if (hooks.read_shape_version)
        out.shape_version = hooks.read_shape_version(hooks.ctx);
    if (hooks.read_ir_soa_generation)
        out.ir_soa_generation = hooks.read_ir_soa_generation(hooks.ctx);

    out.last_step = last;
    if (last != kStepNone)
        post_compact_lifecycle_last_step.store(last, std::memory_order_relaxed);
    return out;
}

} // namespace aura::core::post_compact_lifecycle

#endif // AURA_CORE_POST_COMPACT_LIFECYCLE_HH
