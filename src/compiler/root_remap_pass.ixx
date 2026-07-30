// root_remap_pass.ixx — Issue #2294 / #2267: RootRemapPass real rewrite
// of Stable object roots + Closure capture cells after Moving densify.
//
// #2267 shipped the arena callback surface + observability scaffolding;
// the pass body was a stub (counter bumps only). This module provides:
//   - void** slot registries for stable-object roots and closure captures
//   - real old→new rewrite via object_remap
//   - fail-closed for unmapped densify candidates
//   - optional AURA_ROOT_REMAP_CONTRACT=hard abort on fail
//   - AC3 zero-cost early return on empty remap
//
// Non-goals (see #2294): full heap-graph moving collector; EnvFrame SoA
// index remapping; LifetimePin path (already #2265).

module;

#include "observability_metrics.h"
// Issue #2297: publish densify object_remap for structural capture remount
// (aura_remount_closure_captures defense-in-depth after densify).
#include "aura_jit_bridge.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module aura.compiler.root_remap_pass;

import std;

export namespace aura::compiler {

// Per-invocation rewrite stats (mirrors LiveCompactResult / ArenaStats /
// CompilerMetrics root_remap_* counters).
struct RootRemapStats {
    std::size_t stable_ref_total = 0;
    std::size_t stable_ref_fail_total = 0;
    std::size_t closure_capture_total = 0;
    std::size_t closure_capture_fail_total = 0;

    [[nodiscard]] bool empty() const noexcept {
        return stable_ref_total == 0 && stable_ref_fail_total == 0 && closure_capture_total == 0 &&
               closure_capture_fail_total == 0;
    }
    [[nodiscard]] bool any_fail() const noexcept {
        return stable_ref_fail_total > 0 || closure_capture_fail_total > 0;
    }
};

namespace root_remap_detail {

    inline std::mutex& registry_mtx() {
        static std::mutex m;
        return m;
    }

    // Host-registered remappable pointer slots. "StableNodeRef" in #2267/#2294
    // language = host-stable object root (void** cell), not FlatAST::StableNodeRef
    // (NodeId+gen). FlatAST refs are gen-stamped and do not hold densify addrs.
    inline std::vector<void**>& stable_slots() {
        static std::vector<void**> v;
        return v;
    }
    inline std::vector<void**>& closure_capture_slots() {
        static std::vector<void**> v;
        return v;
    }

    // Optional densify-candidate overlay for fail-closed testing / dropped-track
    // densify paths: addresses that were densified (or claimed densified) even if
    // absent from object_remap. When non-empty and a registered slot points into
    // this set without a remap entry → fail counter.
    inline std::unordered_set<void*>& extra_densify_candidates() {
        static std::unordered_set<void*> s;
        return s;
    }

    inline std::atomic<std::uint64_t> g_pass_calls_total{0};
    inline std::atomic<std::uint64_t> g_rewrite_ok_total{0};
    inline std::atomic<std::uint64_t> g_rewrite_fail_total{0};

    // Thread-local CompilerMetrics for test/direct-invoke paths (mirrors #2267).
    inline thread_local CompilerMetrics* g_metrics_for_test = nullptr;

    [[nodiscard]] inline bool hard_contract_enabled() noexcept {
        const char* e = std::getenv("AURA_ROOT_REMAP_CONTRACT");
        return e != nullptr && (std::strcmp(e, "hard") == 0 || std::strcmp(e, "1") == 0 ||
                                std::strcmp(e, "true") == 0);
    }

    // Remap one slot. Returns: 1 = remapped (or address-stable densify hit),
    // -1 = densify candidate unmapped (fail), 0 = not a densify candidate.
    inline int remap_one_slot(void** slot, const std::unordered_map<void*, void*>& object_remap,
                              const std::unordered_set<void*>& densify_keys) noexcept {
        if (slot == nullptr || *slot == nullptr)
            return 0;
        auto it = object_remap.find(*slot);
        if (it != object_remap.end()) {
            *slot = it->second;
            return 1;
        }
        if (densify_keys.find(*slot) != densify_keys.end())
            return -1; // was densify candidate, no remap entry → fail-closed
        return 0;
    }

