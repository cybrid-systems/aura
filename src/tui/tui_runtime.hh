// tui_runtime.hh — Issues #1331–#1343 Phase 1: headless-safe TUI cell buffer + ANSI diff.
//
// Full termios raw-mode is opt-in (only when stdout is a TTY and force_tty is set).
// CI / tests use headless mode: put_cell + present build last_frame_ansi_ without
// touching the real terminal.
//
// Layer map: META #1331 / runtime #1332 / primitives #1333 / stdlib #1334–5 /
// cyber_cat #1337 / opts #1342 / mouse+games #1343.

#ifndef AURA_TUI_TUI_RUNTIME_HH
#define AURA_TUI_TUI_RUNTIME_HH

#include "tui/tui_input.hh"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace aura::tui {

inline std::atomic<std::uint64_t> g_tui_init_total{0};
inline std::atomic<std::uint64_t> g_tui_present_total{0};
inline std::atomic<std::uint64_t> g_tui_cell_writes{0};
inline std::atomic<std::uint64_t> g_tui_diff_cells_emitted{0};
inline std::atomic<std::uint64_t> g_tui_sync_output_frames{0};
inline std::atomic<std::uint64_t> g_tui_half_block_pixels{0};
inline std::atomic<std::uint64_t> g_tui_mouse_enable_total{0};
inline std::atomic<std::uint64_t> g_tui_read_event_total{0};

// attr bit: second column of an East-Asian wide glyph (CJK etc.). Not emitted;
// the terminal already advanced two columns when the head was printed.
inline constexpr std::uint8_t kAttrWideTail = 0x80;

// East-Asian Wide / Fullwidth (heuristic). Box-drawing stays narrow.
inline bool is_wide_glyph(std::uint32_t cp) noexcept {
    if (cp < 0x1100)
        return false;
    // Hangul Jamo
    if (cp <= 0x115F)
        return true;
    if (cp == 0x2329 || cp == 0x232A)
        return true;
    // CJK radicals .. Yi syllables (includes Unified Ideographs)
    if (cp >= 0x2E80 && cp <= 0xA4CF)
        return true;
    // Hangul syllables
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return true;
    if (cp >= 0xF900 && cp <= 0xFAFF)
        return true;
    if (cp >= 0xFE10 && cp <= 0xFE19)
        return true;
    if (cp >= 0xFE30 && cp <= 0xFE6F)
        return true;
    if (cp >= 0xFF00 && cp <= 0xFF60)
        return true;
    if (cp >= 0xFFE0 && cp <= 0xFFE6)
        return true;
    // Common emoji / symbols often double-width in modern terminals
    if (cp >= 0x1F300 && cp <= 0x1FAFF)
        return true;
    if (cp >= 0x20000 && cp <= 0x3FFFD)
        return true;
    return false;
}

struct Cell {
    std::uint32_t ch = static_cast<std::uint32_t>(' ');
    std::uint32_t fg = 0xFFFFFF;
    std::uint32_t bg = 0x000000;
    std::uint8_t attr = 0;
    bool dirty = true;

    bool visual_eq(const Cell& o) const noexcept {
        return ch == o.ch && fg == o.fg && bg == o.bg && attr == o.attr;
    }
    bool is_wide_tail() const noexcept { return (attr & kAttrWideTail) != 0; }
};

struct Event {
    enum class Type { None, Key, Resize, Mouse, Quit };
    Type type = Type::None;
    std::uint32_t key = 0;
    int mouse_x = 0;
    int mouse_y = 0;
    int mouse_button = 0;
    int new_cols = 0;
    int new_rows = 0;
};

