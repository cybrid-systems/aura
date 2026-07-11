// batch_terminal.ixx — Issues #1175/#1181 Phase 1: batch ANSI + terminal dirty scaffold.

module;

export module aura.renderer.batch_terminal;

import std;

export namespace aura::renderer {

// Phase 1 sentinel: module is linkable; full dirty-region peel in follow-up.
inline constexpr int kBatchTerminalPhase = 1;
inline constexpr int kAnsiHelperPhase = 1; // #1181

struct BatchTerminalStats {
    std::uint64_t sequences_emitted = 0;
    std::uint64_t dirty_rects = 0;
    std::uint64_t ansi_builds = 0;
};

inline BatchTerminalStats g_batch_terminal_stats{};

// Issue #1181 Phase 1: efficient ANSI sequence helpers (no pair alloc on hot path).
// Builds into a caller-provided buffer / std::string.
// Issue #1349/#1350: extended SGR (TermCell+RGB live in batch_terminal.hh)
// Issue #1349: extended SGR fg/bg + CSI 2026 sync + cursor helpers for
// terminal-present-batch (P0 cyber-cat dependency).
inline void ansi_sgr(std::string& out, int code) {
    out.push_back('\033');
    out.push_back('[');
    out.append(std::to_string(code));
    out.push_back('m');
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

// 256-color palette (cell format: fg/bg are 0–255 packed in upper bits).
// Emits a single SGR: ESC[38;5;<fg>;48;5;<bg>m
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

// Issue #1349: build a full frame from packed cells (ch | fg<<16 | bg<<24).
// Emits CSI 2026 sync, hide cursor, per-row CSI H, SGR on fg/bg change, reset.
// Returns number of SGR emits (for metrics).
inline std::uint64_t build_terminal_frame_ansi(std::string& out, std::int32_t w, std::int32_t h,
                                               const std::uint32_t* cells) {
    if (w <= 0 || h <= 0 || !cells)
        return 0;
    out.reserve(out.size() + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 12u + 64u);
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
    return sgr_emits;
}

} // namespace aura::renderer
