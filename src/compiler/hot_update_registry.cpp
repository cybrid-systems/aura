// hot_update_registry.cpp — Issue #1956 / #2014 / #2114
// Process-wide HotUpdateRegistry singleton.

#include "compiler/hot_update_registry.hh"

#include "compiler/aura_jit_bridge.h"

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
}

void HotUpdateRegistry::on_reload_success() noexcept {
    aot_reload_success_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reload_rollback(AotReloadFail reason) noexcept {
    aot_reload_rollback_.fetch_add(1, std::memory_order_relaxed);
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
    last_aot_reload_fail_reason_.store(static_cast<std::uint8_t>(reason),
                                       std::memory_order_release);
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
    // Issue #2236: PerRegion (PerEval is plumbed but eval_id threading
    // is a #2158 follow-up; same code path for now). Critical bypass is
    // checked first (still global per #2132 contract — critical mask is
    // a single process-wide bitmask). Then we read this region's window
    // (if any); a region with no recorded window was never observed →
    // safe to fall through to the global flag.
    if (is_critical_region(region_or_priority))
        return false;
    RegionWindow* w = nullptr;
    {
        std::lock_guard<std::mutex> lock(region_windows_mtx_);
        auto it = region_windows_.find(region_or_priority);
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

// Issue #2236: StormIsolation mode setter / getter. Default = Global
// (process-wide window) — preserves today's behavior. Setting
// PerRegion activates per-region windows (bounded 64 cap). PerEval
// is plumbed but eval_id threading is a future #2158 follow-up;
// the current PerEval selection is the same code path as PerRegion
// (per-region surrogate until eval_id is threaded through).
void HotUpdateRegistry::set_storm_isolation_mode(StormIsolation mode) noexcept {
    storm_isolation_mode_.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
}

HotUpdateRegistry::StormIsolation HotUpdateRegistry::storm_isolation_mode() const noexcept {
    return static_cast<StormIsolation>(storm_isolation_mode_.load(std::memory_order_relaxed));
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

// Issue #2236: region-aware feed. When isolation mode is Global (default),
// routes to the no-arg on_stale_deopt() (process-wide window preserved).
// When PerRegion, feeds the per-region window; if the map cap (64) is
// reached AND the region is new, falls back to global to bound memory.
void HotUpdateRegistry::on_stale_deopt(std::uint64_t region) noexcept {
    const auto mode = storm_isolation_mode();
    if (mode == StormIsolation::Global || region == 0) {
        on_stale_deopt();
        return;
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
    s.deopt_storm_region_detected_total = static_cast<std::int64_t>(
        deopt_storm_region_detected_total_.load(std::memory_order_relaxed));
    s.deopt_storm_region_last_id =
        static_cast<std::int64_t>(deopt_storm_region_last_id_.load(std::memory_order_relaxed));
    s.schema_2236 = 2236;
    s.issue_2236 = 2236;
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
    out->deopt_storm_region_detected_total = s.deopt_storm_region_detected_total;
    out->deopt_storm_region_last_id = s.deopt_storm_region_last_id;
    out->schema_2236 = s.schema_2236;
    out->issue_2236 = s.issue_2236;
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
// AC2 note. PerEval (=2) is plumbed but eval_id threading is a
// #2158 follow-up; the current PerEval selection uses the same code
// path as PerRegion (per-region surrogate until eval_id threaded).
extern "C" void aura_set_storm_isolation_mode(int mode) noexcept {
    aura::compiler::hot_update_registry().set_storm_isolation_mode(
        static_cast<aura::compiler::HotUpdateRegistry::StormIsolation>(mode));
}

extern "C" int aura_get_storm_isolation_mode(void) noexcept {
    return static_cast<int>(aura::compiler::hot_update_registry().storm_isolation_mode());
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