    inline void bump_metrics(const RootRemapStats& s) noexcept {
        auto* m = g_metrics_for_test;
        if (m == nullptr)
            return;
        if (s.stable_ref_total)
            m->root_remap_stable_ref_total.fetch_add(s.stable_ref_total, std::memory_order_relaxed);
        if (s.stable_ref_fail_total)
            m->root_remap_stable_ref_fail_total.fetch_add(s.stable_ref_fail_total,
                                                          std::memory_order_relaxed);
        if (s.closure_capture_total)
            m->root_remap_closure_capture_total.fetch_add(s.closure_capture_total,
                                                          std::memory_order_relaxed);
        if (s.closure_capture_fail_total)
            m->root_remap_closure_capture_fail_total.fetch_add(s.closure_capture_fail_total,
                                                               std::memory_order_relaxed);
    }

} // namespace root_remap_detail

// ── Registry API (host / tests) ──────────────────────────────────

inline void register_root_remap_stable_slot(void** slot) noexcept {
    if (!slot)
        return;
    std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
    auto& v = root_remap_detail::stable_slots();
    if (std::find(v.begin(), v.end(), slot) == v.end())
        v.push_back(slot);
}

inline void unregister_root_remap_stable_slot(void** slot) noexcept {
    if (!slot)
        return;
    std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
    auto& v = root_remap_detail::stable_slots();
    v.erase(std::remove(v.begin(), v.end(), slot), v.end());
}

inline void register_root_remap_closure_capture_slot(void** slot) noexcept {
    if (!slot)
        return;
    std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
    auto& v = root_remap_detail::closure_capture_slots();
    if (std::find(v.begin(), v.end(), slot) == v.end())
        v.push_back(slot);
}

// Issue #2339: auto-register / auto-unregister counters (per-call-site
// that wires up auto-register instead of manual register_root_remap_*
// calls). Mirrors the existing root_remap_*_total counters but
// distinguishes manual vs auto register paths so dashboards can spot
// adoption coverage across materialize sites (Closure capture install,
// Stable object root install).
inline std::atomic<std::uint64_t> g_root_remap_auto_register_total{0};            // #2339
inline std::atomic<std::uint64_t> g_root_remap_auto_register_unregister_total{0}; // #2339

// Issue #2339: auto-register wrappers (counter-bumping). Use these at
// materialize / install sites instead of the manual register_root_remap_*
// so dashboards can observe adoption via the auto_register counters.
// Existing manual register_root_remap_*_slot paths remain unchanged
// (no counter bump) so tests + pre-#2339 code paths still work.
inline void auto_register_root_remap_stable_slot(void** slot) noexcept {
    register_root_remap_stable_slot(slot);
    g_root_remap_auto_register_total.fetch_add(1, std::memory_order_relaxed);
}
inline void auto_unregister_root_remap_stable_slot(void** slot) noexcept {
    unregister_root_remap_stable_slot(slot);
    g_root_remap_auto_register_unregister_total.fetch_add(1, std::memory_order_relaxed);
}
inline void auto_register_root_remap_closure_capture_slot(void** slot) noexcept {
    register_root_remap_closure_capture_slot(slot);
    g_root_remap_auto_register_total.fetch_add(1, std::memory_order_relaxed);
}
inline void auto_unregister_root_remap_closure_capture_slot(void** slot) noexcept {
    unregister_root_remap_closure_capture_slot(slot);
    g_root_remap_auto_register_unregister_total.fetch_add(1, std::memory_order_relaxed);
}

// Issue #2339: RAII helper for stable-object root slots. Construct
// registers via auto_register, destruct unregisters via auto_unregister.
// Avoids forget-unregister leaks at Closure/Stable materialize sites.
class RootRemapAutoRegisterStable {
    void** slot_{nullptr};

public:
    explicit RootRemapAutoRegisterStable(void** slot) noexcept
        : slot_(slot) {
        auto_register_root_remap_stable_slot(slot_);
    }
    ~RootRemapAutoRegisterStable() noexcept {
        if (slot_)
            auto_unregister_root_remap_stable_slot(slot_);
    }
    RootRemapAutoRegisterStable(const RootRemapAutoRegisterStable&) = delete;
    RootRemapAutoRegisterStable& operator=(const RootRemapAutoRegisterStable&) = delete;
    RootRemapAutoRegisterStable(RootRemapAutoRegisterStable&& o) noexcept
        : slot_(o.slot_) {
        o.slot_ = nullptr;
    }
    RootRemapAutoRegisterStable& operator=(RootRemapAutoRegisterStable&& o) noexcept {
        if (this != &o) {
            if (slot_)
                auto_unregister_root_remap_stable_slot(slot_);
            slot_ = o.slot_;
            o.slot_ = nullptr;
        }
        return *this;
    }
};

// Issue #2339: RAII helper for closure capture cell slots. Same pattern
// as RootRemapAutoRegisterStable but for the closure_capture registry.
class RootRemapAutoRegisterClosureCapture {
    void** slot_{nullptr};

public:
    explicit RootRemapAutoRegisterClosureCapture(void** slot) noexcept
        : slot_(slot) {
        auto_register_root_remap_closure_capture_slot(slot_);
    }
    ~RootRemapAutoRegisterClosureCapture() noexcept {
        if (slot_)
            auto_unregister_root_remap_closure_capture_slot(slot_);
    }
    RootRemapAutoRegisterClosureCapture(const RootRemapAutoRegisterClosureCapture&) = delete;
    RootRemapAutoRegisterClosureCapture&
    operator=(const RootRemapAutoRegisterClosureCapture&) = delete;
    RootRemapAutoRegisterClosureCapture(RootRemapAutoRegisterClosureCapture&& o) noexcept
        : slot_(o.slot_) {
        o.slot_ = nullptr;
    }
    RootRemapAutoRegisterClosureCapture&
    operator=(RootRemapAutoRegisterClosureCapture&& o) noexcept {
        if (this != &o) {
            if (slot_)
                auto_unregister_root_remap_closure_capture_slot(slot_);
            slot_ = o.slot_;
            o.slot_ = nullptr;
        }
        return *this;
    }
}

inline void
unregister_root_remap_closure_capture_slot(void** slot) noexcept {
    if (!slot)
        return;
    std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
    auto& v = root_remap_detail::closure_capture_slots();
    v.erase(std::remove(v.begin(), v.end(), slot), v.end());
}

// Mark addresses as densify candidates even when absent from object_remap
// (AC4 fail-closed path + dropped-track densify). Cleared by reset.
inline void mark_root_remap_densify_candidates(const std::unordered_set<void*>& cands) {
    std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
    root_remap_detail::extra_densify_candidates() = cands;
}

inline void clear_root_remap_densify_candidates() noexcept {
    std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
    root_remap_detail::extra_densify_candidates().clear();
}

inline void reset_root_remap_registries_for_test() noexcept {
    std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
    root_remap_detail::stable_slots().clear();
    root_remap_detail::closure_capture_slots().clear();
    root_remap_detail::extra_densify_candidates().clear();
}

inline void set_root_remap_pass_test_metrics(CompilerMetrics* m) noexcept {
    root_remap_detail::g_metrics_for_test = m;
}

[[nodiscard]] inline std::uint64_t root_remap_pass_calls_total() noexcept {
    return root_remap_detail::g_pass_calls_total.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t root_remap_rewrite_ok_total() noexcept {
    return root_remap_detail::g_rewrite_ok_total.load(std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t root_remap_rewrite_fail_total() noexcept {
    return root_remap_detail::g_rewrite_fail_total.load(std::memory_order_relaxed);
}

// Issue #2339: auto-register / auto-unregister accessors.
[[nodiscard]] inline std::uint64_t root_remap_auto_register_total() noexcept {
    return g_root_remap_auto_register_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t root_remap_auto_register_unregister_total() noexcept {
    return g_root_remap_auto_register_unregister_total.load(std::memory_order_relaxed);
}

// Core rewrite. Empty object_remap → zero work (AC3).
inline RootRemapStats
run_root_remap_pass(const std::unordered_map<void*, void*>& object_remap) noexcept {
    RootRemapStats stats;
    root_remap_detail::g_pass_calls_total.fetch_add(1, std::memory_order_relaxed);

    // AC3: empty remap → no rewrite work, no fail scans.
    if (object_remap.empty() && [&] {
            std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
            return root_remap_detail::extra_densify_candidates().empty();
        }()) {
        // Issue #2297: clear densify context when densify did not produce a map.
        aura_clear_densify_object_remap();
        aura_clear_densify_candidates();
        return stats;
    }

    std::vector<void**> stable_copy;
    std::vector<void**> capture_copy;
    std::unordered_set<void*> densify_keys;
    {
        std::lock_guard<std::mutex> lock(root_remap_detail::registry_mtx());
        stable_copy = root_remap_detail::stable_slots();
        capture_copy = root_remap_detail::closure_capture_slots();
        densify_keys = root_remap_detail::extra_densify_candidates();
    }
    densify_keys.reserve(densify_keys.size() + object_remap.size());
    for (const auto& [old_ptr, neu] : object_remap) {
        (void)neu;
        densify_keys.insert(old_ptr);
    }

    // Issue #2297: publish densify object_remap for later remount
    // structural walk (closures that outlived densify without a
    // registered RootRemap slot still get defense-in-depth rewrite).
    if (!object_remap.empty()) {
        std::vector<const void*> olds;
        std::vector<const void*> news;
        olds.reserve(object_remap.size());
        news.reserve(object_remap.size());
        for (const auto& [old_ptr, neu] : object_remap) {
            olds.push_back(old_ptr);
            news.push_back(neu);
        }
        aura_set_densify_object_remap(olds.data(), news.data(), olds.size());
        if (!densify_keys.empty()) {
            std::vector<const void*> cands(densify_keys.begin(), densify_keys.end());
            aura_set_densify_candidates(cands.data(), cands.size());
        }
    }

    for (void** slot : stable_copy) {
        const int r = root_remap_detail::remap_one_slot(slot, object_remap, densify_keys);
        if (r > 0)
            ++stats.stable_ref_total;
        else if (r < 0)
            ++stats.stable_ref_fail_total;
    }
    for (void** slot : capture_copy) {
        const int r = root_remap_detail::remap_one_slot(slot, object_remap, densify_keys);
        if (r > 0)
            ++stats.closure_capture_total;
        else if (r < 0)
            ++stats.closure_capture_fail_total;
    }

    const auto ok = stats.stable_ref_total + stats.closure_capture_total;
    const auto fail = stats.stable_ref_fail_total + stats.closure_capture_fail_total;
    if (ok)
        root_remap_detail::g_rewrite_ok_total.fetch_add(ok, std::memory_order_relaxed);
    if (fail)
        root_remap_detail::g_rewrite_fail_total.fetch_add(fail, std::memory_order_relaxed);

    root_remap_detail::bump_metrics(stats);

    if (stats.any_fail() && root_remap_detail::hard_contract_enabled()) {
        // Fail-closed hard mode: process abort (production safety contract).
        std::abort();
    }
    return stats;
}

// Arena RootRemapCallback body — matches arena.ixx 7-arg typedef.
inline void root_remap_pass_callback_impl(std::uint64_t /*arena_id*/, std::uint64_t /*new_gen*/,
                                          const std::unordered_map<void*, void*>& object_remap,
                                          std::size_t& out_sr, std::size_t& out_sr_fail,
                                          std::size_t& out_cc, std::size_t& out_cc_fail) noexcept {
    const RootRemapStats s = run_root_remap_pass(object_remap);
    out_sr = s.stable_ref_total;
    out_sr_fail = s.stable_ref_fail_total;
    out_cc = s.closure_capture_total;
    out_cc_fail = s.closure_capture_fail_total;
}

// Test helper: 3-arg form that discards out-stats (still bumps process
// atomics + optional CompilerMetrics). Used by unit tests that invoke
// the pass without an ASTArena.
inline void
root_remap_pass_callback_impl_3(std::uint64_t arena_id, std::uint64_t new_gen,
                                const std::unordered_map<void*, void*>& object_remap) noexcept {
    std::size_t a = 0, b = 0, c = 0, d = 0;
    root_remap_pass_callback_impl(arena_id, new_gen, object_remap, a, b, c, d);
}

inline void (*get_root_remap_pass_test_callback() noexcept)(
    std::uint64_t, std::uint64_t, const std::unordered_map<void*, void*>&) {
    return &root_remap_pass_callback_impl_3;
}

// std::function for ASTArena::set_root_remap_callback (production install).
inline auto make_root_remap_arena_callback() {
    return [](std::uint64_t arena_id, std::uint64_t new_gen,
              const std::unordered_map<void*, void*>& object_remap, std::size_t& out_sr,
              std::size_t& out_sr_fail, std::size_t& out_cc, std::size_t& out_cc_fail) {
        root_remap_pass_callback_impl(arena_id, new_gen, object_remap, out_sr, out_sr_fail, out_cc,
                                      out_cc_fail);
    };
}

} // namespace aura::compiler
