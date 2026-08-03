// hot_update_registry.cpp — Issue #1956 / #2014 / #2114
// Process-wide HotUpdateRegistry singleton.

#include "compiler/hot_update_registry.hh"

#include "compiler/aura_jit_bridge.h"
#include "compiler/lock_order_audit.h" // Issue #2316: lock-order audit wire

#include <chrono>
#include <cstdlib> // Issue #2236: std::getenv for AURA_STORM_ISOLATION resolver
#include <cstring> // Issue #2236: std::strcmp for resolver
#include <utility>

// C-linkage boundary probes (strong in evaluator_fiber_mutation; weak stubs).
extern "C" std::size_t aura_evaluator_mutation_boundary_depth();
extern "C" int aura_evaluator_mutation_boundary_held();

namespace aura::compiler {

namespace {

    std::uint64_t steady_ms_now() noexcept {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

    // Issue #2114: TLS soft reemit boundary depth (per-thread reemit call).
    thread_local int g_soft_reemit_boundary_depth = 0;

} // namespace

HotUpdateRegistry& HotUpdateRegistry::instance() noexcept {
    static HotUpdateRegistry reg;
    return reg;
}

void HotUpdateRegistry::on_reemit_provider_set(bool wired) noexcept {
    reemit_wired_.store(wired, std::memory_order_relaxed);
    register_calls_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_define_dirty_provider_set(bool wired) noexcept {
    define_dirty_wired_.store(wired, std::memory_order_relaxed);
    register_calls_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_aot_emit_provider_set(bool wired) noexcept {
    aot_emit_wired_.store(wired, std::memory_order_relaxed);
    register_calls_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_emit_region_mask_set(std::uint64_t mask) noexcept {
    emit_region_mask_.store(mask, std::memory_order_relaxed);
    register_calls_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_stable_func_id_preserve(bool preserved) noexcept {
    if (preserved)
        stable_id_preserve_.fetch_add(1, std::memory_order_relaxed);
    else
        stable_id_assign_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reemit_pipeline_call(std::uint64_t candidates,
                                                std::uint64_t successes) noexcept {
    reemit_pipeline_calls_.fetch_add(1, std::memory_order_relaxed);
    reemit_candidates_.fetch_add(candidates, std::memory_order_relaxed);
    reemit_success_.fetch_add(successes, std::memory_order_relaxed);
    // Issue #2502: feed clean reemit successes into the re-promote window
    // (zero-cost when force_jit_regions_mask_ is already 0).
    if (successes > 0)
        maybe_force_jit_repromote_on_clean_success();
    else if (force_jit_regions_mask_.load(std::memory_order_relaxed) != 0)
        force_jit_stable_successes_.store(0, std::memory_order_relaxed);
    // Issue #2601: lazy retry hook. Zero-cost when force_jit_regions_mask_ == 0
    // (idle path) — the decide_exhausted_min_dirty_retry() short-circuits.
    // The bridge owns the actual reemit drive (counter bumps + reemit call).
    if (force_jit_regions_mask_.load(std::memory_order_relaxed) != 0)
        aura_hot_update_maybe_retry_exhausted_min_dirty();
}

void HotUpdateRegistry::on_reload_success() noexcept {
    aot_reload_success_.fetch_add(1, std::memory_order_relaxed);
    // Issue #2302: clear force_jit_regions_mask (wholesale — successful
    // reload un-forces all regions) + reset attempts_left to 0.
    force_jit_regions_mask_.store(0, std::memory_order_relaxed);
    attempts_left_.store(0, std::memory_order_relaxed);
    // Clear deferred-reemit flag too — successful reload means the
    // deferred reemit (if any) has been processed.
    deferred_reemit_pending_v2_.store(0, std::memory_order_relaxed);
    // Issue #2502: wholesale clear ends the re-promote streak (mask
    // already idle; streak is only meaningful while demoted).
    force_jit_stable_successes_.store(0, std::memory_order_relaxed);
    // Issue #2601: clear retry state (reload succeeded — no more
    // bounded retries needed; the next exhaust will re-seed).
    exhausted_min_dirty_retry_attempts_left_.store(0, std::memory_order_relaxed);
    exhausted_min_dirty_retry_last_at_ms_.store(0, std::memory_order_relaxed);
    exhausted_min_dirty_retry_last_reason_.store(0, std::memory_order_relaxed);
}

// Issue #2502: auto re-promote force-JIT regions after a stable window of
// consecutive clean successes. Policy:
//   - Soft zero-cost when force_jit_regions_mask_ == 0
//   - Require StormLevel::None (no global / shape storm)
//   - Require attempts_left_ == 0 (not mid multi-round reload)
//   - Optionally require pending_dirty_count_ == 0 (default on)
//   - N consecutive clean reemit successes (window knob, default 3)
// Storm / fail / zero-success reemit resets the streak (no re-promote).
void HotUpdateRegistry::maybe_force_jit_repromote_on_clean_success() noexcept {
    const auto mask = force_jit_regions_mask_.load(std::memory_order_relaxed);
    if (mask == 0) {
        // Idle demotion: keep streak zero (zero-cost path for hot reemit).
        if (force_jit_stable_successes_.load(std::memory_order_relaxed) != 0)
            force_jit_stable_successes_.store(0, std::memory_order_relaxed);
        return;
    }
    const auto window = force_jit_repromote_window_.load(std::memory_order_relaxed);
    if (window == 0) {
        force_jit_stable_successes_.store(0, std::memory_order_relaxed);
        return; // policy disabled
    }
    // Preconditions: storm idle + attempts idle + optional pending idle.
    if (current_storm_level() != StormLevel::None) {
        force_jit_stable_successes_.store(0, std::memory_order_relaxed);
        return;
    }
    if (attempts_left_.load(std::memory_order_relaxed) != 0) {
        force_jit_stable_successes_.store(0, std::memory_order_relaxed);
        return;
    }
    if (force_jit_repromote_require_pending_idle_.load(std::memory_order_relaxed) != 0 &&
        pending_dirty_count_.load(std::memory_order_relaxed) != 0) {
        // Issue #2601 AC2: optional policy knob — when enabled, advance
        // the streak even with pending_dirty > 0 if the success covered
        // the force-JIT reason regions (force_jit_regions_mask_ != 0 AND
        // successes > 0 on this reemit pipeline call). Default off
        // preserves #2502 require-pending-idle behavior.
        if (force_jit_repromote_allow_pending_idle_when_force_jit_covered_.load(
                std::memory_order_relaxed) == 0) {
            force_jit_stable_successes_.store(0, std::memory_order_relaxed);
            return;
        }
        // Fall through with the knob enabled — caller-side successes
        // already proved the reemit covered the force-JIT regions.
    }
    const auto streak = force_jit_stable_successes_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (streak < window)
        return;
    // Window met: clear demoted mask bits (process-wide stability → all
    // reasons eligible). Stamp last repromoted reason from the demotion
    // that put us here for agent correlation.
    const auto reason = last_force_jit_reason_.load(std::memory_order_relaxed);
    force_jit_regions_mask_.store(0, std::memory_order_relaxed);
    force_jit_stable_successes_.store(0, std::memory_order_relaxed);
    force_jit_repromote_total_.fetch_add(1, std::memory_order_relaxed);
    last_force_jit_repromote_reason_.store(reason, std::memory_order_release);
    last_force_jit_repromote_at_epoch_notify_.store(epoch_notify_.load(std::memory_order_relaxed),
                                                    std::memory_order_relaxed);
}

void HotUpdateRegistry::set_force_jit_repromote_window(std::uint32_t n) noexcept {
    force_jit_repromote_window_.store(n, std::memory_order_relaxed);
}

std::uint32_t HotUpdateRegistry::force_jit_repromote_window() const noexcept {
    return force_jit_repromote_window_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::set_force_jit_repromote_require_pending_idle(bool require) noexcept {
    force_jit_repromote_require_pending_idle_.store(require ? 1 : 0, std::memory_order_relaxed);
}

bool HotUpdateRegistry::force_jit_repromote_require_pending_idle() const noexcept {
    return force_jit_repromote_require_pending_idle_.load(std::memory_order_relaxed) != 0;
}

std::uint32_t HotUpdateRegistry::force_jit_stable_successes() const noexcept {
    return force_jit_stable_successes_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::force_jit_repromote_total() const noexcept {
    return force_jit_repromote_total_.load(std::memory_order_relaxed);
}

std::uint8_t HotUpdateRegistry::last_force_jit_repromote_reason() const noexcept {
    return last_force_jit_repromote_reason_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::last_force_jit_repromote_at_epoch_notify() const noexcept {
    return last_force_jit_repromote_at_epoch_notify_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::reset_force_jit_repromote_for_test() noexcept {
    force_jit_repromote_window_.store(3, std::memory_order_relaxed);
    force_jit_stable_successes_.store(0, std::memory_order_relaxed);
    force_jit_repromote_require_pending_idle_.store(1, std::memory_order_relaxed);
    force_jit_repromote_total_.store(0, std::memory_order_relaxed);
    last_force_jit_repromote_reason_.store(0, std::memory_order_relaxed);
    last_force_jit_repromote_at_epoch_notify_.store(0, std::memory_order_relaxed);
}

// ── Issue #2601: exhausted min-dirty retry closed loop ──
// Setters / getters (zero-cost reads; set is rare).
void HotUpdateRegistry::set_exhausted_min_dirty_retry_cap(std::uint32_t n) noexcept {
    exhausted_min_dirty_retry_attempts_cap_.store(n, std::memory_order_relaxed);
}

std::uint32_t HotUpdateRegistry::exhausted_min_dirty_retry_cap() const noexcept {
    return exhausted_min_dirty_retry_attempts_cap_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::set_exhausted_min_dirty_retry_backoff_ms(std::uint64_t ms) noexcept {
    exhausted_min_dirty_retry_backoff_ms_.store(ms, std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::exhausted_min_dirty_retry_backoff_ms() const noexcept {
    return exhausted_min_dirty_retry_backoff_ms_.load(std::memory_order_relaxed);
}

std::uint32_t HotUpdateRegistry::exhausted_min_dirty_retry_attempts_left() const noexcept {
    return exhausted_min_dirty_retry_attempts_left_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::exhausted_min_dirty_retry_last_at_ms() const noexcept {
    return exhausted_min_dirty_retry_last_at_ms_.load(std::memory_order_relaxed);
}

std::uint8_t HotUpdateRegistry::exhausted_min_dirty_retry_last_reason() const noexcept {
    return exhausted_min_dirty_retry_last_reason_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::aot_exhausted_min_dirty_retry_total() const noexcept {
    return aot_exhausted_min_dirty_retry_total_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::aot_exhausted_min_dirty_retry_success_total() const noexcept {
    return aot_exhausted_min_dirty_retry_success_total_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::aot_exhausted_min_dirty_retry_storm_skip_total() const noexcept {
    return aot_exhausted_min_dirty_retry_storm_skip_total_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::aot_exhausted_min_dirty_retry_cap_hit_total() const noexcept {
    return aot_exhausted_min_dirty_retry_cap_hit_total_.load(std::memory_order_relaxed);
}

// Issue #2601 AC2: policy knob.
void HotUpdateRegistry::set_force_jit_repromote_allow_pending_idle_when_force_jit_covered(
    bool allow) noexcept {
    force_jit_repromote_allow_pending_idle_when_force_jit_covered_.store(allow ? 1 : 0,
                                                                         std::memory_order_relaxed);
}

bool HotUpdateRegistry::force_jit_repromote_allow_pending_idle_when_force_jit_covered()
    const noexcept {
    return force_jit_repromote_allow_pending_idle_when_force_jit_covered_.load(
               std::memory_order_relaxed) != 0;
}

// Issue #2601: pure decision helper. Caller (bridge) reads this and
// dispatches based on the outcome. Zero-cost when force_jit_regions_mask_
// is 0 (idle path) — short-circuits on the first load.
HotUpdateRegistry::ExhaustedMinDirtyRetryDecision
HotUpdateRegistry::decide_exhausted_min_dirty_retry() const noexcept {
    if (force_jit_regions_mask_.load(std::memory_order_relaxed) == 0)
        return ExhaustedMinDirtyRetryDecision::NoForceJit;
    const auto attempts_left =
        exhausted_min_dirty_retry_attempts_left_.load(std::memory_order_relaxed);
    if (attempts_left == 0)
        return ExhaustedMinDirtyRetryDecision::NoAttemptsLeft;
    const auto last_at = exhausted_min_dirty_retry_last_at_ms_.load(std::memory_order_relaxed);
    if (last_at != 0) {
        const auto now = steady_ms_now();
        const auto backoff = exhausted_min_dirty_retry_backoff_ms_.load(std::memory_order_relaxed);
        if (now < last_at || (now - last_at) < backoff)
            return ExhaustedMinDirtyRetryDecision::BackoffNotElapsed;
    }
    if (current_storm_level() != StormLevel::None)
        return ExhaustedMinDirtyRetryDecision::StormActive;
    if (hard_storm_active())
        return ExhaustedMinDirtyRetryDecision::StormActive;
    return ExhaustedMinDirtyRetryDecision::Retry;
}

// Issue #2601: consume one retry attempt. Single bookkeeping site —
// CAS loop on attempts_left, then stamp last_at_ms.
void HotUpdateRegistry::consume_exhausted_min_dirty_retry_attempt() noexcept {
    auto attempts_left = exhausted_min_dirty_retry_attempts_left_.load(std::memory_order_relaxed);
    while (attempts_left > 0) {
        if (exhausted_min_dirty_retry_attempts_left_.compare_exchange_weak(
                attempts_left, attempts_left - 1, std::memory_order_relaxed))
            break;
    }
    exhausted_min_dirty_retry_last_at_ms_.store(steady_ms_now(), std::memory_order_relaxed);
}

// Issue #2601: test isolation. Clear retry state + counters without
// touching force_jit_regions_mask_ (use on_reload_success for that).
void HotUpdateRegistry::reset_exhausted_min_dirty_retry_for_test() noexcept {
    exhausted_min_dirty_retry_attempts_left_.store(0, std::memory_order_relaxed);
    exhausted_min_dirty_retry_attempts_cap_.store(3, std::memory_order_relaxed);
    exhausted_min_dirty_retry_backoff_ms_.store(100, std::memory_order_relaxed);
    exhausted_min_dirty_retry_last_at_ms_.store(0, std::memory_order_relaxed);
    exhausted_min_dirty_retry_last_reason_.store(0, std::memory_order_relaxed);
    aot_exhausted_min_dirty_retry_total_.store(0, std::memory_order_relaxed);
    aot_exhausted_min_dirty_retry_success_total_.store(0, std::memory_order_relaxed);
    aot_exhausted_min_dirty_retry_storm_skip_total_.store(0, std::memory_order_relaxed);
    aot_exhausted_min_dirty_retry_cap_hit_total_.store(0, std::memory_order_relaxed);
    force_jit_repromote_allow_pending_idle_when_force_jit_covered_.store(0,
                                                                         std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reload_rollback(AotReloadFail reason) noexcept {
    aot_reload_rollback_.fetch_add(1, std::memory_order_relaxed);
    // Issue #2502: any fail in the window breaks the clean-success streak.
    force_jit_stable_successes_.store(0, std::memory_order_relaxed);
    last_aot_reload_fail_reason_.store(static_cast<std::uint8_t>(reason),
                                       std::memory_order_release);
    switch (reason) {
        case AotReloadFail::Dlopen:
            aot_reload_fail_dlopen_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Version:
            aot_reload_fail_version_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Region:
            aot_reload_fail_region_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Defuse:
            aot_reload_fail_defuse_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Env:
            aot_reload_fail_env_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Linear:
            aot_reload_fail_linear_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Staging:
            aot_reload_fail_staging_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Other:
            aot_reload_fail_other_.fetch_add(1, std::memory_order_relaxed);
            break;
        case AotReloadFail::Ok:
            // Success path uses on_reload_success() instead — Ok here is
            // a no-op (no counter to bump).
            break;
    }
}

void HotUpdateRegistry::on_reload_rollback() noexcept {
    aot_reload_rollback_.fetch_add(1, std::memory_order_relaxed);
    // Issue #2502: any fail in the window breaks the clean-success streak.
    force_jit_stable_successes_.store(0, std::memory_order_relaxed);
    last_aot_reload_fail_reason_.store(static_cast<std::uint8_t>(AotReloadFail::Other),
                                       std::memory_order_release);
    aot_reload_fail_other_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_force_jit_for_reason(AotReloadFail reason) noexcept {
    // Issue #2232: visible Agent-facing callback when reload policy
    // exhausts multi-round reemit and falls back to JIT-only. Slot
    // invalidation is deferred; counters make the decision observable.
    force_jit_for_reason_total_.fetch_add(1, std::memory_order_relaxed);
    last_force_jit_reason_.store(static_cast<std::uint8_t>(reason), std::memory_order_release);
    // Issue #2367: stamp epoch_notify_ progress at fall-back so agents
    // can correlate force-JIT reason with the last recovery epoch.
    last_force_jit_at_epoch_notify_.store(epoch_notify_.load(std::memory_order_relaxed),
                                          std::memory_order_relaxed);
    // Issue #2302: set the force_jit_regions_mask bit for this reason
    // (bit N = reason N in the AotReloadFail enum). Agents query the
    // mask via query:reload-recovery-state to know which regions
    // are currently in force-JIT mode without OR'ing per-reason counters.
    force_jit_regions_mask_.fetch_or(static_cast<std::uint64_t>(1)
                                         << static_cast<std::uint8_t>(reason),
                                     std::memory_order_relaxed);
    // attempts_left exhausted on fall-back (matches the policy_for()
    // loop terminal condition in aura_jit_bridge.cpp).
    attempts_left_.store(0, std::memory_order_relaxed);
    last_aot_reload_fail_reason_.store(static_cast<std::uint8_t>(reason),
                                       std::memory_order_release);
    // Issue #2502: new force-JIT reason resets the re-promote streak
    // (window must rebuild after demotion recurrence).
    force_jit_stable_successes_.store(0, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_exhausted_min_dirty_queue(AotReloadFail reason) noexcept {
    // Issue #2544: minimal dirty set from last fail reason — same bit
    // encoding as force_jit_regions_mask_ (bit N = AotReloadFail N).
    // Cascade trigger marks "reemit wanted" for agents + cascade path
    // without a full-module dirty fan-out.
    const auto mask = static_cast<std::uint64_t>(1) << static_cast<std::uint8_t>(reason);
    on_region_mask_from_dirty(mask);
    on_cascade_reemit_trigger(/*candidates_hint=*/1);
    // Issue #2601: seed retry closed loop. attempts_left = cap (default 3),
    // last_reason = reason, last_at_ms = 0 (the first retry is ready
    // immediately when storm clears — no backoff on the first attempt).
    // Subsequent retries await a steady_ms_now() >= last_at + backoff_ms.
    const auto cap = exhausted_min_dirty_retry_attempts_cap_.load(std::memory_order_relaxed);
    if (cap > 0) {
        exhausted_min_dirty_retry_attempts_left_.store(cap, std::memory_order_relaxed);
        exhausted_min_dirty_retry_last_reason_.store(static_cast<std::uint8_t>(reason),
                                                     std::memory_order_relaxed);
        exhausted_min_dirty_retry_last_at_ms_.store(0, std::memory_order_relaxed);
    }
}

void HotUpdateRegistry::on_live_closure_remap(std::uint64_t count) noexcept {
    if (count == 0)
        return;
    live_closure_remap_.fetch_add(count, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_region_mask_adapt_clear(std::uint64_t /*region*/) noexcept {
    region_mask_adapt_clears_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_region_mask_adapt_restore(std::uint64_t /*region*/) noexcept {
    region_mask_adapt_restores_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_region_mask_from_dirty(std::uint64_t mask) noexcept {
    region_mask_from_dirty_total_.fetch_add(1, std::memory_order_relaxed);
    last_region_mask_from_dirty_.store(mask, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_cascade_reemit_trigger(std::uint64_t /*candidates_hint*/) noexcept {
    cascade_reemit_trigger_total_.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2014 / #2132: sliding-window deopt rate. Under threshold this is:
//   1× fetch_add (observed) + 1× load start + 1× load window + branch.
// Clock is read only when the window may have rolled or on first deopt.
// Soft storm (threshold) sets reemit_throttled_; hard ceiling (default 4×)
// sets hard_storm_active_ so critical-region bypass is disabled.
void HotUpdateRegistry::on_stale_deopt() noexcept {
    deopt_observed_total_.fetch_add(1, std::memory_order_relaxed);

    const std::uint64_t window_ms = deopt_storm_window_ms_.load(std::memory_order_relaxed);
    const std::uint64_t threshold = deopt_storm_threshold_.load(std::memory_order_relaxed);
    // Defensive: zero window/threshold means detection disabled (zero overhead path).
    if (window_ms == 0 || threshold == 0)
        return;

    const std::uint64_t now = steady_ms_now();
    std::uint64_t start = deopt_window_start_ms_.load(std::memory_order_relaxed);

    if (start == 0 || now < start || (now - start) >= window_ms) {
        // Open a new window. Concurrent openers may race; last writer wins
        // on start, and counts restart — acceptable for rate estimation.
        deopt_window_start_ms_.store(now, std::memory_order_relaxed);
        deopt_window_count_.store(1, std::memory_order_relaxed);
        // Fresh window clears reemit throttle so recovery can proceed.
        reemit_throttled_.store(false, std::memory_order_relaxed);
        hard_storm_active_.store(false, std::memory_order_relaxed);
        return;
    }

    const std::uint64_t n = deopt_window_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n < threshold)
        return;

    // Trip soft throttle for the remainder of this window.
    reemit_throttled_.store(true, std::memory_order_relaxed);
    // Count storm once when the threshold is first crossed.
    if (n == threshold) {
        deopt_storm_detected_.fetch_add(1, std::memory_order_relaxed);
        notify_deopt_storm_locked(n, window_ms);
    }

    // Issue #2132: hard ceiling — no critical bypass once crossed.
    std::uint64_t hard_thr = hard_deopt_storm_threshold_.load(std::memory_order_relaxed);
    if (hard_thr == 0) {
        // Auto: 4× soft threshold (at least soft+1).
        hard_thr = threshold * 4;
        if (hard_thr <= threshold)
            hard_thr = threshold + 1;
    }
    if (n >= hard_thr) {
        const bool was = hard_storm_active_.exchange(true, std::memory_order_relaxed);
        if (!was)
            hard_storm_detected_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool HotUpdateRegistry::should_throttle_reemit() const noexcept {
    // Process-global soft (or hard) storm flag — StormLevel / legacy callers.
    return reemit_throttled_.load(std::memory_order_relaxed) ||
           hard_storm_active_.load(std::memory_order_relaxed);
}

bool HotUpdateRegistry::is_critical_region(std::uint64_t region_or_priority) const noexcept {
    const auto mask = critical_region_mask_.load(std::memory_order_relaxed);
    return mask != 0 && region_or_priority != 0 && (region_or_priority & mask) != 0;
}

bool HotUpdateRegistry::hard_storm_active() const noexcept {
    return hard_storm_active_.load(std::memory_order_relaxed);
}

bool HotUpdateRegistry::should_throttle_reemit(std::uint64_t region_or_priority) const noexcept {
    // Issue #2132: hard ceiling always throttles (critical cannot bypass).
    if (hard_storm_active_.load(std::memory_order_relaxed))
        return true;
    const auto mode = storm_isolation_mode();
    if (mode == StormIsolation::Global) {
        if (!reemit_throttled_.load(std::memory_order_relaxed))
            return false;
        // Soft storm: critical region / priority bits may still reemit.
        if (is_critical_region(region_or_priority))
            return false;
        return true;
    }
    // Issue #2236 / #2370: PerRegion + PerEval. Critical bypass first
    // (still global per #2132). Then read this region's window; under
    // PerEval fold TLS storm eval context into the key (#2370).
    if (is_critical_region(region_or_priority))
        return false;
    std::uint64_t key = region_or_priority;
    if (mode == StormIsolation::PerEval) {
        const auto eval_key = reinterpret_cast<std::uintptr_t>(aura_get_storm_eval_context());
        key = (eval_key << 16) ^ region_or_priority;
        if (key == 0)
            key = 1;
    }
    RegionWindow* w = nullptr;
    {
        // Issue #2316: wire HotUpdateRegistry region_windows_mtx_
        // acquisition to lock_order audit (Level::HotUpdate rank).
        (void)::aura::compiler::lock_order::on_acquire(
            ::aura::compiler::lock_order::Level::HotUpdate, __builtin_FILE(), __builtin_LINE());
        std::lock_guard<std::mutex> lock(region_windows_mtx_);
        auto it = region_windows_.find(key);
        if (it != region_windows_.end() && it->second)
            w = it->second.get();
    }
    if (!w)
        return reemit_throttled_.load(std::memory_order_relaxed);
    if (w->hard_throttled_.load(std::memory_order_relaxed))
        return true;
    return w->soft_throttled_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reemit_throttled() noexcept {
    on_reemit_throttled(ThrottleReason::Global);
}

void HotUpdateRegistry::on_reemit_throttled(ThrottleReason reason) noexcept {
    reemit_throttle_skips_.fetch_add(1, std::memory_order_relaxed);
    switch (reason) {
        case ThrottleReason::Global:
            reemit_throttle_skips_global_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ThrottleReason::Region:
            reemit_throttle_skips_region_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ThrottleReason::Hard:
            reemit_throttle_skips_hard_.fetch_add(1, std::memory_order_relaxed);
            break;
        case ThrottleReason::CriticalBypass:
            // Not a skip — use on_reemit_critical_bypass instead.
            break;
    }
}

void HotUpdateRegistry::on_reemit_critical_bypass() noexcept {
    reemit_critical_bypass_.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2236 / #2370: StormIsolation mode setter / getter. Default =
// Global (process-wide window). PerRegion = per-region windows (cap 64).
// PerEval (#2370): per-eval windows keyed by TLS storm eval context
// (aura_set_storm_eval_context); SpecJIT isolation epoch is per-controller.
void HotUpdateRegistry::set_storm_isolation_mode(StormIsolation mode) noexcept {
    storm_isolation_mode_.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
}

HotUpdateRegistry::StormIsolation HotUpdateRegistry::storm_isolation_mode() const noexcept {
    return static_cast<StormIsolation>(storm_isolation_mode_.load(std::memory_order_relaxed));
}

// Issue #2274: cap overflow bumper + getter impls.
void HotUpdateRegistry::bump_deopt_storm_region_overflow_total() noexcept {
    deopt_storm_region_overflow_total_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::deopt_storm_region_overflow_total() const noexcept {
    return deopt_storm_region_overflow_total_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::storm_isolation_region_count() const noexcept {
    std::lock_guard<std::mutex> lock(region_windows_mtx_);
    return static_cast<std::uint64_t>(region_windows_.size());
}

std::uint64_t HotUpdateRegistry::deopt_storm_region_last_id() const noexcept {
    return deopt_storm_region_last_id_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::deopt_storm_region_detected_total() const noexcept {
    return deopt_storm_region_detected_total_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::reset_region_storm_windows_for_test() noexcept {
    std::lock_guard<std::mutex> lock(region_windows_mtx_);
    region_windows_.clear();
    deopt_storm_region_last_id_.store(0, std::memory_order_relaxed);
    // deopt_storm_region_detected_total_ is cumulative lifetime count — keep.
}

void HotUpdateRegistry::test_pump_deopt_for_region(std::uint64_t region, std::uint64_t n) noexcept {
    if (n == 0)
        return;
    const auto threshold = deopt_storm_threshold_.load(std::memory_order_relaxed);
    const auto window_ms = deopt_storm_window_ms_.load(std::memory_order_relaxed);
    if (window_ms == 0 || threshold == 0)
        return;
    auto hard_thr = hard_deopt_storm_threshold_.load(std::memory_order_relaxed);
    if (hard_thr == 0) {
        hard_thr = threshold * 4;
        if (hard_thr <= threshold)
            hard_thr = threshold + 1;
    }
    deopt_observed_total_.fetch_add(n, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(region_windows_mtx_);
    auto it = region_windows_.find(region);
    if (it == region_windows_.end()) {
        if (region_windows_.size() >= kStormIsolationRegionCap)
            return; // overflow → drop (matches issue AC2 "Cap map size; overflow falls back to
                    // global")
        it = region_windows_.emplace(region, std::make_unique<RegionWindow>()).first;
    }
    if (!it->second)
        return;
    feed_region_deopt_locked(*it->second, n, threshold, window_ms, hard_thr, region);
}

bool HotUpdateRegistry::feed_region_deopt_locked(RegionWindow& w, std::uint64_t n,
                                                 std::uint64_t threshold, std::uint64_t window_ms,
                                                 std::uint64_t hard_thr,
                                                 std::uint64_t region) noexcept {
    const auto now = steady_ms_now();
    auto start = w.window_start_ms_.load(std::memory_order_relaxed);
    if (start == 0 || now < start || (now - start) >= window_ms) {
        w.window_start_ms_.store(now, std::memory_order_relaxed);
        w.window_count_.store(n, std::memory_order_relaxed);
        w.soft_throttled_.store(false, std::memory_order_relaxed);
        w.hard_throttled_.store(false, std::memory_order_relaxed);
        return false;
    }
    const auto cnt = w.window_count_.fetch_add(n, std::memory_order_relaxed) + n;
    if (cnt < threshold)
        return false;
    if (cnt >= threshold && (cnt - n) < threshold) {
        // First crossing of the soft threshold for this window.
        w.soft_throttled_.store(true, std::memory_order_relaxed);
        deopt_storm_region_detected_total_.fetch_add(1, std::memory_order_relaxed);
        deopt_storm_region_last_id_.store(region, std::memory_order_relaxed);
    }
    if (cnt >= hard_thr)
        w.hard_throttled_.store(true, std::memory_order_relaxed);
    return true;
}

// Issue #2236 / #2370: region/eval-aware feed. Global → process-wide
// window. PerRegion → per-region window. PerEval (#2370) → per-eval
// window keyed by (TLS storm eval context XOR region) so concurrent
// evals do not share sliding windows.
void HotUpdateRegistry::on_stale_deopt(std::uint64_t region) noexcept {
    const auto mode = storm_isolation_mode();
    if (mode == StormIsolation::Global || region == 0) {
        on_stale_deopt();
        return;
    }
    // Issue #2370: under PerEval, fold TLS eval context into the key so
    // two evals using the same region id do not share a storm window.
    if (mode == StormIsolation::PerEval) {
        const auto eval_key = reinterpret_cast<std::uintptr_t>(aura_get_storm_eval_context());
        // Mix eval identity into the high bits; keep region in low bits.
        region = (eval_key << 16) ^ region;
        if (region == 0)
            region = 1; // avoid Global fallback for empty TLS
    }
    const auto threshold = deopt_storm_threshold_.load(std::memory_order_relaxed);
    const auto window_ms = deopt_storm_window_ms_.load(std::memory_order_relaxed);
    if (window_ms == 0 || threshold == 0)
        return; // detection disabled
    auto hard_thr = hard_deopt_storm_threshold_.load(std::memory_order_relaxed);
    if (hard_thr == 0) {
        hard_thr = threshold * 4;
        if (hard_thr <= threshold)
            hard_thr = threshold + 1;
    }
    deopt_observed_total_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(region_windows_mtx_);
    auto it = region_windows_.find(region);
    if (it == region_windows_.end()) {
        if (region_windows_.size() >= kStormIsolationRegionCap) {
            // Issue #2274: cap overflow counter (Agent-visible fallback to global).
            bump_deopt_storm_region_overflow_total();
            // Overflow: skip per-region entry (cap); callers use Global path.
            return;
        }
        it = region_windows_.emplace(region, std::make_unique<RegionWindow>()).first;
    }
    if (!it->second)
        return;
    feed_region_deopt_locked(*it->second, 1, threshold, window_ms, hard_thr, region);
}

// Issue #2094: ShapeProfiler publishes its deopt_storm_active state
// here so current_storm_level() can OR both detectors without
// importing shape_profiler.h. Tests can call this directly to
// simulate shape-only storms (AC2) without spinning up the
// profile machinery.
void HotUpdateRegistry::set_shape_storm_active(bool active) noexcept {
    shape_storm_active_.store(active, std::memory_order_release);
}

bool HotUpdateRegistry::shape_storm_active() const noexcept {
    return shape_storm_active_.load(std::memory_order_acquire);
}

// Issue #2302: accessor for the 5-field ReloadRecovery state.
// Reads all 5 atomics in a single sweep so the query primitive
// gets a coherent view (relaxed loads — safe because the
// individual atomics are independently meaningful even if
// temporally skewed across the 5 reads; queries are advisory).
HotUpdateRegistry::ReloadRecoveryState HotUpdateRegistry::reload_recovery_state() const noexcept {
    ReloadRecoveryState s;
    s.attempts_left = attempts_left_.load(std::memory_order_relaxed);
    s.force_jit_regions_mask = force_jit_regions_mask_.load(std::memory_order_relaxed);
    s.last_reason = last_aot_reload_fail_reason_.load(std::memory_order_relaxed);
    s.pending_dirty_count = pending_dirty_count_.load(std::memory_order_relaxed);
    s.deferred_reemit_pending = deferred_reemit_pending_v2_.load(std::memory_order_relaxed);
    return s;
}

// Issue #2302: C-linkage reader for the pending-dirty count
// (Agent dashboard / linter helpers).
extern "C" std::uint64_t aura_hot_update_recovery_pending_dirty_total_v_read(void) {
    return aura::compiler::hot_update_registry().pending_dirty_count();
}

// Issue #2367: agent-facing ReloadRecovery snapshot. Pure relaxed
// atomic loads — free when idle (no alloc, no locks). recovery_active
// collapses "is anything pending?" into a single branch key.
extern "C" void aura_hot_update_reload_recovery_get_snapshot(aura_reload_recovery_snapshot* out) {
    if (!out)
        return;
    auto& reg = aura::compiler::hot_update_registry();
    const auto rs = reg.reload_recovery_state();
    const auto snap = reg.snapshot();
    out->schema = 2367;
    out->issue = 2367;
    out->attempts_left = static_cast<std::int64_t>(rs.attempts_left);
    out->force_jit_regions_mask = static_cast<std::int64_t>(rs.force_jit_regions_mask);
    out->last_reason = static_cast<std::int64_t>(rs.last_reason);
    out->pending_dirty_count = static_cast<std::int64_t>(rs.pending_dirty_count);
    out->deferred_reemit_pending = static_cast<std::int64_t>(rs.deferred_reemit_pending);
    out->storm_level = snap.storm_level;
    out->reemit_boundary_policy = snap.reemit_boundary_policy;
    out->emit_region_mask = snap.emit_region_mask;
    out->critical_region_mask = snap.critical_region_mask;
    out->storm_isolation_mode = snap.storm_isolation_mode;
    out->deopt_storm_region_last_id = snap.deopt_storm_region_last_id;
    out->deopt_storm_region_detected_total = snap.deopt_storm_region_detected_total;
    out->hard_storm_active = snap.hard_storm_active;
    out->reemit_deferred_pending_boundary = snap.reemit_deferred_pending;
    out->last_force_jit_reason = static_cast<std::int64_t>(reg.last_force_jit_reason());
    out->force_jit_for_reason_total = static_cast<std::int64_t>(reg.force_jit_for_reason_total());
    out->last_force_jit_at_epoch_notify =
        static_cast<std::int64_t>(reg.last_force_jit_at_epoch_notify());
    out->epoch_notify_total = snap.epoch_notify_total;
    // Issue #2502: re-promote window + totals (additive; zero when idle).
    out->force_jit_repromote_total = static_cast<std::int64_t>(reg.force_jit_repromote_total());
    out->last_force_jit_repromote_reason =
        static_cast<std::int64_t>(reg.last_force_jit_repromote_reason());
    out->last_force_jit_repromote_at_epoch_notify =
        static_cast<std::int64_t>(reg.last_force_jit_repromote_at_epoch_notify());
    out->force_jit_stable_successes = static_cast<std::int64_t>(reg.force_jit_stable_successes());
    out->force_jit_repromote_window = static_cast<std::int64_t>(reg.force_jit_repromote_window());
    out->force_jit_repromote_require_pending_idle =
        reg.force_jit_repromote_require_pending_idle() ? 1 : 0;
    out->schema_2502 = 2502;
    // Issue #2601: exhausted min-dirty retry closed loop.
    out->aot_exhausted_min_dirty_retry_total =
        static_cast<std::int64_t>(reg.aot_exhausted_min_dirty_retry_total());
    out->aot_exhausted_min_dirty_retry_success_total =
        static_cast<std::int64_t>(reg.aot_exhausted_min_dirty_retry_success_total());
    out->aot_exhausted_min_dirty_retry_storm_skip_total =
        static_cast<std::int64_t>(reg.aot_exhausted_min_dirty_retry_storm_skip_total());
    out->aot_exhausted_min_dirty_retry_cap_hit_total =
        static_cast<std::int64_t>(reg.aot_exhausted_min_dirty_retry_cap_hit_total());
    out->exhausted_min_dirty_retry_attempts_left =
        static_cast<std::int64_t>(reg.exhausted_min_dirty_retry_attempts_left());
    out->exhausted_min_dirty_retry_attempts_cap =
        static_cast<std::int64_t>(reg.exhausted_min_dirty_retry_cap());
    out->exhausted_min_dirty_retry_backoff_ms =
        static_cast<std::int64_t>(reg.exhausted_min_dirty_retry_backoff_ms());
    out->exhausted_min_dirty_retry_last_at_ms =
        static_cast<std::int64_t>(reg.exhausted_min_dirty_retry_last_at_ms());
    out->exhausted_min_dirty_retry_last_reason =
        static_cast<std::int64_t>(reg.exhausted_min_dirty_retry_last_reason());
    out->force_jit_repromote_allow_pending_idle_when_force_jit_covered =
        reg.force_jit_repromote_allow_pending_idle_when_force_jit_covered() ? 1 : 0;
    out->schema_2601 = 2601;
    const bool active = rs.attempts_left != 0 || rs.force_jit_regions_mask != 0 ||
                        rs.pending_dirty_count != 0 || rs.deferred_reemit_pending != 0 ||
                        snap.storm_level != 0 || snap.reemit_deferred_pending != 0 ||
                        reg.exhausted_min_dirty_retry_attempts_left() != 0;
    out->recovery_active = active ? 1 : 0;
    out->reload_recovery_wired = 1;
}

// Issue #2094: unified StormLevel facade. Combines the global
// deopt storm (should_throttle_reemit) with the shape storm
// (shape_storm_active) into a single bitmask for downstream
// consumers (SpecJITController, reemit entry, Agent dashboards).
// Policy table documented in hot_update_registry.hh.
HotUpdateRegistry::StormLevel HotUpdateRegistry::current_storm_level() const noexcept {
    const bool g = should_throttle_reemit();
    const bool s = shape_storm_active();
    return static_cast<StormLevel>((g ? 2 : 0) | (s ? 1 : 0));
}

void HotUpdateRegistry::set_deopt_storm_threshold(std::uint64_t deopts_per_window,
                                                  std::uint64_t window_ms) noexcept {
    deopt_storm_threshold_.store(deopts_per_window, std::memory_order_relaxed);
    deopt_storm_window_ms_.store(window_ms, std::memory_order_relaxed);
}

void HotUpdateRegistry::set_hard_deopt_storm_threshold(std::uint64_t deopts_per_window) noexcept {
    hard_deopt_storm_threshold_.store(deopts_per_window, std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::hard_deopt_storm_threshold() const noexcept {
    return hard_deopt_storm_threshold_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::set_critical_region_mask(std::uint64_t mask) noexcept {
    critical_region_mask_.store(mask, std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::critical_region_mask() const noexcept {
    return critical_region_mask_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::deopt_storm_threshold() const noexcept {
    return deopt_storm_threshold_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::deopt_storm_window_ms() const noexcept {
    return deopt_storm_window_ms_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::reset_deopt_storm_state_for_test() noexcept {
    deopt_window_start_ms_.store(0, std::memory_order_relaxed);
    deopt_window_count_.store(0, std::memory_order_relaxed);
    reemit_throttled_.store(false, std::memory_order_relaxed);
    hard_storm_active_.store(false, std::memory_order_relaxed);
    // Keep lifetime counters (detected / observed / skips / bypass) for dashboards.
}

// ── Issue #2114: reemit ↔ MutationBoundary handshake ──
void HotUpdateRegistry::set_reemit_boundary_policy(ReemitBoundaryPolicy p) noexcept {
    reemit_boundary_policy_.store(static_cast<int>(p), std::memory_order_relaxed);
}

HotUpdateRegistry::ReemitBoundaryPolicy HotUpdateRegistry::reemit_boundary_policy() const noexcept {
    return static_cast<ReemitBoundaryPolicy>(
        reemit_boundary_policy_.load(std::memory_order_relaxed));
}

bool HotUpdateRegistry::soft_enter_allowed() const noexcept {
    // Issue #2205 / #2208: SoftEnter only when policy is SoftEnter (explicit
    // opt-in via setter or AURA_REEMIT_SOFT_ENTER under production defaults).
    // TLS soft boundary is not steal-safe — never allow SoftEnter when
    // production policy is Defer/RequireRealBoundary.
    return reemit_boundary_policy() == ReemitBoundaryPolicy::SoftEnter;
}

bool HotUpdateRegistry::in_mutation_boundary_for_reemit() const noexcept {
    // Soft reemit boundary (this thread mid-reemit soft-enter).
    if (g_soft_reemit_boundary_depth > 0)
        return true;
    // Real MutationBoundary stack depth (enter/exit_mutation_boundary).
    if (aura_evaluator_mutation_boundary_depth() > 0)
        return true;
    // Outermost Guard held flag — still true during #2090 dtor reemit
    // (held cleared only after reemit pipeline).
    if (aura_evaluator_mutation_boundary_held() != 0)
        return true;
    return false;
}

void HotUpdateRegistry::soft_reemit_boundary_enter() noexcept {
    ++g_soft_reemit_boundary_depth;
}

void HotUpdateRegistry::soft_reemit_boundary_exit() noexcept {
    if (g_soft_reemit_boundary_depth > 0)
        --g_soft_reemit_boundary_depth;
}

int HotUpdateRegistry::soft_reemit_boundary_depth() const noexcept {
    return g_soft_reemit_boundary_depth;
}

void HotUpdateRegistry::on_reemit_outside_boundary() noexcept {
    reemit_outside_boundary_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reemit_soft_boundary_entered() noexcept {
    reemit_soft_boundary_entered_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reemit_deferred_for_boundary() noexcept {
    reemit_deferred_for_boundary_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reemit_rejected_require_real() noexcept {
    reemit_rejected_require_real_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::defer_reemit_for_boundary(std::uint64_t defuse_version) noexcept {
    reemit_deferred_version_.store(defuse_version, std::memory_order_relaxed);
    reemit_deferred_pending_.store(true, std::memory_order_release);
    on_reemit_deferred_for_boundary();
}

bool HotUpdateRegistry::has_deferred_reemit() const noexcept {
    return reemit_deferred_pending_.load(std::memory_order_acquire);
}

// Issue #2273: steal-path observability bumper.
void HotUpdateRegistry::on_deferred_reemit_seen_on_steal(std::int64_t fiber_id) noexcept {
    // Issue #2302: mark deferred_reemit_pending so the unified
    // recovery state reflects the steal-path reemit deferral.
    deferred_reemit_pending_v2_.store(1, std::memory_order_relaxed);
    // existing impl continues below
    reemit_deferred_seen_on_steal_total_.fetch_add(1, std::memory_order_relaxed);
    reemit_deferred_seen_on_steal_last_fiber_id_.store(fiber_id, std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::take_deferred_reemit_version() noexcept {
    if (!reemit_deferred_pending_.exchange(false, std::memory_order_acq_rel))
        return 0;
    return reemit_deferred_version_.exchange(0, std::memory_order_relaxed);
}

void HotUpdateRegistry::reset_reemit_boundary_handshake_for_test() noexcept {
    // Issue #2205 / #2208: reset to production default Defer (not SoftEnter).
    reemit_boundary_policy_.store(static_cast<int>(ReemitBoundaryPolicy::Defer),
                                  std::memory_order_relaxed);
    reemit_outside_boundary_.store(0, std::memory_order_relaxed);
    reemit_soft_boundary_entered_.store(0, std::memory_order_relaxed);
    reemit_deferred_for_boundary_.store(0, std::memory_order_relaxed);
    reemit_rejected_require_real_.store(0, std::memory_order_relaxed);
    reemit_deferred_pending_.store(false, std::memory_order_relaxed);
    reemit_deferred_version_.store(0, std::memory_order_relaxed);
    // Clear TLS soft depth for this thread (test isolation).
    g_soft_reemit_boundary_depth = 0;
}

void HotUpdateRegistry::set_emit_region_mask(std::uint64_t mask) noexcept {
    // C path calls on_emit_region_mask_set (single bookkeeping site).
    aura_set_aot_emit_region_mask(mask);
}

std::uint64_t HotUpdateRegistry::emit_region_mask() const noexcept {
    return emit_region_mask_.load(std::memory_order_relaxed);
}

std::uint64_t HotUpdateRegistry::register_epoch_listener(EpochListener fn) {
    // Issue #2316: wire HotUpdateRegistry listeners_mtx_ acquisition
    // to lock_order audit (Level::HotUpdate rank).
    (void)::aura::compiler::lock_order::on_acquire(::aura::compiler::lock_order::Level::HotUpdate,
                                                   __builtin_FILE(), __builtin_LINE());
    std::lock_guard<std::mutex> lock(listeners_mtx_);
    epoch_listeners_.push_back(std::move(fn));
    register_calls_.fetch_add(1, std::memory_order_relaxed);
    return next_listener_id_++;
}

std::uint64_t HotUpdateRegistry::register_dirty_listener(DirtyListener fn) {
    std::lock_guard<std::mutex> lock(listeners_mtx_);
    dirty_listeners_.push_back(std::move(fn));
    register_calls_.fetch_add(1, std::memory_order_relaxed);
    return next_listener_id_++;
}

std::uint64_t HotUpdateRegistry::register_storm_listener(StormListener fn) {
    std::lock_guard<std::mutex> lock(listeners_mtx_);
    storm_listeners_.push_back(std::move(fn));
    register_calls_.fetch_add(1, std::memory_order_relaxed);
    return next_listener_id_++;
}

void HotUpdateRegistry::clear_listeners() noexcept {
    std::lock_guard<std::mutex> lock(listeners_mtx_);
    epoch_listeners_.clear();
    dirty_listeners_.clear();
    storm_listeners_.clear();
}

void HotUpdateRegistry::notify_epoch_bump(std::uint64_t epoch) noexcept {
    epoch_notify_.fetch_add(1, std::memory_order_relaxed);
    std::vector<EpochListener> copy;
    {
        std::lock_guard<std::mutex> lock(listeners_mtx_);
        copy = epoch_listeners_;
    }
    for (auto& fn : copy) {
        if (fn) {
            try {
                fn(epoch);
            } catch (...) {
                // [SILENCE-PRIM-#1956] listener errors must not poison hot-update path
            }
        }
    }
}

void HotUpdateRegistry::notify_dirty_define(const char* name) noexcept {
    dirty_notify_.fetch_add(1, std::memory_order_relaxed);
    std::vector<DirtyListener> copy;
    {
        std::lock_guard<std::mutex> lock(listeners_mtx_);
        copy = dirty_listeners_;
    }
    for (auto& fn : copy) {
        if (fn) {
            try {
                fn(name);
            } catch (...) {
                // [SILENCE-PRIM-#1956] listener errors must not poison hot-update path
            }
        }
    }
}

void HotUpdateRegistry::notify_deopt_storm_locked(std::uint64_t deopts_in_window,
                                                  std::uint64_t window_ms) noexcept {
    std::vector<StormListener> storm_copy;
    std::vector<EpochListener> epoch_copy;
    {
        std::lock_guard<std::mutex> lock(listeners_mtx_);
        storm_copy = storm_listeners_;
        epoch_copy = epoch_listeners_;
    }
    for (auto& fn : storm_copy) {
        if (fn) {
            try {
                fn(deopts_in_window, window_ms);
            } catch (...) {
                // [SILENCE-PRIM-#2014] storm listener errors must not poison deopt path
            }
        }
    }
    // Also fan-out epoch listeners with the deopt-storm sentinel so agents
    // that only subscribe to epoch bumps can still coalesce recovery.
    if (!epoch_copy.empty()) {
        epoch_notify_.fetch_add(1, std::memory_order_relaxed);
        for (auto& fn : epoch_copy) {
            if (fn) {
                try {
                    fn(kHotUpdateDeoptStormEpoch);
                } catch (...) {
                    // [SILENCE-PRIM-#2014]
                }
            }
        }
    }
}

HotUpdateRegistry::Snapshot HotUpdateRegistry::snapshot() const noexcept {
    Snapshot s;
    s.reemit_provider_wired = reemit_wired_.load(std::memory_order_relaxed) ? 1 : 0;
    s.define_dirty_provider_wired = define_dirty_wired_.load(std::memory_order_relaxed) ? 1 : 0;
    s.aot_emit_provider_wired = aot_emit_wired_.load(std::memory_order_relaxed) ? 1 : 0;
    s.emit_region_mask =
        static_cast<std::int64_t>(emit_region_mask_.load(std::memory_order_relaxed));
    {
        std::lock_guard<std::mutex> lock(listeners_mtx_);
        s.epoch_listeners = static_cast<std::int64_t>(epoch_listeners_.size());
        s.dirty_listeners = static_cast<std::int64_t>(dirty_listeners_.size());
        s.storm_listeners = static_cast<std::int64_t>(storm_listeners_.size());
    }
    s.register_calls_total =
        static_cast<std::int64_t>(register_calls_.load(std::memory_order_relaxed));
    s.epoch_notify_total = static_cast<std::int64_t>(epoch_notify_.load(std::memory_order_relaxed));
    s.dirty_notify_total = static_cast<std::int64_t>(dirty_notify_.load(std::memory_order_relaxed));
    s.reemit_pipeline_calls_total =
        static_cast<std::int64_t>(reemit_pipeline_calls_.load(std::memory_order_relaxed));
    s.reemit_candidates_total =
        static_cast<std::int64_t>(reemit_candidates_.load(std::memory_order_relaxed));
    s.reemit_success_total =
        static_cast<std::int64_t>(reemit_success_.load(std::memory_order_relaxed));
    s.stable_id_preserve_total =
        static_cast<std::int64_t>(stable_id_preserve_.load(std::memory_order_relaxed));
    s.stable_id_assign_total =
        static_cast<std::int64_t>(stable_id_assign_.load(std::memory_order_relaxed));
    s.stable_func_id_map_size = static_cast<std::int64_t>(aura_stable_func_id_map_size());
    s.aot_reload_success_total =
        static_cast<std::int64_t>(aot_reload_success_.load(std::memory_order_relaxed));
    s.aot_reload_rollback_total =
        static_cast<std::int64_t>(aot_reload_rollback_.load(std::memory_order_relaxed));
    // Issue #2093: per-reason reload failure breakdown (refine #2012).
    s.aot_reload_fail_dlopen_total =
        static_cast<std::int64_t>(aot_reload_fail_dlopen_.load(std::memory_order_relaxed));
    s.aot_reload_fail_version_total =
        static_cast<std::int64_t>(aot_reload_fail_version_.load(std::memory_order_relaxed));
    s.aot_reload_fail_region_total =
        static_cast<std::int64_t>(aot_reload_fail_region_.load(std::memory_order_relaxed));
    s.aot_reload_fail_defuse_total =
        static_cast<std::int64_t>(aot_reload_fail_defuse_.load(std::memory_order_relaxed));
    s.aot_reload_fail_env_total =
        static_cast<std::int64_t>(aot_reload_fail_env_.load(std::memory_order_relaxed));
    s.aot_reload_fail_linear_total =
        static_cast<std::int64_t>(aot_reload_fail_linear_.load(std::memory_order_relaxed));
    s.aot_reload_fail_staging_total =
        static_cast<std::int64_t>(aot_reload_fail_staging_.load(std::memory_order_relaxed));
    s.aot_reload_fail_other_total =
        static_cast<std::int64_t>(aot_reload_fail_other_.load(std::memory_order_relaxed));
    s.aot_reload_last_fail_reason =
        static_cast<std::int64_t>(last_aot_reload_fail_reason_.load(std::memory_order_relaxed));
    s.live_closure_remap_total =
        static_cast<std::int64_t>(live_closure_remap_.load(std::memory_order_relaxed));
    s.deopt_storm_detected_total =
        static_cast<std::int64_t>(deopt_storm_detected_.load(std::memory_order_relaxed));
    s.deopt_observed_total =
        static_cast<std::int64_t>(deopt_observed_total_.load(std::memory_order_relaxed));
    s.deopt_window_count =
        static_cast<std::int64_t>(deopt_window_count_.load(std::memory_order_relaxed));
    s.deopt_storm_threshold =
        static_cast<std::int64_t>(deopt_storm_threshold_.load(std::memory_order_relaxed));
    s.deopt_storm_window_ms =
        static_cast<std::int64_t>(deopt_storm_window_ms_.load(std::memory_order_relaxed));
    s.reemit_throttle_active = (reemit_throttled_.load(std::memory_order_relaxed) ||
                                hard_storm_active_.load(std::memory_order_relaxed))
                                   ? 1
                                   : 0;
    s.reemit_throttle_skips_total =
        static_cast<std::int64_t>(reemit_throttle_skips_.load(std::memory_order_relaxed));
    // Issue #2132
    s.reemit_throttle_skips_global_total =
        static_cast<std::int64_t>(reemit_throttle_skips_global_.load(std::memory_order_relaxed));
    s.reemit_throttle_skips_region_total =
        static_cast<std::int64_t>(reemit_throttle_skips_region_.load(std::memory_order_relaxed));
    s.reemit_throttle_skips_hard_total =
        static_cast<std::int64_t>(reemit_throttle_skips_hard_.load(std::memory_order_relaxed));
    s.reemit_critical_bypass_total =
        static_cast<std::int64_t>(reemit_critical_bypass_.load(std::memory_order_relaxed));
    s.hard_storm_active = hard_storm_active_.load(std::memory_order_relaxed) ? 1 : 0;
    s.hard_storm_detected_total =
        static_cast<std::int64_t>(hard_storm_detected_.load(std::memory_order_relaxed));
    s.hard_deopt_storm_threshold =
        static_cast<std::int64_t>(hard_deopt_storm_threshold_.load(std::memory_order_relaxed));
    s.critical_region_mask =
        static_cast<std::int64_t>(critical_region_mask_.load(std::memory_order_relaxed));
    s.schema_2132 = 2132;
    s.issue_2132 = 2132;
    s.region_mask_adapt_clears_total =
        static_cast<std::int64_t>(region_mask_adapt_clears_.load(std::memory_order_relaxed));
    s.region_mask_adapt_restores_total =
        static_cast<std::int64_t>(region_mask_adapt_restores_.load(std::memory_order_relaxed));
    s.emit_region_mask_preferred =
        static_cast<std::int64_t>(aura_get_aot_emit_region_mask_preferred());
    // Issue #2035
    s.region_mask_from_dirty_total =
        static_cast<std::int64_t>(region_mask_from_dirty_total_.load(std::memory_order_relaxed));
    s.cascade_reemit_trigger_total =
        static_cast<std::int64_t>(cascade_reemit_trigger_total_.load(std::memory_order_relaxed));
    s.last_region_mask_from_dirty =
        static_cast<std::int64_t>(last_region_mask_from_dirty_.load(std::memory_order_relaxed));
    s.schema_2035 = 2035;
    s.issue_2035 = 2035;
    // Issue #2094: unified StormLevel facade (Shape|Global|Both).
    s.storm_level = static_cast<std::int64_t>(current_storm_level());
    // Issue #2114 / #2205 / #2208: reemit ↔ MutationBoundary handshake.
    s.reemit_outside_boundary_total =
        static_cast<std::int64_t>(reemit_outside_boundary_.load(std::memory_order_relaxed));
    s.reemit_soft_boundary_entered_total =
        static_cast<std::int64_t>(reemit_soft_boundary_entered_.load(std::memory_order_relaxed));
    s.reemit_deferred_for_boundary_total =
        static_cast<std::int64_t>(reemit_deferred_for_boundary_.load(std::memory_order_relaxed));
    s.reemit_boundary_policy =
        static_cast<std::int64_t>(reemit_boundary_policy_.load(std::memory_order_relaxed));
    s.reemit_deferred_pending = reemit_deferred_pending_.load(std::memory_order_relaxed) ? 1 : 0;
    // Issue #2273: steal-path observability fields.
    s.reemit_deferred_seen_on_steal_total = static_cast<std::int64_t>(
        reemit_deferred_seen_on_steal_total_.load(std::memory_order_relaxed));
    s.reemit_deferred_seen_on_steal_last_fiber_id =
        reemit_deferred_seen_on_steal_last_fiber_id_.load(std::memory_order_relaxed);
    s.reemit_rejected_require_real_total =
        static_cast<std::int64_t>(reemit_rejected_require_real_.load(std::memory_order_relaxed));
    s.schema_2114 = 2114;
    s.issue_2114 = 2114;
    s.schema_2205 = 2205;
    s.issue_2205 = 2205;
    s.schema_2208 = 2208;
    s.issue_2208 = 2208;
    // Issue #2236: StormIsolation mode + per-region storm trip counters.
    s.storm_isolation_mode = static_cast<std::int64_t>(storm_isolation_mode());
    // Issue #2274: cap overflow counter snapshot.
    s.deopt_storm_region_overflow_total = static_cast<std::int64_t>(
        deopt_storm_region_overflow_total_.load(std::memory_order_relaxed));
    s.deopt_storm_region_detected_total = static_cast<std::int64_t>(
        deopt_storm_region_detected_total_.load(std::memory_order_relaxed));
    s.deopt_storm_region_last_id =
        static_cast<std::int64_t>(deopt_storm_region_last_id_.load(std::memory_order_relaxed));
    s.schema_2236 = 2236;
    s.issue_2236 = 2236;
    // Issue #2601: exhausted min-dirty retry closed loop.
    s.aot_exhausted_min_dirty_retry_total = static_cast<std::int64_t>(
        aot_exhausted_min_dirty_retry_total_.load(std::memory_order_relaxed));
    s.aot_exhausted_min_dirty_retry_success_total = static_cast<std::int64_t>(
        aot_exhausted_min_dirty_retry_success_total_.load(std::memory_order_relaxed));
    s.aot_exhausted_min_dirty_retry_storm_skip_total = static_cast<std::int64_t>(
        aot_exhausted_min_dirty_retry_storm_skip_total_.load(std::memory_order_relaxed));
    s.aot_exhausted_min_dirty_retry_cap_hit_total = static_cast<std::int64_t>(
        aot_exhausted_min_dirty_retry_cap_hit_total_.load(std::memory_order_relaxed));
    s.exhausted_min_dirty_retry_attempts_left = static_cast<std::int64_t>(
        exhausted_min_dirty_retry_attempts_left_.load(std::memory_order_relaxed));
    s.exhausted_min_dirty_retry_attempts_cap = static_cast<std::int64_t>(
        exhausted_min_dirty_retry_attempts_cap_.load(std::memory_order_relaxed));
    s.exhausted_min_dirty_retry_backoff_ms = static_cast<std::int64_t>(
        exhausted_min_dirty_retry_backoff_ms_.load(std::memory_order_relaxed));
    s.exhausted_min_dirty_retry_last_at_ms = static_cast<std::int64_t>(
        exhausted_min_dirty_retry_last_at_ms_.load(std::memory_order_relaxed));
    s.exhausted_min_dirty_retry_last_reason = static_cast<std::int64_t>(
        exhausted_min_dirty_retry_last_reason_.load(std::memory_order_relaxed));
    s.force_jit_repromote_allow_pending_idle_when_force_jit_covered = static_cast<std::int64_t>(
        force_jit_repromote_allow_pending_idle_when_force_jit_covered_.load(
            std::memory_order_relaxed));
    // schema_2601 / issue_2601 are constexpr defaults in the struct.
    return s;
}

} // namespace aura::compiler

extern "C" void aura_hot_update_registry_get_snapshot(aura_hot_update_registry_snapshot* out) {
    if (!out)
        return;
    const auto s = aura::compiler::hot_update_registry().snapshot();
    out->schema = s.schema;
    out->issue = s.issue;
    out->active = s.active;
    out->reemit_provider_wired = s.reemit_provider_wired;
    out->define_dirty_provider_wired = s.define_dirty_provider_wired;
    out->aot_emit_provider_wired = s.aot_emit_provider_wired;
    out->emit_region_mask = s.emit_region_mask;
    out->epoch_listeners = s.epoch_listeners;
    out->dirty_listeners = s.dirty_listeners;
    out->register_calls_total = s.register_calls_total;
    out->epoch_notify_total = s.epoch_notify_total;
    out->dirty_notify_total = s.dirty_notify_total;
    out->reemit_pipeline_calls_total = s.reemit_pipeline_calls_total;
    out->reemit_candidates_total = s.reemit_candidates_total;
    out->reemit_success_total = s.reemit_success_total;
    out->stable_id_preserve_total = s.stable_id_preserve_total;
    out->stable_id_assign_total = s.stable_id_assign_total;
    out->stable_func_id_map_size = s.stable_func_id_map_size;
    out->aot_reload_success_total = s.aot_reload_success_total;
    out->aot_reload_rollback_total = s.aot_reload_rollback_total;
    // Issue #2093: per-reason reload failure breakdown (refine #2012).
    out->aot_reload_fail_dlopen_total = s.aot_reload_fail_dlopen_total;
    out->aot_reload_fail_version_total = s.aot_reload_fail_version_total;
    out->aot_reload_fail_region_total = s.aot_reload_fail_region_total;
    out->aot_reload_fail_defuse_total = s.aot_reload_fail_defuse_total;
    out->aot_reload_fail_env_total = s.aot_reload_fail_env_total;
    out->aot_reload_fail_linear_total = s.aot_reload_fail_linear_total;
    out->aot_reload_fail_staging_total = s.aot_reload_fail_staging_total;
    out->aot_reload_fail_other_total = s.aot_reload_fail_other_total;
    out->aot_reload_last_fail_reason = s.aot_reload_last_fail_reason;
    out->live_closure_remap_total = s.live_closure_remap_total;
    out->deopt_storm_detected_total = s.deopt_storm_detected_total;
    out->deopt_observed_total = s.deopt_observed_total;
    out->deopt_window_count = s.deopt_window_count;
    out->deopt_storm_threshold = s.deopt_storm_threshold;
    out->deopt_storm_window_ms = s.deopt_storm_window_ms;
    out->reemit_throttle_active = s.reemit_throttle_active;
    out->reemit_throttle_skips_total = s.reemit_throttle_skips_total;
    // Issue #2132
    out->reemit_throttle_skips_global_total = s.reemit_throttle_skips_global_total;
    out->reemit_throttle_skips_region_total = s.reemit_throttle_skips_region_total;
    out->reemit_throttle_skips_hard_total = s.reemit_throttle_skips_hard_total;
    out->reemit_critical_bypass_total = s.reemit_critical_bypass_total;
    out->hard_storm_active = s.hard_storm_active;
    out->hard_storm_detected_total = s.hard_storm_detected_total;
    out->hard_deopt_storm_threshold = s.hard_deopt_storm_threshold;
    out->critical_region_mask = s.critical_region_mask;
    out->schema_2132 = s.schema_2132;
    out->issue_2132 = s.issue_2132;
    out->storm_listeners = s.storm_listeners;
    out->region_mask_adapt_clears_total = s.region_mask_adapt_clears_total;
    out->region_mask_adapt_restores_total = s.region_mask_adapt_restores_total;
    out->emit_region_mask_preferred = s.emit_region_mask_preferred;
    // Issue #2035
    out->region_mask_from_dirty_total = s.region_mask_from_dirty_total;
    out->cascade_reemit_trigger_total = s.cascade_reemit_trigger_total;
    out->last_region_mask_from_dirty = s.last_region_mask_from_dirty;
    out->schema_2035 = s.schema_2035;
    out->issue_2035 = s.issue_2035;
    // Issue #2094: unified StormLevel facade (uint8_t enum, copied as int64_t).
    out->storm_level = s.storm_level;
    // Issue #2114 / #2205 / #2208
    out->reemit_outside_boundary_total = s.reemit_outside_boundary_total;
    out->reemit_soft_boundary_entered_total = s.reemit_soft_boundary_entered_total;
    out->reemit_deferred_for_boundary_total = s.reemit_deferred_for_boundary_total;
    out->reemit_boundary_policy = s.reemit_boundary_policy;
    out->reemit_deferred_pending = s.reemit_deferred_pending;
    out->reemit_rejected_require_real_total = s.reemit_rejected_require_real_total;
    out->schema_2114 = s.schema_2114;
    out->issue_2114 = s.issue_2114;
    out->schema_2205 = s.schema_2205;
    out->issue_2205 = s.issue_2205;
    out->schema_2208 = s.schema_2208;
    out->issue_2208 = s.issue_2208;
    // Issue #2236: StormIsolation mode + per-region storm trip counters.
    out->storm_isolation_mode = s.storm_isolation_mode;
    // Issue #2274: cap overflow counter in snapshot.
    out->deopt_storm_region_overflow_total = s.deopt_storm_region_overflow_total;
    out->deopt_storm_region_detected_total = s.deopt_storm_region_detected_total;
    out->deopt_storm_region_last_id = s.deopt_storm_region_last_id;
    out->schema_2236 = s.schema_2236;
    out->issue_2236 = s.issue_2236;
    // Issue #2601: exhausted min-dirty retry closed loop.
    out->aot_exhausted_min_dirty_retry_total = s.aot_exhausted_min_dirty_retry_total;
    out->aot_exhausted_min_dirty_retry_success_total =
        s.aot_exhausted_min_dirty_retry_success_total;
    out->aot_exhausted_min_dirty_retry_storm_skip_total =
        s.aot_exhausted_min_dirty_retry_storm_skip_total;
    out->aot_exhausted_min_dirty_retry_cap_hit_total =
        s.aot_exhausted_min_dirty_retry_cap_hit_total;
    out->exhausted_min_dirty_retry_attempts_left = s.exhausted_min_dirty_retry_attempts_left;
    out->exhausted_min_dirty_retry_attempts_cap = s.exhausted_min_dirty_retry_attempts_cap;
    out->exhausted_min_dirty_retry_backoff_ms = s.exhausted_min_dirty_retry_backoff_ms;
    out->exhausted_min_dirty_retry_last_at_ms = s.exhausted_min_dirty_retry_last_at_ms;
    out->exhausted_min_dirty_retry_last_reason = s.exhausted_min_dirty_retry_last_reason;
    out->force_jit_repromote_allow_pending_idle_when_force_jit_covered =
        s.force_jit_repromote_allow_pending_idle_when_force_jit_covered;
    out->schema_2601 = s.schema_2601;
    out->issue_2601 = s.issue_2601;
}

extern "C" void aura_hot_update_note_deopt(void) {
    aura::compiler::hot_update_registry().on_stale_deopt();
}

extern "C" int aura_hot_update_should_throttle_reemit(void) {
    return aura::compiler::hot_update_registry().should_throttle_reemit() ? 1 : 0;
}

// Issue #2132: region/priority-aware throttle decision.
extern "C" int aura_hot_update_should_throttle_reemit_for_region(std::uint64_t region_or_priority) {
    return aura::compiler::hot_update_registry().should_throttle_reemit(region_or_priority) ? 1 : 0;
}

extern "C" void aura_hot_update_set_critical_region_mask(std::uint64_t mask) {
    aura::compiler::hot_update_registry().set_critical_region_mask(mask);
}

extern "C" std::uint64_t aura_hot_update_critical_region_mask(void) {
    return aura::compiler::hot_update_registry().critical_region_mask();
}

extern "C" void aura_hot_update_set_hard_deopt_storm_threshold(std::uint64_t deopts_per_window) {
    aura::compiler::hot_update_registry().set_hard_deopt_storm_threshold(deopts_per_window);
}

extern "C" std::uint64_t aura_hot_update_hard_deopt_storm_threshold(void) {
    return aura::compiler::hot_update_registry().hard_deopt_storm_threshold();
}

// Issue #2094: StormLevel facade accessor (C ABI). Returns the
// combined bitmask of shape-storm + global-deopt-storm detectors.
extern "C" std::uint8_t aura_hot_update_current_storm_level(void) {
    return static_cast<std::uint8_t>(aura::compiler::hot_update_registry().current_storm_level());
}

// Issue #2236: StormIsolation mode setter / getter (C ABI). Default
// value is Global (=0) — preserves today's process-wide deopt-storm
// behavior. Set PerRegion (=1) to activate per-region sliding windows
// with bounded 64 cap; overflow falls back to global per the issue
// AC2 note. PerEval (=2) is real as of #2370 (TLS storm eval context +
// SpecJIT isolation epoch); PerRegion remains the multi-eval default
// when AURA_STORM_ISOLATION is unset.
extern "C" void aura_set_storm_isolation_mode(int mode) noexcept {
    aura::compiler::hot_update_registry().set_storm_isolation_mode(
        static_cast<aura::compiler::HotUpdateRegistry::StormIsolation>(mode));
}

extern "C" int aura_get_storm_isolation_mode(void) noexcept {
    return static_cast<int>(aura::compiler::hot_update_registry().storm_isolation_mode());
}

// Issue #2274: cap overflow C ABI.
extern "C" void aura_bump_deopt_storm_region_overflow_total(void) {
    aura::compiler::hot_update_registry().bump_deopt_storm_region_overflow_total();
}

extern "C" std::uint64_t aura_get_deopt_storm_region_overflow_total(void) {
    return aura::compiler::hot_update_registry().deopt_storm_region_overflow_total();
}

// Issue #2236: env resolver for AURA_STORM_ISOLATION. Reads at call
// time (no global cache) so runtime hosts can change the env without
// restarting. Accepts: "global" / "" (default), "region" / "per-region",
// "eval" / "per-eval". Invalid values fall back to Global (preserves
// backwards compat with a typo).
extern "C" void aura_apply_storm_isolation_env(void) noexcept {
    const char* env = std::getenv("AURA_STORM_ISOLATION");
    auto mode = aura::compiler::HotUpdateRegistry::StormIsolation::Global;
    if (env != nullptr) {
        if (std::strcmp(env, "region") == 0 || std::strcmp(env, "per-region") == 0)
            mode = aura::compiler::HotUpdateRegistry::StormIsolation::PerRegion;
        else if (std::strcmp(env, "eval") == 0 || std::strcmp(env, "per-eval") == 0)
            mode = aura::compiler::HotUpdateRegistry::StormIsolation::PerEval;
    }
    // Issue #2274: production default — if AURA_STORM_ISOLATION is unset
    // AND the host is multi-eval (aot_state_map_size > 1), auto-select
    // PerRegion to prevent cross-eval deopt storm cross-contamination.
    // Single-workspace MVP stays Global. env opt-in overrides this.
    if (env == nullptr && aura_aot_state_map_size() > 1) {
        mode = aura::compiler::HotUpdateRegistry::StormIsolation::PerRegion;
    }
    aura::compiler::hot_update_registry().set_storm_isolation_mode(mode);
}

// Issue #2236: per-region test helpers. Pump bumps a region's window
// count by `n` (skips the threshold-check atomic dance so tests are
// deterministic). Reset clears the region map + last-id (keeps the
// cumulative detected_total as lifetime observability).
extern "C" void aura_hot_update_registry_test_pump_deopt_for_region(std::uint64_t region,
                                                                    std::uint64_t n) noexcept {
    aura::compiler::hot_update_registry().test_pump_deopt_for_region(region, n);
}

extern "C" void aura_hot_update_registry_reset_region_for_test(void) noexcept {
    aura::compiler::hot_update_registry().reset_region_storm_windows_for_test();
}

// Issue #2094: setter for ShapeProfiler (or tests) to publish its
// deopt_storm_active state without importing shape_profiler.h.
extern "C" void aura_hot_update_set_shape_storm_active(int active) {
    aura::compiler::hot_update_registry().set_shape_storm_active(active != 0);
}

extern "C" void aura_hot_update_on_reemit_throttled(void) {
    aura::compiler::hot_update_registry().on_reemit_throttled();
}

// Issue #2604: outermost MutationBoundary exit auto-drain deferred
// reemit + one region-filtered pass. Bumped from
// evaluator_mutation_boundary.cpp exit_mutation_boundary success path.
// on_boundary_exit_total: outermost exit attempts.
// success_total: aura_reemit_aot_for_dirty returned >0.
// throttled_total: storm throttle / storm-skip blocked (defer re-pending
// per existing policy; no silent drop forever).
extern "C" void aura_bump_reemit_auto_drain_on_boundary_exit_total(void) {
    if (aot_metrics())
        aot_metrics()->reemit_auto_drain_on_boundary_exit_total.fetch_add(
            1, std::memory_order_relaxed);
}
extern "C" void aura_bump_reemit_auto_drain_success_total(void) {
    if (aot_metrics())
        aot_metrics()->reemit_auto_drain_success_total.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void aura_bump_reemit_auto_drain_throttled_total(void) {
    if (aot_metrics())
        aot_metrics()->reemit_auto_drain_throttled_total.fetch_add(1, std::memory_order_relaxed);
}

extern "C" void aura_hot_update_set_deopt_storm_threshold(std::uint64_t deopts_per_window,
                                                          std::uint64_t window_ms) {
    aura::compiler::hot_update_registry().set_deopt_storm_threshold(deopts_per_window, window_ms);
}

extern "C" void aura_hot_update_reset_deopt_storm_state_for_test(void) {
    aura::compiler::hot_update_registry().reset_deopt_storm_state_for_test();
}

// Issue #2017: C entry for compact-env-frames / other module-partition callers.
extern "C" void aura_hot_update_notify_epoch_bump(std::uint64_t epoch) {
    aura::compiler::hot_update_registry().notify_epoch_bump(epoch);
}

// Issue #2035: module-safe dirty notify for service_dirty cascade paths.
extern "C" void aura_hot_update_notify_dirty_define(const char* name) {
    aura::compiler::hot_update_registry().notify_dirty_define(name);
}

extern "C" int aura_hot_update_reemit_provider_wired(void) {
    return aura::compiler::hot_update_registry().reemit_provider_wired() ? 1 : 0;
}

// Issue #2114: reemit ↔ MutationBoundary handshake C ABI.
extern "C" int aura_hot_update_in_mutation_boundary_for_reemit(void) {
    return aura::compiler::hot_update_registry().in_mutation_boundary_for_reemit() ? 1 : 0;
}

extern "C" void aura_hot_update_set_reemit_boundary_policy(int policy) {
    using P = aura::compiler::HotUpdateRegistry::ReemitBoundaryPolicy;
    P p = P::Defer; // production default (#2205)
    if (policy == 0)
        p = P::SoftEnter;
    else if (policy == 2)
        p = P::RequireRealBoundary;
    else if (policy == 1)
        p = P::Defer;
    aura::compiler::hot_update_registry().set_reemit_boundary_policy(p);
}

extern "C" int aura_hot_update_get_reemit_boundary_policy(void) {
    return static_cast<int>(aura::compiler::hot_update_registry().reemit_boundary_policy());
}

extern "C" void aura_hot_update_reset_reemit_boundary_handshake_for_test(void) {
    aura::compiler::hot_update_registry().reset_reemit_boundary_handshake_for_test();
}

extern "C" int aura_hot_update_soft_reemit_boundary_active(void) {
    return aura::compiler::hot_update_registry().soft_reemit_boundary_depth() > 0 ? 1 : 0;
}

extern "C" int aura_hot_update_has_deferred_reemit(void) {
    return aura::compiler::hot_update_registry().has_deferred_reemit() ? 1 : 0;
}

// Issue #2273: C ABI bumper for steal-path observability.
extern "C" void aura_hot_update_on_deferred_reemit_seen_on_steal(std::int64_t fiber_id) {
    aura::compiler::hot_update_registry().on_deferred_reemit_seen_on_steal(fiber_id);
}

// Issue #2601: exhausted min-dirty retry closed loop C ABI.
extern "C" void aura_set_exhausted_min_dirty_retry_cap(std::uint32_t n) noexcept {
    aura::compiler::hot_update_registry().set_exhausted_min_dirty_retry_cap(n);
}

extern "C" std::uint32_t aura_get_exhausted_min_dirty_retry_cap(void) noexcept {
    return aura::compiler::hot_update_registry().exhausted_min_dirty_retry_cap();
}

extern "C" void aura_set_exhausted_min_dirty_retry_backoff_ms(std::uint64_t ms) noexcept {
    aura::compiler::hot_update_registry().set_exhausted_min_dirty_retry_backoff_ms(ms);
}

extern "C" std::uint64_t aura_get_exhausted_min_dirty_retry_backoff_ms(void) noexcept {
    return aura::compiler::hot_update_registry().exhausted_min_dirty_retry_backoff_ms();
}

extern "C" void
aura_set_force_jit_repromote_allow_pending_idle_when_force_jit_covered(int allow) noexcept {
    aura::compiler::hot_update_registry()
        .set_force_jit_repromote_allow_pending_idle_when_force_jit_covered(allow != 0);
}

extern "C" int
aura_get_force_jit_repromote_allow_pending_idle_when_force_jit_covered(void) noexcept {
    return aura::compiler::hot_update_registry()
                   .force_jit_repromote_allow_pending_idle_when_force_jit_covered()
               ? 1
               : 0;
}

extern "C" void aura_hot_update_reset_exhausted_min_dirty_retry_for_test(void) noexcept {
    aura::compiler::hot_update_registry().reset_exhausted_min_dirty_retry_for_test();
}
