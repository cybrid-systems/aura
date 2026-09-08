// panic_checkpoint_raii.ixx — Issue #1239 / #1363: PanicCheckpoint RAII
//
// Phase 2 (#1363): wired to Evaluator::save/restore_panic_checkpoint via
// type-erased PanicCheckpointHost (void* + fn ptrs) so this core module
// does not depend on aura.compiler.evaluator.
//
// Hot-Update MVP scope (Issue #1943): PanicCheckpointRAII is part of the
// **in-scope** hot-update correctness contract for single-workspace
// function-body replacement. Cross-workspace / cross-fiber steal paths
// are **deferred** — see docs/hot-update.md.

module;

#include "gc_hooks.h"

export module aura.core.panic_checkpoint_raii;

import std;

export namespace aura::core::panic_cp {

// Phase 2: real save/restore wiring (was 1 = scaffold-only).
inline constexpr int kPanicCheckpointRaiiPhase = 2;

struct PanicCheckpointStats {
    std::uint64_t guards_constructed = 0;
    std::uint64_t auto_rollbacks = 0;
    std::uint64_t commits = 0;
    std::uint64_t saves_ok = 0;
    std::uint64_t saves_failed = 0;
    std::uint64_t restores_ok = 0;
    std::uint64_t restores_failed = 0;
    // Issue #1393: count of restore attempts where
    // PanicCheckpointGuard's bound Evaluator (host.expected_evaluator_id)
    // no longer matches the host's ctx pointer. This signals a
    // cross-evaluator restore attempt (e.g. via aot:reload /
    // persist:load / fiber with cross-evaluator body). The Guard
    // bumps this counter and skips restore (no UB) — operators
    // can monitor via the stats accessor or primitive.
    std::uint64_t restores_discriminator_failed = 0;
    // Issue #1727: discriminator mismatch also clears the stale
    // checkpoint (via host.clear) so panic_safe_* / GC defer do not
    // permanently leak when restore is skipped.
    std::uint64_t restores_discriminator_cleared = 0;
    // Issue #3570: restore attempts skipped because the instance at the ctx
    // address was destroyed and replaced (address match, generation
    // advanced — ABA). Restore AND clear are both skipped: the original
    // save receiver no longer exists, and clearing would drop the NEW
    // occupant's live checkpoint.
    std::uint64_t restores_gen_aba_mismatch_total = 0;
};

inline PanicCheckpointStats g_panic_checkpoint_raii_stats{};

inline void reset_panic_checkpoint_raii_stats() noexcept {
    g_panic_checkpoint_raii_stats = {};
}

// Type-erased host: Evaluator (or a test double) binds save/restore.
// save/restore/clear may be null (stats-only / no-op host).
//
// Issue #1393: added `expected_evaluator_id` discriminator.
// PanicCheckpointGuard dtor verifies host.expected_evaluator_id ==
// host.ctx; on mismatch it bumps restores_discriminator_failed and
// skips restore. This catches cross-evaluator restore attempts
// (aot:reload / persist:load / fiber cross-evaluator body) where
// the void* ctx is no longer the active Evaluator. The
// discriminator lives in the host (not Guard) so a Guard that
// outlives its Evaluator can detect the mismatch on dtor without
// needing a thread_local "current evaluator" pointer.
//
// Issue #1727: on the same mismatch path, invoke `clear` (if set)
// so the evaluator that received save() does not keep a stale
// panic_safe_* snapshot / GC-defer arm forever.
// Issue #3570: process-wide monotonic instance generation for ABA-safe
// cross-instance discrimination. Handed out once per instance construction
// (Evaluator member init); a recycled address always carries a NEW value,
// so a stale Guard's saved generation no longer matches (closes the
// address-only #1393 residual). Lives beside the host — no new registry.
inline std::atomic<std::uint64_t> g_instance_discriminator_gen{0};
[[nodiscard]] inline std::uint64_t next_instance_discriminator_gen() noexcept {
    return g_instance_discriminator_gen.fetch_add(1, std::memory_order_relaxed) + 1;
}

struct PanicCheckpointHost {
    void* ctx = nullptr;
    void* expected_evaluator_id = nullptr; // Issue #1393: cross-evaluator discriminator
    bool (*save)(void* ctx) noexcept = nullptr;
    bool (*restore)(void* ctx) noexcept = nullptr;
    bool (*clear)(void* ctx) noexcept = nullptr; // Issue #1727
    // Issue #3570: ABA discriminator. ctx_gen_source points at the ctx
    // instance's generation atomic; ctx_gen_at_save snapshots it at host
    // construction. nullptr = legacy (no gen check — existing hosts/tests
    // unchanged).
    const std::atomic<std::uint64_t>* ctx_gen_source = nullptr;
    std::uint64_t ctx_gen_at_save = 0;
    // Issue #3604: ABA skip drains the OLD occupant's gc_defer (keyed by
    // the shared evaluator_id) only when the NEW occupant at the recycled
    // address has not re-armed a live checkpoint. nullptr = legacy host —
    // drain skipped, exact #3570 behavior preserved.
    bool (*has_panic_checkpoint)(void* ctx) = nullptr;
};

// RAII guard: save on construct; restore on dtor unless commit().
// Exception-safe panic recovery when host points at Evaluator.
class PanicCheckpointGuard {
public:
    explicit PanicCheckpointGuard(PanicCheckpointHost host) noexcept
        : host_(host) {
        ++g_panic_checkpoint_raii_stats.guards_constructed;
        if (host_.save && host_.ctx) {
            saved_ = host_.save(host_.ctx);
            if (saved_)
                ++g_panic_checkpoint_raii_stats.saves_ok;
            else
                ++g_panic_checkpoint_raii_stats.saves_failed;
        }
        // Issue #3604: snapshot whether a gc_defer arm exists under this
        // evaluator_id once the save attempt completed — the ABA dtor arm
        // may only drain what the OLD occupant owned.
        defer_armed_at_save_ =
            saved_ && host_.ctx != nullptr && aura::gc_hooks::gc_deferred_for_evaluator(host_.ctx);
    }

