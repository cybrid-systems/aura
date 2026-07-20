// hot_update_registry.cpp — Issue #1956
// Process-wide HotUpdateRegistry singleton.

#include "compiler/hot_update_registry.hh"

#include "compiler/aura_jit_bridge.h"

#include <utility>

namespace aura::compiler {

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

void HotUpdateRegistry::clear_listeners() noexcept {
    std::lock_guard<std::mutex> lock(listeners_mtx_);
    epoch_listeners_.clear();
    dirty_listeners_.clear();
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
}
