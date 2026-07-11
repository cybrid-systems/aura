// render_ffi.ixx — Issue #1182 Phase 1: render FFI binding scaffold.

module;

export module aura.renderer.render_ffi;

import std;

export namespace aura::renderer::ffi {

inline constexpr int kRenderFfiPhase = 1;

// Symbolic binding names for future c-render-draw / c-present-batch peel.
inline constexpr std::string_view kBindDraw = "c-render-draw";
inline constexpr std::string_view kBindPresentBatch = "c-present-batch";
inline constexpr std::string_view kBindAnsiEmit = "c-ansi-emit";

struct RenderFfiRegistry {
    std::uint64_t registered = 0;
    std::uint64_t hot_path_dispatches = 0;
};

inline RenderFfiRegistry g_render_ffi_registry{};

} // namespace aura::renderer::ffi