    ~PanicCheckpointGuard() noexcept {
        if (committed_)
            return;
        // Issue #3570: ABA check first. Same address, different generation
        // → the instance that received save() was destroyed and a new one
        // occupies the address. Restore would write the old checkpoint into
        // the new instance; clear would drop the new instance's live
        // checkpoint (#1727's clear targets the original save receiver,
        // which no longer exists here) — skip both, count only.
        if (host_.ctx_gen_source != nullptr &&
            host_.ctx_gen_source->load(std::memory_order_acquire) != host_.ctx_gen_at_save) {
            ++g_panic_checkpoint_raii_stats.restores_gen_aba_mismatch_total;
            ++g_panic_checkpoint_raii_stats.auto_rollbacks;
            // Issue #3604: skip restore AND host.clear (#3570 — the address
            // now names the NEW occupant), but do not leak the OLD
            // occupant's gc_defer arm: it is keyed by the shared
            // evaluator_id and would keep compact_sweep / destructive
            // Moving deferred for the new occupant until an unrelated
            // reconcile. Drain only what this Guard's save owned AND only
            // when the new occupant has not re-armed a live checkpoint
            // (dropping that arm would expose a half-graph to destructive
            // GC). Legacy hosts without the probe keep exact #3570
            // behavior. steal_complete (#2203/#2314) still owns the
            // fiber-migration case — this is the no-steal recycle path.
            if (defer_armed_at_save_ && host_.has_panic_checkpoint != nullptr &&
                !host_.has_panic_checkpoint(host_.ctx)) {
                const auto drained = aura::gc_hooks::clear_gc_defer_for_evaluator(
                    host_.expected_evaluator_id != nullptr ? host_.expected_evaluator_id
                                                           : host_.ctx);
                if (drained > 0) {
                    (void)aura::gc_hooks::reconcile_gc_defer_bits_after_clear();
                    // Issue #3604 (issue-sanctioned): the drain actually
                    // ran — bump the existing clear total. No new
                    // PanicCheckpointStats mid-field, no steal-named reuse.
                    ++g_panic_checkpoint_raii_stats.restores_discriminator_cleared;
                }
            }
            return;
        }
        // Issue #1393: cross-evaluator discriminator check.
        // If expected_evaluator_id is set (non-null) AND differs
        // from ctx, this Guard was constructed on a different
        // Evaluator than the host is now bound to. Cross-evaluator
        // restore would operate on the wrong state → bump the
        // discriminator-failed counter and skip restore. The user
        // is expected to manually re-establish the checkpoint on
        // the new Evaluator if needed.
        if (host_.expected_evaluator_id != nullptr && host_.expected_evaluator_id != host_.ctx) {
            ++g_panic_checkpoint_raii_stats.restores_discriminator_failed;
            ++g_panic_checkpoint_raii_stats.auto_rollbacks;
            // Issue #1727: skip restore (wrong evaluator) but still clear
            // the checkpoint that save() wrote on host_.ctx — otherwise
            // panic_safe_* + GC defer can permanently leak.
            if (host_.clear && host_.ctx) {
                if (host_.clear(host_.ctx))
                    ++g_panic_checkpoint_raii_stats.restores_discriminator_cleared;
            }
            return;
        }
        if (saved_ && host_.restore && host_.ctx) {
            if (host_.restore(host_.ctx))
                ++g_panic_checkpoint_raii_stats.restores_ok;
            else
                ++g_panic_checkpoint_raii_stats.restores_failed;
        }
        ++g_panic_checkpoint_raii_stats.auto_rollbacks;
    }

    PanicCheckpointGuard(const PanicCheckpointGuard&) = delete;
    PanicCheckpointGuard& operator=(const PanicCheckpointGuard&) = delete;

    // Allow return-by-value / placement from factory helpers.
    PanicCheckpointGuard(PanicCheckpointGuard&& o) noexcept
        : host_(o.host_)
        , saved_(o.saved_)
        , committed_(o.committed_)
        , defer_armed_at_save_(o.defer_armed_at_save_) {
        o.committed_ = true; // moved-from: no restore on dtor
        o.saved_ = false;
        o.defer_armed_at_save_ = false;
    }
    PanicCheckpointGuard& operator=(PanicCheckpointGuard&&) = delete;

    void commit() noexcept {
        committed_ = true;
        ++g_panic_checkpoint_raii_stats.commits;
    }

    [[nodiscard]] bool saved() const noexcept { return saved_; }
    [[nodiscard]] bool committed() const noexcept { return committed_; }

private:
    PanicCheckpointHost host_{};
    bool saved_ = false;
    bool committed_ = false;
    bool defer_armed_at_save_ = false; // Issue #3604: ABA drain gate
};

} // namespace aura::core::panic_cp
