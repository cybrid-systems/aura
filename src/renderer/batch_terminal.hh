// batch_terminal.hh — Issue #1349: ANSI helpers for terminal-present-batch.
// Header form for evaluator partition TUs (module partition cannot easily
// import aura.renderer.batch_terminal). Keep in sync with batch_terminal.ixx.

#ifndef AURA_RENDERER_BATCH_TERMINAL_HH
#define AURA_RENDERER_BATCH_TERMINAL_HH

#include <cstdint>
#include <string>

namespace aura::renderer {

inline void ansi_sgr(std::string& out, int code) {
    out.push_back('\033');
    out.push_back('[');
    out.append(std::to_string(code));
    out.push_back('m');
}

// 256-color: ESC[38;5;<fg>;48;5;<bg>m
inline void ansi_sgr_fg_bg(std::string& out, std::uint8_t fg, std::uint8_t bg) {
    out.append("\033[38;5;");
    out.append(std::to_string(static_cast<unsigned>(fg)));
    out.append(";48;5;");
    out.append(std::to_string(static_cast<unsigned>(bg)));
    out.push_back('m');
}

// CSI H: ESC[<row>;<col>H (1-based)
inline void ansi_csi_h(std::string& out, int row, int col) {
    out.push_back('\033');
    out.push_back('[');
    out.append(std::to_string(row));
    out.push_back(';');
    out.append(std::to_string(col));
    out.push_back('H');
}

inline void ansi_sync_begin(std::string& out) {
    out.append("\033[?2026h");
}
inline void ansi_sync_end(std::string& out) {
    out.append("\033[?2026l");
}
inline void ansi_reset(std::string& out) {
    out.append("\033[0m");
}
inline void ansi_hide_cursor(std::string& out) {
    out.append("\033[?25l");
}
inline void ansi_show_cursor(std::string& out) {
    out.append("\033[?25h");
}
inline void ansi_clear_screen(std::string& out) {
    out.append("\033[2J\033[H");
}

// Build full frame from packed cells (ch | fg<<16 | bg<<24).
// Returns SGR emit count (for metrics).
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

#endif // AURA_RENDERER_BATCH_TERMINAL_HH
