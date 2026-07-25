// batch_terminal.ixx — Issues #1175/#1181 Phase 1 scaffold; #2047 Phase 2:
// full dirty-region differential update + present short-circuit stats.
//
// Header twin: batch_terminal.hh (TermCell / full dirty AABB path used by
// present_batch). This module keeps the packed-u32 API for module importers
// and owns process-wide BatchTerminalStats (+ Phase constant).

module;

export module aura.renderer.batch_terminal;

import std;
import aura.renderer.render_pass;

export namespace aura::renderer {

// Issue #2047: Phase 2 — dirty-region differential present is live.
inline constexpr int kBatchTerminalPhase = 2;
inline constexpr int kBatchTerminalIssue = 2047;
inline constexpr int kAnsiHelperPhase = 2; // #1181 + dirty peel

// Process-wide batch terminal observability (#1175 / #2047).
struct BatchTerminalStats {
    std::uint64_t sequences_emitted = 0;
    std::uint64_t dirty_rects = 0; // partial dirty presents
    std::uint64_t ansi_builds = 0;
    std::uint64_t dirty_short_circuit = 0; // clean present skipped
    std::uint64_t dirty_cells_emitted = 0;
    std::uint64_t dirty_full_presents = 0;
    std::uint64_t dirty_partial_presents = 0;
    std::uint64_t ansi_bytes_emitted = 0;
    std::uint64_t ansi_bytes_saved = 0;   // est full-frame − actual dirty
    std::uint64_t dirty_region_skips = 0; // alias of short-circuit samples
};

inline BatchTerminalStats g_batch_terminal_stats{};

inline void reset_batch_terminal_stats_for_test() noexcept {
    g_batch_terminal_stats = {};
}

// Issue #1181 / #1349: efficient ANSI sequence helpers (no pair alloc).
inline void ansi_sgr(std::string& out, int code) {
    out.push_back('\033');
    out.push_back('[');
    out.append(std::to_string(code));
    out.push_back('m');
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

// 256-color palette: ESC[38;5;<fg>;48;5;<bg>m
inline void ansi_sgr_fg_bg(std::string& out, std::uint8_t fg, std::uint8_t bg) {
    out.append("\033[38;5;");
    out.append(std::to_string(static_cast<unsigned>(fg)));
    out.append(";48;5;");
    out.append(std::to_string(static_cast<unsigned>(bg)));
    out.push_back('m');
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

// CSI H: ESC[<row>;<col>H  (1-based)
inline void ansi_csi_h(std::string& out, int row, int col) {
    out.push_back('\033');
    out.push_back('[');
    out.append(std::to_string(row));
    out.push_back(';');
    out.append(std::to_string(col));
    out.push_back('H');
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_cursor_move(std::string& out, int row, int col) {
    ansi_csi_h(out, row, col);
}

inline void ansi_sync_begin(std::string& out) {
    out.append("\033[?2026h");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_sync_end(std::string& out) {
    out.append("\033[?2026l");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_reset(std::string& out) {
    out.append("\033[0m");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_hide_cursor(std::string& out) {
    out.append("\033[?25l");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_show_cursor(std::string& out) {
    out.append("\033[?25h");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_alt_screen(std::string& out) {
    out.append("\033[?1049h");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_main_screen(std::string& out) {
    out.append("\033[?1049l");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_clear_screen(std::string& out) {
    out.append("\033[2J\033[H");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

// Packed cell: ch | fg<<16 | bg<<24
[[nodiscard]] inline std::size_t estimate_packed_frame_bytes(std::int32_t w,
                                                             std::int32_t h) noexcept {
    if (w <= 0 || h <= 0)
        return 128;
    return static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 12u + 64u;
}

// Full-frame packed builder (backward-compat). Returns SGR emit count.
inline std::uint64_t build_terminal_frame_ansi(std::string& out, std::int32_t w, std::int32_t h,
                                               const std::uint32_t* cells) {
    if (w <= 0 || h <= 0 || !cells)
        return 0;
    out.reserve(out.size() + estimate_packed_frame_bytes(w, h));
    ansi_sync_begin(out);
    ansi_hide_cursor(out);
    ansi_csi_h(out, 1, 1);
    std::int32_t last_fg = -1;
    std::int32_t last_bg = -1;
    std::uint64_t sgr_emits = 0;
    for (std::int32_t y = 0; y < h; ++y) {
        ansi_csi_h(out, y + 1, 1);
        for (std::int32_t x = 0; x < w; ++x) {
            const auto cell = cells[static_cast<std::size_t>(y * w + x)];
            const auto ch = cell & 0xFFu;
            const auto fg = static_cast<std::int32_t>((cell >> 16) & 0xFFu);
            const auto bg = static_cast<std::int32_t>((cell >> 24) & 0xFFu);
            if (fg != last_fg || bg != last_bg) {
                ansi_sgr_fg_bg(out, static_cast<std::uint8_t>(fg), static_cast<std::uint8_t>(bg));
                last_fg = fg;
                last_bg = bg;
                ++sgr_emits;
            }
            const char c = static_cast<char>(ch);
            out.push_back(c >= 32 ? c : ' ');
        }
    }
    ansi_reset(out);
    ansi_show_cursor(out);
    ansi_sync_end(out);
    g_batch_terminal_stats.ansi_bytes_emitted += out.size();
    ++g_batch_terminal_stats.dirty_full_presents;
    return sgr_emits;
}

// Issue #2047 / #1562: dirty-region differential for packed cells.
// Only emits CSI H + SGR + chars for inclusive AABB. When dirty is clean,
// returns zeros without writing (caller should short-circuit present).
struct PackedDirtyFrameEmitResult {
    std::uint64_t sgr_emits = 0;
    std::uint64_t cells_emitted = 0;
    bool partial = false;
    std::size_t bytes = 0;
};

inline PackedDirtyFrameEmitResult build_terminal_frame_ansi_dirty(std::string& out, std::int32_t w,
                                                                  std::int32_t h,
                                                                  const std::uint32_t* cells,
                                                                  const DirtyRegion& dirty) {
    PackedDirtyFrameEmitResult r;
    if (w <= 0 || h <= 0 || !cells || dirty.is_clean())
        return r;

    DirtyRegion region = dirty;
    if (!region.clamp_to(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)))
        return r;

    const auto x0 = region.x0;
    const auto y0 = region.y0;
    const auto x1 = region.x1;
    const auto y1 = region.y1;
    const bool full =
        region.is_full_frame(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    r.partial = !full;

    const auto dirty_w = x1 - x0 + 1;
    const auto dirty_h = y1 - y0 + 1;
    const auto before = out.size();
    out.reserve(out.size() + static_cast<std::size_t>(dirty_w) * dirty_h * 12u + 64u);
    ansi_sync_begin(out);
    ansi_hide_cursor(out);

    std::int32_t last_fg = -1;
    std::int32_t last_bg = -1;
    for (std::uint32_t y = y0; y <= y1; ++y) {
        ansi_csi_h(out, static_cast<int>(y + 1), static_cast<int>(x0 + 1));
        last_fg = -1;
        last_bg = -1;
        for (std::uint32_t x = x0; x <= x1; ++x) {
            const auto cell = cells[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + x];
            const auto ch = cell & 0xFFu;
            const auto fg = static_cast<std::int32_t>((cell >> 16) & 0xFFu);
            const auto bg = static_cast<std::int32_t>((cell >> 24) & 0xFFu);
            if (fg != last_fg || bg != last_bg) {
                ansi_sgr_fg_bg(out, static_cast<std::uint8_t>(fg), static_cast<std::uint8_t>(bg));
                last_fg = fg;
                last_bg = bg;
                ++r.sgr_emits;
            }
            const char c = static_cast<char>(ch);
            out.push_back(c >= 32 ? c : ' ');
            ++r.cells_emitted;
        }
    }
    ansi_reset(out);
    ansi_show_cursor(out);
    ansi_sync_end(out);
    r.bytes = out.size() - before;

    // Observability: partial rects + byte savings vs full-frame estimate.
    g_batch_terminal_stats.dirty_cells_emitted += r.cells_emitted;
    g_batch_terminal_stats.ansi_bytes_emitted += r.bytes;
    if (r.partial) {
        ++g_batch_terminal_stats.dirty_rects;
        ++g_batch_terminal_stats.dirty_partial_presents;
    } else {
        ++g_batch_terminal_stats.dirty_full_presents;
    }
    const auto full_est = estimate_packed_frame_bytes(w, h);
    if (full_est > r.bytes)
        g_batch_terminal_stats.ansi_bytes_saved += (full_est - r.bytes);
    return r;
}

// Short-circuit helper for clean present (Phase 2 contract).
// Returns true when caller should skip ANSI build entirely.
[[nodiscard]] inline bool batch_terminal_should_short_circuit(const DirtyRegion& dirty) noexcept {
    if (dirty.is_dirty())
        return false;
    ++g_batch_terminal_stats.dirty_short_circuit;
    ++g_batch_terminal_stats.dirty_region_skips;
    return true;
}

// Note a clean present from the C++ present_batch path (TermCell / engine).
inline void note_batch_terminal_short_circuit() noexcept {
    ++g_batch_terminal_stats.dirty_short_circuit;
    ++g_batch_terminal_stats.dirty_region_skips;
}

// Note a dirty present sample from the engine (TermCell path in .hh).
inline void note_batch_terminal_dirty_present(std::uint64_t cells_emitted, std::size_t bytes,
                                              std::size_t full_est, bool partial) noexcept {
    g_batch_terminal_stats.dirty_cells_emitted += cells_emitted;
    g_batch_terminal_stats.ansi_bytes_emitted += bytes;
    if (partial) {
        ++g_batch_terminal_stats.dirty_rects;
        ++g_batch_terminal_stats.dirty_partial_presents;
    } else {
        ++g_batch_terminal_stats.dirty_full_presents;
    }
    if (full_est > bytes)
        g_batch_terminal_stats.ansi_bytes_saved += (full_est - bytes);
}

} // namespace aura::renderer
