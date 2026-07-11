// batch_terminal.hh — Issues #1349/#1350: ANSI helpers + TermCell RGB/Unicode.
// Header form for evaluator partition TUs. Keep in sync with batch_terminal.ixx.

#ifndef AURA_RENDERER_BATCH_TERMINAL_HH
#define AURA_RENDERER_BATCH_TERMINAL_HH

#include <cstdint>
#include <cstring>
#include <string>

namespace aura::renderer {

// Issue #1350: full cell (Unicode codepoint + 24-bit RGB fg/bg).
// mode=0 palette: fg_r/bg_r hold 0–255 indices; emit CSI 38;5 / 48;5.
// mode=1 rgb:     full RGB triplets; emit CSI 38;2 / 48;2.
struct TermCell {
    std::uint32_t ch = static_cast<std::uint32_t>(' ');
    std::uint8_t fg_r = 7; // default palette light-gray / or R
    std::uint8_t fg_g = 0;
    std::uint8_t fg_b = 0;
    std::uint8_t bg_r = 0;
    std::uint8_t bg_g = 0;
    std::uint8_t bg_b = 0;
    std::uint8_t mode = 0; // 0=palette, 1=rgb

    bool operator==(const TermCell& o) const noexcept {
        return ch == o.ch && fg_r == o.fg_r && fg_g == o.fg_g && fg_b == o.fg_b && bg_r == o.bg_r &&
               bg_g == o.bg_g && bg_b == o.bg_b && mode == o.mode;
    }
    bool operator!=(const TermCell& o) const noexcept { return !(*this == o); }

    // Color key for SGR change detection (mode + 6 channels).
    std::uint64_t color_key() const noexcept {
        return (static_cast<std::uint64_t>(mode) << 48) | (static_cast<std::uint64_t>(fg_r) << 40) |
               (static_cast<std::uint64_t>(fg_g) << 32) | (static_cast<std::uint64_t>(fg_b) << 24) |
               (static_cast<std::uint64_t>(bg_r) << 16) | (static_cast<std::uint64_t>(bg_g) << 8) |
               static_cast<std::uint64_t>(bg_b);
    }

    static TermCell space_palette() {
        TermCell c;
        c.ch = static_cast<std::uint32_t>(' ');
        c.fg_r = 7;
        c.bg_r = 0;
        c.mode = 0;
        return c;
    }
};

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

// Truecolor: ESC[38;2;R;G;Bm ESC[48;2;R;G;Bm (or combined)
inline void ansi_sgr_rgb_fg_bg(std::string& out, std::uint8_t fr, std::uint8_t fg, std::uint8_t fb,
                               std::uint8_t br, std::uint8_t bg, std::uint8_t bb) {
    out.append("\033[38;2;");
    out.append(std::to_string(static_cast<unsigned>(fr)));
    out.push_back(';');
    out.append(std::to_string(static_cast<unsigned>(fg)));
    out.push_back(';');
    out.append(std::to_string(static_cast<unsigned>(fb)));
    out.append(";48;2;");
    out.append(std::to_string(static_cast<unsigned>(br)));
    out.push_back(';');
    out.append(std::to_string(static_cast<unsigned>(bg)));
    out.push_back(';');
    out.append(std::to_string(static_cast<unsigned>(bb)));
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

// UTF-8 encode a Unicode codepoint (BMP + supplementary).
inline void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 32u || (cp >= 0x7Fu && cp < 0xA0u)) {
        out.push_back(' ');
        return;
    }
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(' ');
    }
}

