// stdlib/render_ffi.hh — Issue #1560: render backend FFI bindings + hot-path dispatch.
// Thin stdlib surface over renderer/render_ffi.hh + compiler/ffi_hot_path.hh.

#ifndef AURA_STDLIB_RENDER_FFI_HH
#define AURA_STDLIB_RENDER_FFI_HH

#include "compiler/ffi_hot_path.hh"
#include "renderer/render_ffi.hh"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace aura::stdlib::render_ffi {

inline constexpr int kStdlibRenderFfiPhase = 1;
inline constexpr int kStdlibRenderFfiIssue = 1560;

// Re-export logical binding names for Agent discovery.
using aura::renderer::ffi::FfiRenderHotpathGuard;
using aura::renderer::ffi::kBindAnsiEmit;
using aura::renderer::ffi::kBindDraw;
using aura::renderer::ffi::kBindPresentBatch;
using aura::renderer::ffi::render_ffi_registry;
using aura::renderer::ffi::RenderFfiRegistry;

// Register a render backend binding (name → c symbol + signature + optional ptr).
// Returns 0 on success.
inline int register_binding(std::string_view name, std::string_view c_name, std::string_view sig,
                            void* fn_ptr = nullptr) {
    return render_ffi_registry().register_binding(name, c_name, sig, fn_ptr);
}

namespace detail {

    inline std::int64_t dispatch_batch_c_render_impl(std::string_view binding_name,
                                                     std::span<const std::int64_t> args) {
        auto& reg = render_ffi_registry();
        auto& hot = aura::compiler::ffi_hot::global_ffi_batch_hot_path();

        std::string sig;
        void* fn = nullptr;
        {
            std::lock_guard<std::mutex> lock(reg.registry_mtx);
            auto it = reg.bindings.find(std::string(binding_name));
            if (it == reg.bindings.end())
                return -1;
            sig = it->second.signature;
            fn = it->second.fn_ptr;
        }

        const auto h = aura::compiler::ffi_hot::ffi_sig_hash(binding_name, sig);
        const auto abi = aura::compiler::ffi_hot::abi_from_signature(sig);

        void* resolved = fn;
        if (!hot.cached_sig_match(h)) {
            resolved = reg.resolve_binding(binding_name);
            if (!resolved)
                resolved = fn;
        }

        const auto ret = hot.dispatch_batch(h, resolved, abi, args);
        reg.record_dispatch(binding_name, 0);
        return ret;
    }

} // namespace detail

// Hot-path batch dispatch for a registered binding name.
// Always enters render hotpath (AC5). Returns invoke result, or -1 if unbound.
inline std::int64_t dispatch_batch_c_render(std::string_view binding_name,
                                            std::span<const std::int64_t> args) {
    FfiRenderHotpathGuard hp;
    return detail::dispatch_batch_c_render_impl(binding_name, args);
}

// Fixed-name helpers for standard render backends.
inline std::int64_t dispatch_c_render_draw(std::span<const std::int64_t> args) {
    return dispatch_batch_c_render(kBindDraw, args);
}
inline std::int64_t dispatch_c_present_batch(std::span<const std::int64_t> args) {
    return dispatch_batch_c_render(kBindPresentBatch, args);
}
inline std::int64_t dispatch_c_ansi_emit(std::span<const std::int64_t> args) {
    return dispatch_batch_c_render(kBindAnsiEmit, args);
}

} // namespace aura::stdlib::render_ffi

#endif // AURA_STDLIB_RENDER_FFI_HH
