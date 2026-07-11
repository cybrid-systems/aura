// render_ffi.hh — Issues #1182 / #1354: render FFI registry + hot-path helpers.
// Header form for evaluator / FFI partition TUs (global module fragment).

#ifndef AURA_RENDERER_RENDER_FFI_HH
#define AURA_RENDERER_RENDER_FFI_HH

#include "core/arena_auto_policy_stats.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aura::renderer::ffi {

inline constexpr int kRenderFfiPhase = 2; // #1354: real registry

inline constexpr std::string_view kBindDraw = "c-render-draw";
inline constexpr std::string_view kBindPresentBatch = "c-present-batch";
inline constexpr std::string_view kBindAnsiEmit = "c-ansi-emit";

struct RenderFfiDescriptor {
    std::string name;   // logical binding name
    std::string c_name; // C symbol
    std::string signature;
    void* fn_ptr = nullptr;
    std::atomic<std::uint64_t> call_count{0};
    std::atomic<std::uint64_t> total_ns{0};
};

struct RenderFfiRegistry {
    std::atomic<std::uint64_t> registered{0};
    std::atomic<std::uint64_t> hot_path_dispatches{0};
    std::atomic<std::uint64_t> bind_attempts{0};
    std::atomic<std::uint64_t> bind_success{0};
    std::atomic<std::uint64_t> resolve_hits{0};
    // Process-wide: enter_render_hotpath calls from c-* / c-render-* paths.
    std::atomic<std::uint64_t> ffi_hotpath_enter_total{0};
    std::mutex registry_mtx;
    std::unordered_map<std::string, RenderFfiDescriptor> bindings;

    // Register / replace binding. fn_ptr may be null (unresolved).
    // Returns 0 on success, -1 on empty name.
    int register_binding(std::string_view name, std::string_view c_name, std::string_view sig,
                         void* fn_ptr = nullptr) {
        bind_attempts.fetch_add(1, std::memory_order_relaxed);
        if (name.empty())
            return -1;
        std::lock_guard<std::mutex> lock(registry_mtx);
        auto& d = bindings[std::string(name)];
        const bool is_new = d.name.empty();
        d.name = std::string(name);
        d.c_name = std::string(c_name);
        d.signature = std::string(sig);
        if (fn_ptr)
            d.fn_ptr = fn_ptr;
        if (is_new)
            registered.fetch_add(1, std::memory_order_relaxed);
        bind_success.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    void* resolve_binding(std::string_view name) {
        std::lock_guard<std::mutex> lock(registry_mtx);
        auto it = bindings.find(std::string(name));
        if (it == bindings.end() || !it->second.fn_ptr)
            return nullptr;
        resolve_hits.fetch_add(1, std::memory_order_relaxed);
        return it->second.fn_ptr;
    }

    void record_dispatch(std::string_view name, std::uint64_t ns = 0) {
        hot_path_dispatches.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(registry_mtx);
        auto it = bindings.find(std::string(name));
        if (it == bindings.end())
            return;
        it->second.call_count.fetch_add(1, std::memory_order_relaxed);
        if (ns)
            it->second.total_ns.fetch_add(ns, std::memory_order_relaxed);
    }

    // Snapshot for Agent query (name, c_name, sig, fn_ptr as int, call_count, total_ns).
    struct BindingSnap {
        std::string name;
        std::string c_name;
        std::string signature;
        std::int64_t fn_ptr = 0;
        std::int64_t call_count = 0;
        std::int64_t total_ns = 0;
    };
    std::vector<BindingSnap> snapshot() {
        std::lock_guard<std::mutex> lock(registry_mtx);
        std::vector<BindingSnap> out;
        out.reserve(bindings.size());
        for (auto& [k, d] : bindings) {
            BindingSnap s;
            s.name = d.name;
            s.c_name = d.c_name;
            s.signature = d.signature;
            s.fn_ptr = reinterpret_cast<std::int64_t>(d.fn_ptr);
            s.call_count = static_cast<std::int64_t>(d.call_count.load(std::memory_order_relaxed));
            s.total_ns = static_cast<std::int64_t>(d.total_ns.load(std::memory_order_relaxed));
            out.push_back(std::move(s));
        }
        return out;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(registry_mtx);
        bindings.clear();
        registered.store(0, std::memory_order_relaxed);
    }
};

inline RenderFfiRegistry& render_ffi_registry() {
    static RenderFfiRegistry reg;
    return reg;
}

// RAII: enter/exit arena render hot path + registry counter for FFI c-* paths.
struct FfiRenderHotpathGuard {
    FfiRenderHotpathGuard() noexcept {
        aura::core::arena_policy::enter_render_hotpath();
        render_ffi_registry().ffi_hotpath_enter_total.fetch_add(1, std::memory_order_relaxed);
    }
    ~FfiRenderHotpathGuard() noexcept { aura::core::arena_policy::exit_render_hotpath(); }
    FfiRenderHotpathGuard(const FfiRenderHotpathGuard&) = delete;
    FfiRenderHotpathGuard& operator=(const FfiRenderHotpathGuard&) = delete;
};

} // namespace aura::renderer::ffi

#endif // AURA_RENDERER_RENDER_FFI_HH