// Decode first UTF-8 codepoint from a byte span. Returns U+FFFD on error.
inline std::uint32_t utf8_first_codepoint(const char* s, std::size_t n) {
    if (!s || n == 0)
        return static_cast<std::uint32_t>(' ');
    const auto b0 = static_cast<std::uint8_t>(s[0]);
    if (b0 < 0x80)
        return b0;
    if ((b0 & 0xE0) == 0xC0 && n >= 2) {
        const auto b1 = static_cast<std::uint8_t>(s[1]);
        if ((b1 & 0xC0) != 0x80)
            return 0xFFFDu;
        return (static_cast<std::uint32_t>(b0 & 0x1F) << 6) | (b1 & 0x3F);
    }
    if ((b0 & 0xF0) == 0xE0 && n >= 3) {
        const auto b1 = static_cast<std::uint8_t>(s[1]);
        const auto b2 = static_cast<std::uint8_t>(s[2]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80)
            return 0xFFFDu;
        return (static_cast<std::uint32_t>(b0 & 0x0F) << 12) |
               (static_cast<std::uint32_t>(b1 & 0x3F) << 6) | (b2 & 0x3F);
    }
    if ((b0 & 0xF8) == 0xF0 && n >= 4) {
        const auto b1 = static_cast<std::uint8_t>(s[1]);
        const auto b2 = static_cast<std::uint8_t>(s[2]);
        const auto b3 = static_cast<std::uint8_t>(s[3]);
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80)
            return 0xFFFDu;
        return (static_cast<std::uint32_t>(b0 & 0x07) << 18) |
               (static_cast<std::uint32_t>(b1 & 0x3F) << 12) |
               (static_cast<std::uint32_t>(b2 & 0x3F) << 6) | (b3 & 0x3F);
    }
    return 0xFFFDu;
}

// Build full frame from TermCell array. Returns SGR emit count.
inline std::uint64_t build_terminal_frame_ansi(std::string& out, std::int32_t w, std::int32_t h,
                                               const TermCell* cells) {
    if (w <= 0 || h <= 0 || !cells)
        return 0;
    out.reserve(out.size() + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 16u + 64u);
    ansi_sync_begin(out);
    ansi_hide_cursor(out);
    ansi_csi_h(out, 1, 1);
    std::uint64_t last_key = ~0ull;
    std::uint64_t sgr_emits = 0;
    for (std::int32_t y = 0; y < h; ++y) {
        ansi_csi_h(out, y + 1, 1);
        for (std::int32_t x = 0; x < w; ++x) {
            const auto& cell = cells[static_cast<std::size_t>(y * w + x)];
            const auto key = cell.color_key();
            if (key != last_key) {
                if (cell.mode == 0) {
                    ansi_sgr_fg_bg(out, cell.fg_r, cell.bg_r);
                } else {
                    ansi_sgr_rgb_fg_bg(out, cell.fg_r, cell.fg_g, cell.fg_b, cell.bg_r, cell.bg_g,
                                       cell.bg_b);
                }
                last_key = key;
                ++sgr_emits;
            }
            append_utf8(out, cell.ch);
        }
    }
    ansi_reset(out);
    ansi_show_cursor(out);
    ansi_sync_end(out);
    return sgr_emits;
}

// Legacy overload: packed u32 cells (ch | fg<<16 | bg<<24) → converted on the fly.
inline std::uint64_t build_terminal_frame_ansi(std::string& out, std::int32_t w, std::int32_t h,
                                               const std::uint32_t* cells) {
    if (w <= 0 || h <= 0 || !cells)
        return 0;
    // Convert stack-local — small buffers only; present path uses TermCell*.
    std::string tmp;
    // Avoid large stack: stream via temp TermCell one at a time by building inline.
    out.reserve(out.size() + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 12u + 64u);
    ansi_sync_begin(out);
    ansi_hide_cursor(out);
    ansi_csi_h(out, 1, 1);
    std::int32_t last_fg = -1, last_bg = -1;
    std::uint64_t sgr_emits = 0;
    for (std::int32_t y = 0; y < h; ++y) {
        ansi_csi_h(out, y + 1, 1);
        for (std::int32_t x = 0; x < w; ++x) {
            const auto packed = cells[static_cast<std::size_t>(y * w + x)];
            const auto ch = packed & 0xFFu;
            const auto fg = static_cast<std::int32_t>((packed >> 16) & 0xFFu);
            const auto bg = static_cast<std::int32_t>((packed >> 24) & 0xFFu);
            if (fg != last_fg || bg != last_bg) {
                ansi_sgr_fg_bg(out, static_cast<std::uint8_t>(fg), static_cast<std::uint8_t>(bg));
                last_fg = fg;
                last_bg = bg;
                ++sgr_emits;
            }
            append_utf8(out, ch);
        }
    }
    ansi_reset(out);
    ansi_show_cursor(out);
    ansi_sync_end(out);
    return sgr_emits;
}

} // namespace aura::renderer

#endif // AURA_RENDERER_BATCH_TERMINAL_HH
