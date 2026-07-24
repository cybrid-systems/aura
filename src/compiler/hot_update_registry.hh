// hot_update_registry.hh — Issue #1956
// Unified coordination center for hot-update / incremental re-emit
// callbacks, region mask, epoch listeners, and aggregated metrics.
//
// Existing C-linkage entry points (aura_set_reemit_candidate_fn,
// aura_set_aot_emit_fn, aura_set_is_define_dirty_fn,
// aura_set_aot_emit_region_mask, stable func_id map) remain the
// process ABI. This registry:
//   1. records every registration for observability
//   2. owns dynamic epoch-bump listeners (plugin/agent extension)
//   3. provides notify_dirty_define / notify_epoch_bump fan-out
//   4. exposes hot_update_registry_* counters for dashboards
//
// MVP scope (#1943): single-workspace; no cross-COW migration.

#ifndef AURA_COMPILER_HOT_UPDATE_REGISTRY_HH
#define AURA_COMPILER_HOT_UPDATE_REGISTRY_HH

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace aura::compiler {

class HotUpdateRegistry {
public:
    using EpochListener = std::function<void(std::uint64_t epoch)>;
    using DirtyListener = std::function<void(const char* name)>;

    static HotUpdateRegistry& instance() noexcept;

    // ── registration bookkeeping (called from C setters) ──
    void on_reemit_provider_set(bool wired) noexcept;
    void on_define_dirty_provider_set(bool wired) noexcept;
    void on_aot_emit_provider_set(bool wired) noexcept;
    void on_emit_region_mask_set(std::uint64_t mask) noexcept;
    void on_stable_func_id_preserve(bool preserved) noexcept;
    void on_reemit_pipeline_call(std::uint64_t candidates, std::uint64_t successes) noexcept;
    // Issue #2012: atomic AOT reload success / rollback bookkeeping.
    void on_reload_success() noexcept;
    void on_reload_rollback() noexcept;

    // ── preferred C++ API (forwards to C ABI + bookkeeping) ──
    void set_emit_region_mask(std::uint64_t mask) noexcept;
    [[nodiscard]] std::uint64_t emit_region_mask() const noexcept;

    // Dynamic listeners (not process-ABI; for tests / agents / plugins).
    // Returns listener id (stable until clear).
    std::uint64_t register_epoch_listener(EpochListener fn);
    std::uint64_t register_dirty_listener(DirtyListener fn);
    void clear_listeners() noexcept;

    void notify_epoch_bump(std::uint64_t epoch) noexcept;
    void notify_dirty_define(const char* name) noexcept;

    // ── snapshot for query:hot-update-registry-stats ──
    struct Snapshot {
        std::int64_t schema = 1956;
        std::int64_t issue = 1956;
        std::int64_t active = 1;
        std::int64_t reemit_provider_wired = 0;
        std::int64_t define_dirty_provider_wired = 0;
        std::int64_t aot_emit_provider_wired = 0;
        std::int64_t emit_region_mask = 0;
        std::int64_t epoch_listeners = 0;
        std::int64_t dirty_listeners = 0;
        std::int64_t register_calls_total = 0;
        std::int64_t epoch_notify_total = 0;
        std::int64_t dirty_notify_total = 0;
        std::int64_t reemit_pipeline_calls_total = 0;
        std::int64_t reemit_candidates_total = 0;
        std::int64_t reemit_success_total = 0;
        std::int64_t stable_id_preserve_total = 0;
        std::int64_t stable_id_assign_total = 0;
        std::int64_t stable_func_id_map_size = 0;
        // Issue #2012: atomic reload recovery counters.
        std::int64_t aot_reload_success_total = 0;
        std::int64_t aot_reload_rollback_total = 0;
    };
    [[nodiscard]] Snapshot snapshot() const noexcept;

    // Process-wide counters (also mirrored into CompilerMetrics when available).
    [[nodiscard]] std::uint64_t register_calls_total() const noexcept {
        return register_calls_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t epoch_notify_total() const noexcept {
        return epoch_notify_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t dirty_notify_total() const noexcept {
        return dirty_notify_.load(std::memory_order_relaxed);
    }

private:
    HotUpdateRegistry() = default;

    mutable std::mutex listeners_mtx_;
    std::vector<EpochListener> epoch_listeners_;
    std::vector<DirtyListener> dirty_listeners_;
    std::uint64_t next_listener_id_{1};

    std::atomic<bool> reemit_wired_{false};
    std::atomic<bool> define_dirty_wired_{false};
    std::atomic<bool> aot_emit_wired_{false};
    std::atomic<std::uint64_t> emit_region_mask_{0};

    std::atomic<std::uint64_t> register_calls_{0};
    std::atomic<std::uint64_t> epoch_notify_{0};
    std::atomic<std::uint64_t> dirty_notify_{0};
    std::atomic<std::uint64_t> reemit_pipeline_calls_{0};
    std::atomic<std::uint64_t> reemit_candidates_{0};
    std::atomic<std::uint64_t> reemit_success_{0};
    std::atomic<std::uint64_t> stable_id_preserve_{0};
    std::atomic<std::uint64_t> stable_id_assign_{0};
    std::atomic<std::uint64_t> aot_reload_success_{0};  // #2012
    std::atomic<std::uint64_t> aot_reload_rollback_{0}; // #2012
};

// Free functions for C bridge (no C++ class in extern "C" bodies).
inline HotUpdateRegistry& hot_update_registry() noexcept {
    return HotUpdateRegistry::instance();
}

} // namespace aura::compiler

// C-linkage snapshot for module TUs (cannot attach HotUpdateRegistry to a
// module partition — Issue #1956 link discipline).
extern "C" {
struct aura_hot_update_registry_snapshot {
    std::int64_t schema;
    std::int64_t issue;
    std::int64_t active;
    std::int64_t reemit_provider_wired;
    std::int64_t define_dirty_provider_wired;
    std::int64_t aot_emit_provider_wired;
    std::int64_t emit_region_mask;
    std::int64_t epoch_listeners;
    std::int64_t dirty_listeners;
    std::int64_t register_calls_total;
    std::int64_t epoch_notify_total;
    std::int64_t dirty_notify_total;
    std::int64_t reemit_pipeline_calls_total;
    std::int64_t reemit_candidates_total;
    std::int64_t reemit_success_total;
    std::int64_t stable_id_preserve_total;
    std::int64_t stable_id_assign_total;
    std::int64_t stable_func_id_map_size;
    std::int64_t aot_reload_success_total;  // #2012
    std::int64_t aot_reload_rollback_total; // #2012
};
void aura_hot_update_registry_get_snapshot(aura_hot_update_registry_snapshot* out);
}

#endif // AURA_COMPILER_HOT_UPDATE_REGISTRY_HH
