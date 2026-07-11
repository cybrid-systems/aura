// render_ffi.ixx — Issues #1182 / #1354: module surface for render FFI.
// Implementation: include renderer/render_ffi.hh from C++ TUs / global fragments.

module;

export module aura.renderer.render_ffi;

import std;

export namespace aura::renderer::ffi {

inline constexpr int kRenderFfiPhase = 2;

inline constexpr std::string_view kBindDraw = "c-render-draw";
inline constexpr std::string_view kBindPresentBatch = "c-present-batch";
inline constexpr std::string_view kBindAnsiEmit = "c-ansi-emit";

// Lightweight module-visible counters (full registry is in render_ffi.hh).
struct RenderFfiModuleStats {
    std::uint64_t phase = 2;
    std::uint64_t scaffold = 1;
};

inline RenderFfiModuleStats g_render_ffi_module_stats{};

} // namespace aura::renderer::ffi
