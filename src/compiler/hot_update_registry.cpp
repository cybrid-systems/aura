// hot_update_registry.cpp — Issue #1956 / #2014
// Process-wide HotUpdateRegistry singleton.

#include "compiler/hot_update_registry.hh"

#include "compiler/aura_jit_bridge.h"

#include <chrono>
#include <utility>

namespace aura::compiler {

namespace {

    std::uint64_t steady_ms_now() noexcept {
        using namespace std::chrono;
        return static_cast<std::uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }

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

void HotUpdateRegistry::on_reload_rollback() noexcept {
    aot_reload_rollback_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::on_live_closure_remap(std::uint64_t count) noexcept {
    if (count == 0)
        return;
    live_closure_remap_.fetch_add(count, std::memory_order_relaxed);
}

// Issue #2014: sliding-window deopt rate. Under threshold this is:
//   1× fetch_add (observed) + 1× load start + 1× load window + branch.
// Clock is read only when the window may have rolled or on first deopt.
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
        return;
    }

    const std::uint64_t n = deopt_window_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n < threshold)
        return;

    // Trip throttle for the remainder of this window.
    reemit_throttled_.store(true, std::memory_order_relaxed);
    // Count storm once when the threshold is first crossed.
    if (n == threshold) {
        deopt_storm_detected_.fetch_add(1, std::memory_order_relaxed);
        notify_deopt_storm_locked(n, window_ms);
    }
}

bool HotUpdateRegistry::should_throttle_reemit() const noexcept {
    return reemit_throttled_.load(std::memory_order_relaxed);
}

void HotUpdateRegistry::on_reemit_throttled() noexcept {
    reemit_throttle_skips_.fetch_add(1, std::memory_order_relaxed);
}

void HotUpdateRegistry::set_deopt_storm_threshold(std::uint64_t deopts_per_window,
                                                  std::uint64_t window_ms) noexcept {
    deopt_storm_threshold_.store(deopts_per_window, std::memory_order_relaxed);
    deopt_storm_window_ms_.store(window_ms, std::memory_order_relaxed);
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
    // Keep lifetime counters (detected / observed / skips) for dashboards.
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
    s.reemit_throttle_active = reemit_throttled_.load(std::memory_order_relaxed) ? 1 : 0;
    s.reemit_throttle_skips_total =
        static_cast<std::int64_t>(reemit_throttle_skips_.load(std::memory_order_relaxed));
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
    out->live_closure_remap_total = s.live_closure_remap_total;
    out->deopt_storm_detected_total = s.deopt_storm_detected_total;
    out->deopt_observed_total = s.deopt_observed_total;
    out->deopt_window_count = s.deopt_window_count;
    out->deopt_storm_threshold = s.deopt_storm_threshold;
    out->deopt_storm_window_ms = s.deopt_storm_window_ms;
    out->reemit_throttle_active = s.reemit_throttle_active;
    out->reemit_throttle_skips_total = s.reemit_throttle_skips_total;
    out->storm_listeners = s.storm_listeners;
}

extern "C" void aura_hot_update_note_deopt(void) {
    aura::compiler::hot_update_registry().on_stale_deopt();
}

extern "C" int aura_hot_update_should_throttle_reemit(void) {
    return aura::compiler::hot_update_registry().should_throttle_reemit() ? 1 : 0;
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
