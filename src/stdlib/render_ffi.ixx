// stdlib/render_ffi.ixx — Issue #1560: render backend FFI module surface.
// Implementation: include stdlib/render_ffi.hh from C++ TUs / global fragments.
// Pattern mirrors ffi_primitives.ixx (registration + hot-path dispatch).

module;

export module aura.stdlib.render_ffi;

import std;

export namespace aura::stdlib::render_ffi {

inline constexpr int kStdlibRenderFfiPhase = 1;
inline constexpr int kStdlibRenderFfiIssue = 1560;

inline constexpr std::string_view kBindDraw = "c-render-draw";
inline constexpr std::string_view kBindPresentBatch = "c-present-batch";
inline constexpr std::string_view kBindAnsiEmit = "c-ansi-emit";

// Module-visible phase stats (full registry + dispatch in .hh).
struct RenderFfiStdlibStats {
    std::uint64_t phase = 1;
    std::uint64_t issue = 1560;
    std::uint64_t active = 1;
};

inline RenderFfiStdlibStats g_stdlib_render_ffi_stats{};

} // namespace aura::stdlib::render_ffi