class TUIRuntime {
public:
    bool init(const std::string& title, int cols = 80, int rows = 24, bool force_tty = false) {
        if (initialized_)
            return false;
        cols_ = cols > 0 && cols <= 512 ? cols : 80;
        rows_ = rows > 0 && rows <= 256 ? rows : 24;
        title_ = title;
        headless_ = true;
#if defined(__unix__) || defined(__APPLE__)
        if (force_tty && ::isatty(STDOUT_FILENO))
            headless_ = false;
#else
        (void)force_tty;
#endif
        front_.assign(static_cast<std::size_t>(cols_ * rows_), Cell{});
        back_.assign(static_cast<std::size_t>(cols_ * rows_), Cell{});
        cursor_visible_ = false;
        mouse_enabled_ = false;
        last_frame_ansi_.clear();
        initialized_ = true;
        g_tui_init_total.fetch_add(1, std::memory_order_relaxed);
        clear();
        force_present();
        return true;
    }

    void shutdown() {
        if (!initialized_)
            return;
        // #1353: leave raw mode if we entered it with the TUI session.
        global_tui_input().disable_raw_mode();
        // Always restore the real TTY cursor. show_cursor() only flips a flag
        // that is applied on the next present(); after quit there is no present,
        // so without this the terminal stays cursor-hidden (\033[?25l).
        if (!headless_) {
            // show cursor + reset SGR + end synchronized output if stuck
            constexpr const char* kRestore = "\033[?25h\033[0m\033[?2026l";
            std::fwrite(kRestore, 1, std::strlen(kRestore), stdout);
            std::fflush(stdout);
        }
        cursor_visible_ = true;
        mouse_enabled_ = false;
        initialized_ = false;
        front_.clear();
        back_.clear();
        last_frame_ansi_.clear();
    }

    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool is_headless() const noexcept { return headless_; }
    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] const std::string& last_frame_ansi() const noexcept { return last_frame_ansi_; }
    [[nodiscard]] const std::string& title() const noexcept { return title_; }

    bool put_cell(int col, int row, std::uint32_t ch, std::uint32_t fg = 0xFFFFFF,
                  std::uint32_t bg = 0x000000, std::uint8_t attr = 0) {
        if (!initialized_ || col < 0 || row < 0 || col >= cols_ || row >= rows_)
            return false;
        auto& c = front_[static_cast<std::size_t>(row * cols_ + col)];
        const std::uint32_t glyph = ch ? ch : static_cast<std::uint32_t>(' ');
        // If we overwrite the tail of a previous wide glyph, clear its head.
        if (c.is_wide_tail() && col > 0) {
            auto& head = front_[static_cast<std::size_t>(row * cols_ + (col - 1))];
            if (is_wide_glyph(head.ch)) {
                head.ch = static_cast<std::uint32_t>(' ');
                head.attr = static_cast<std::uint8_t>(head.attr & ~kAttrWideTail);
                head.dirty = true;
            }
        }
        c.ch = glyph;
        c.fg = fg & 0xFFFFFFu;
        c.bg = bg & 0xFFFFFFu;
        c.attr = static_cast<std::uint8_t>(attr & ~kAttrWideTail);
        c.dirty = true;
        // Wide glyphs occupy two terminal columns: mark the next cell as tail
        // so emit_diff does not print into the second half of the glyph.
        if (is_wide_glyph(glyph) && col + 1 < cols_) {
            auto& tail = front_[static_cast<std::size_t>(row * cols_ + (col + 1))];
            tail.ch = static_cast<std::uint32_t>(' ');
            tail.fg = c.fg;
            tail.bg = c.bg;
            tail.attr = kAttrWideTail;
            tail.dirty = true;
        } else if (col + 1 < cols_) {
            auto& nxt = front_[static_cast<std::size_t>(row * cols_ + (col + 1))];
            if (nxt.is_wide_tail()) {
                nxt.attr = static_cast<std::uint8_t>(nxt.attr & ~kAttrWideTail);
                nxt.ch = static_cast<std::uint32_t>(' ');
                nxt.dirty = true;
            }
        }
        g_tui_cell_writes.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Half-block pixel (#1342): encode top=fg bottom=bg with U+2580 ▀
    bool put_half_block_pixel(int col, int row, std::uint32_t top_rgb, std::uint32_t bot_rgb) {
        g_tui_half_block_pixels.fetch_add(1, std::memory_order_relaxed);
        return put_cell(col, row, 0x2580u, top_rgb, bot_rgb, 0);
    }

    [[nodiscard]] Cell get_cell(int col, int row) const {
        if (!initialized_ || col < 0 || row < 0 || col >= cols_ || row >= rows_)
            return Cell{};
        return front_[static_cast<std::size_t>(row * cols_ + col)];
    }

    void clear(std::uint32_t fg = 0xFFFFFF, std::uint32_t bg = 0x000000) {
        if (!initialized_)
            return;
        for (auto& c : front_) {
            c.ch = static_cast<std::uint32_t>(' ');
            c.fg = fg;
            c.bg = bg;
            c.attr = 0;
            c.dirty = true;
        }
    }

    void present() {
        if (!initialized_)
            return;
        emit_diff(/*force=*/false);
        g_tui_present_total.fetch_add(1, std::memory_order_relaxed);
    }

    void force_present() {
        if (!initialized_)
            return;
        for (auto& c : front_)
            c.dirty = true;
        emit_diff(/*force=*/true);
        g_tui_present_total.fetch_add(1, std::memory_order_relaxed);
    }

    void hide_cursor() {
        cursor_visible_ = false;
        if (initialized_ && !headless_) {
            constexpr const char* kHide = "\033[?25l";
            std::fwrite(kHide, 1, std::strlen(kHide), stdout);
            std::fflush(stdout);
        }
    }
    void show_cursor() {
        cursor_visible_ = true;
        // Immediate restore so callers that do not present() still get a cursor.
        if (initialized_ && !headless_) {
            constexpr const char* kShow = "\033[?25h";
            std::fwrite(kShow, 1, std::strlen(kShow), stdout);
            std::fflush(stdout);
        }
    }
    void set_title(const std::string& t) { title_ = t; }

    void set_mouse_enabled(bool on) {
        mouse_enabled_ = on;
        auto& in = global_tui_input();
        if (on)
            in.enable_mouse();
        else
            in.disable_mouse();
        if (on)
            g_tui_mouse_enable_total.fetch_add(1, std::memory_order_relaxed);
    }
    [[nodiscard]] bool mouse_enabled() const noexcept {
        return mouse_enabled_ || global_tui_input().mouse_enabled();
    }

    // #1353: inject queue first, then real/raw/injected byte stream via TUIInput.
    std::optional<Event> read_event(int timeout_ms = 0) {
        g_tui_read_event_total.fetch_add(1, std::memory_order_relaxed);
        if (!injected_.empty()) {
            Event e = injected_.front();
            injected_.erase(injected_.begin());
            return e;
        }
        auto ie = global_tui_input().poll_event(timeout_ms);
        if (!ie)
            return std::nullopt;
        Event e;
        switch (ie->kind) {
            case InputEvent::Kind::Key:
                e.type = Event::Type::Key;
                e.key = ie->ch;
                break;
            case InputEvent::Kind::Quit:
                e.type = Event::Type::Quit;
                break;
            case InputEvent::Kind::Mouse:
                e.type = Event::Type::Mouse;
                e.mouse_button = ie->btn;
                e.mouse_x = ie->col;
                e.mouse_y = ie->row;
                break;
            case InputEvent::Kind::Resize:
                e.type = Event::Type::Resize;
                e.new_cols = ie->col;
                e.new_rows = ie->row;
                break;
            default:
                return std::nullopt;
        }
        return e;
    }

    void inject_key(std::uint32_t key) {
        Event e;
        e.type = Event::Type::Key;
        e.key = key;
        injected_.push_back(e);
    }

    void inject_quit() {
        Event e;
        e.type = Event::Type::Quit;
        injected_.push_back(e);
    }

    // #1353: inject raw terminal bytes (CSI arrows, mouse SGR, etc.)
    void inject_bytes(std::string_view bytes) { global_tui_input().inject_bytes(bytes); }

private:
    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    void emit_diff(bool force) {
        std::string out;
        out.reserve(static_cast<std::size_t>(cols_ * rows_ * 8));
        // #1342: synchronized output (CSI 2026)
        out += "\033[?2026h";
        g_tui_sync_output_frames.fetch_add(1, std::memory_order_relaxed);
        if (!cursor_visible_)
            out += "\033[?25l";
        if (!title_.empty()) {
            out += "\033]0;";
            out += title_;
            out += "\007";
        }

        std::int64_t emitted = 0;
        std::int64_t last_fg = -1, last_bg = -1;
        for (int r = 0; r < rows_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                auto idx = static_cast<std::size_t>(r * cols_ + c);
                auto& fc = front_[idx];
                auto& bc = back_[idx];
                // Wide-tail cells are consumed by the previous CJK head; the
                // terminal already advanced 2 columns. Never print into them
                // (that used to paint black spaces over Chinese glyphs).
                if (fc.is_wide_tail()) {
                    if (force || fc.dirty || !fc.visual_eq(bc)) {
                        bc = fc;
                        fc.dirty = false;
                    }
                    continue;
                }
                if (!force && !fc.dirty && fc.visual_eq(bc))
                    continue;
                // Absolute cursor move (1-based)
                out += "\033[";
                out += std::to_string(r + 1);
                out += ';';
                out += std::to_string(c + 1);
                out += 'H';
                if (static_cast<std::int64_t>(fc.fg) != last_fg ||
                    static_cast<std::int64_t>(fc.bg) != last_bg) {
                    out += "\033[38;2;";
                    out += std::to_string((fc.fg >> 16) & 0xFF);
                    out += ';';
                    out += std::to_string((fc.fg >> 8) & 0xFF);
                    out += ';';
                    out += std::to_string(fc.fg & 0xFF);
                    out += ";48;2;";
                    out += std::to_string((fc.bg >> 16) & 0xFF);
                    out += ';';
                    out += std::to_string((fc.bg >> 8) & 0xFF);
                    out += ';';
                    out += std::to_string(fc.bg & 0xFF);
                    out += 'm';
                    last_fg = fc.fg;
                    last_bg = fc.bg;
                }
                append_utf8(out, fc.ch);
                bc = fc;
                fc.dirty = false;
                ++emitted;
                // Keep back-buffer in sync for the implied tail column.
                if (is_wide_glyph(fc.ch) && c + 1 < cols_) {
                    auto tidx = static_cast<std::size_t>(r * cols_ + (c + 1));
                    auto& ft = front_[tidx];
                    auto& bt = back_[tidx];
                    bt = ft;
                    ft.dirty = false;
                }
            }
        }
        out += "\033[?2026l";
        if (cursor_visible_)
            out += "\033[?25h";
        last_frame_ansi_ = std::move(out);
        g_tui_diff_cells_emitted.fetch_add(static_cast<std::uint64_t>(emitted),
                                           std::memory_order_relaxed);
        if (!headless_ && !last_frame_ansi_.empty()) {
            std::fwrite(last_frame_ansi_.data(), 1, last_frame_ansi_.size(), stdout);
            std::fflush(stdout);
        }
    }

    bool initialized_ = false;
    bool headless_ = true;
    bool cursor_visible_ = true;
    bool mouse_enabled_ = false;
    int cols_ = 80;
    int rows_ = 24;
    std::string title_;
    std::string last_frame_ansi_;
    std::vector<Cell> front_;
    std::vector<Cell> back_;
    std::vector<Event> injected_;
};

// Process-wide singleton for Aura primitives (one TUI session).
inline TUIRuntime& global_tui() {
    static TUIRuntime rt;
    return rt;
}

} // namespace aura::tui

#endif // AURA_TUI_TUI_RUNTIME_HH
