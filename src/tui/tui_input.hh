// tui_input.hh — Issue #1353: termios raw mode + non-blocking poll + event parse.
// Header-only for global-module-fragment inclusion (same pattern as tui_runtime.hh).

#ifndef AURA_TUI_TUI_INPUT_HH
#define AURA_TUI_TUI_INPUT_HH

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace aura::tui {

inline std::atomic<std::uint64_t> g_tui_raw_mode_on_total{0};
inline std::atomic<std::uint64_t> g_tui_raw_mode_off_total{0};
inline std::atomic<std::uint64_t> g_tui_poll_event_total{0};
inline std::atomic<std::uint64_t> g_tui_poll_event_hits{0};
inline std::atomic<std::uint64_t> g_tui_key_events_total{0};
inline std::atomic<std::uint64_t> g_tui_mouse_events_total{0};
inline std::atomic<std::uint64_t> g_tui_quit_events_total{0};
inline std::atomic<std::uint64_t> g_tui_sigint_restore_total{0};
inline std::atomic<std::uint64_t> g_tui_input_active{1};

// Special key codepoints (BMP arrows as in #1353).
inline constexpr std::uint32_t kKeyArrowUp = 0x2191u;
inline constexpr std::uint32_t kKeyArrowDown = 0x2193u;
inline constexpr std::uint32_t kKeyArrowRight = 0x2192u;
inline constexpr std::uint32_t kKeyArrowLeft = 0x2190u;
inline constexpr std::uint32_t kKeyEnter = 0x0Du;
inline constexpr std::uint32_t kKeyEscape = 0x1Bu;
inline constexpr std::uint32_t kKeySpace = 0x20u;
inline constexpr std::uint32_t kKeyBackspace = 0x7Fu;

struct InputEvent {
    enum class Kind { None, Key, Resize, Quit, Mouse };
    Kind kind = Kind::None;
    std::uint32_t ch = 0;  // Unicode codepoint for Key
    std::int32_t mods = 0; // bit0=ctrl, bit1=alt, bit2=shift
    std::int32_t btn = 0;
    std::int32_t row = 0;
    std::int32_t col = 0;
    bool press = true;
};

// Forward: singleton accessor for signal handler.
class TUIInput;
inline TUIInput& global_tui_input();

class TUIInput {
public:
    TUIInput() = default;
    ~TUIInput() {
        disable_raw_mode();
        close_tty_fd();
    }

    // Prefer real keyboard TTY even when program source was piped on stdin
    // (echo '(...)' | aura — common for demos). Falls back to STDIN_FILENO.
    int input_fd() {
#if defined(__unix__) || defined(__APPLE__)
        if (input_fd_ >= 0)
            return input_fd_;
        if (::isatty(STDIN_FILENO)) {
            input_fd_ = STDIN_FILENO;
            return input_fd_;
        }
        // Program fed via pipe: open controlling terminal for keys.
        int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            input_fd_ = fd;
            owns_tty_fd_ = true;
            return input_fd_;
        }
        input_fd_ = STDIN_FILENO;
        return input_fd_;
#else
        return STDIN_FILENO;
#endif
    }

    void close_tty_fd() {
#if defined(__unix__) || defined(__APPLE__)
        if (owns_tty_fd_ && input_fd_ >= 0 && input_fd_ != STDIN_FILENO) {
            ::close(input_fd_);
        }
        owns_tty_fd_ = false;
        input_fd_ = -1;
#endif
    }

    // Enable raw mode. Idempotent. Uses /dev/tty when stdin is a pipe.
    bool enable_raw_mode() {
        if (raw_mode_)
            return true;
#if defined(__unix__) || defined(__APPLE__)
        const int fd = input_fd();
        if (::isatty(fd)) {
            if (::tcgetattr(fd, &orig_termios_) != 0)
                return false;
            struct termios raw = orig_termios_;
            raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG | IEXTEN));
            raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | INPCK | ISTRIP | BRKINT));
            raw.c_cflag |= static_cast<tcflag_t>(CS8);
            // Keep OPOST so ANSI output still works reasonably.
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (::tcsetattr(fd, TCSAFLUSH, &raw) != 0)
                return false;
            tty_raw_ = true;
            install_sigint_handler();
        } else {
            tty_raw_ = false; // simulated / headless
        }
#else
        tty_raw_ = false;
#endif
        raw_mode_ = true;
        g_tui_raw_mode_on_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool disable_raw_mode() {
        if (!raw_mode_)
            return true;
#if defined(__unix__) || defined(__APPLE__)
        if (tty_raw_) {
            if (mouse_enabled_)
                disable_mouse_sequences();
            const int fd = input_fd_ >= 0 ? input_fd_ : STDIN_FILENO;
            ::tcsetattr(fd, TCSAFLUSH, &orig_termios_);
            tty_raw_ = false;
            restore_sigint_handler();
        }
#endif
        raw_mode_ = false;
        mouse_enabled_ = false;
        g_tui_raw_mode_off_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool is_raw_mode() const noexcept { return raw_mode_; }
    [[nodiscard]] bool is_tty_raw() const noexcept { return tty_raw_; }

    // Non-blocking if timeout_ms==0. Returns event or nullopt.
    std::optional<InputEvent> poll_event(int timeout_ms = 0) {
        g_tui_poll_event_total.fetch_add(1, std::memory_order_relaxed);
        InputEvent ev;
        if (parse_from_buffer(ev)) {
            g_tui_poll_event_hits.fetch_add(1, std::memory_order_relaxed);
            count_event(ev);
            return ev;
        }
#if defined(__unix__) || defined(__APPLE__)
        const int fd = input_fd();
        if (tty_raw_ && ::isatty(fd)) {
            struct pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;
            const int pr = ::poll(&pfd, 1, timeout_ms < 0 ? -1 : timeout_ms);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                std::uint8_t buf[64];
                const auto n = ::read(fd, buf, sizeof(buf));
                if (n > 0) {
                    for (ssize_t i = 0; i < n; ++i)
                        byte_buf_.push_back(buf[i]);
                }
            }
        } else if (timeout_ms > 0 && byte_buf_.empty()) {
            // Headless: nothing to wait on; treat as timeout.
            (void)timeout_ms;
        }
#else
        (void)timeout_ms;
#endif
        if (parse_from_buffer(ev)) {
            g_tui_poll_event_hits.fetch_add(1, std::memory_order_relaxed);
            count_event(ev);
            return ev;
        }
        return std::nullopt;
    }

    // Test / agent hook: push raw bytes as if typed on stdin.
    void inject_bytes(const std::uint8_t* data, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i)
            byte_buf_.push_back(data[i]);
    }
    void inject_bytes(std::string_view s) {
        inject_bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    }

    void enable_mouse() {
        mouse_enabled_ = true;
#if defined(__unix__) || defined(__APPLE__)
        // SGR 1006 + normal tracking 1000
        const char* seq = "\033[?1006h\033[?1000h";
        if (::isatty(STDOUT_FILENO))
            (void)::write(STDOUT_FILENO, seq, std::strlen(seq));
#endif
    }

    void disable_mouse() {
        if (!mouse_enabled_)
            return;
        disable_mouse_sequences();
        mouse_enabled_ = false;
    }

    [[nodiscard]] bool mouse_enabled() const noexcept { return mouse_enabled_; }

    // Returns (rows, cols). Falls back to 24x80 if ioctl unavailable.
    [[nodiscard]] std::pair<int, int> terminal_size() const {
#if defined(__unix__) || defined(__APPLE__)
        struct winsize ws{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
            return {static_cast<int>(ws.ws_row), static_cast<int>(ws.ws_col)};
#endif
        return {24, 80};
    }

    // Emergency restore from signal path (async-signal-safe subset).
    void emergency_restore() noexcept {
#if defined(__unix__) || defined(__APPLE__)
        if (tty_raw_) {
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios_);
            const char* off = "\033[?1000l\033[?1006l";
            (void)::write(STDOUT_FILENO, off, 16);
            tty_raw_ = false;
        }
#endif
        raw_mode_ = false;
        g_tui_sigint_restore_total.fetch_add(1, std::memory_order_relaxed);
    }

private:
    void count_event(const InputEvent& ev) {
        switch (ev.kind) {
            case InputEvent::Kind::Key:
                g_tui_key_events_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case InputEvent::Kind::Mouse:
                g_tui_mouse_events_total.fetch_add(1, std::memory_order_relaxed);
                break;
            case InputEvent::Kind::Quit:
                g_tui_quit_events_total.fetch_add(1, std::memory_order_relaxed);
                break;
            default:
                break;
        }
    }

    void disable_mouse_sequences() {
#if defined(__unix__) || defined(__APPLE__)
        const char* seq = "\033[?1000l\033[?1006l";
        if (::isatty(STDOUT_FILENO))
            (void)::write(STDOUT_FILENO, seq, std::strlen(seq));
#endif
    }

    bool parse_from_buffer(InputEvent& ev) {
        if (byte_buf_.empty())
            return false;
        const auto b0 = byte_buf_.front();

        // Ctrl-C → Quit
        if (b0 == 0x03) {
            byte_buf_.pop_front();
            ev = {};
            ev.kind = InputEvent::Kind::Quit;
            return true;
        }

        // ESC sequence or bare ESC
        if (b0 == 0x1B) {
            return parse_escape(ev);
        }

        // UTF-8 / ASCII key
        std::uint32_t cp = 0;
        int need = 1;
        if (b0 < 0x80) {
            cp = b0;
            need = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            need = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            need = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            need = 4;
        } else {
            byte_buf_.pop_front(); // invalid
            return false;
        }
        if (static_cast<int>(byte_buf_.size()) < need)
            return false; // wait for more bytes
        if (need == 1) {
            byte_buf_.pop_front();
        } else if (need == 2) {
            cp = (static_cast<std::uint32_t>(byte_buf_[0] & 0x1F) << 6) |
                 (static_cast<std::uint32_t>(byte_buf_[1] & 0x3F));
            byte_buf_.pop_front();
            byte_buf_.pop_front();
        } else if (need == 3) {
            cp = (static_cast<std::uint32_t>(byte_buf_[0] & 0x0F) << 12) |
                 (static_cast<std::uint32_t>(byte_buf_[1] & 0x3F) << 6) |
                 (static_cast<std::uint32_t>(byte_buf_[2] & 0x3F));
            byte_buf_.pop_front();
            byte_buf_.pop_front();
            byte_buf_.pop_front();
        } else {
            cp = (static_cast<std::uint32_t>(byte_buf_[0] & 0x07) << 18) |
                 (static_cast<std::uint32_t>(byte_buf_[1] & 0x3F) << 12) |
                 (static_cast<std::uint32_t>(byte_buf_[2] & 0x3F) << 6) |
                 (static_cast<std::uint32_t>(byte_buf_[3] & 0x3F));
            for (int i = 0; i < 4; ++i)
                byte_buf_.pop_front();
        }
        ev = {};
        ev.kind = InputEvent::Kind::Key;
        ev.ch = cp;
        if (cp < 32 && cp != kKeyEnter)
            ev.mods |= 1; // treat low control chars as ctrl (except Enter)
        return true;
    }

    bool parse_escape(InputEvent& ev) {
        // Need at least ESC
        if (byte_buf_.empty() || byte_buf_.front() != 0x1B)
            return false;
        // Peek: if only ESC and no more, treat as Escape key when no pending tty read.
        if (byte_buf_.size() == 1) {
            // Incomplete CSI possible on real tty — for inject path, single ESC is Escape.
            // If tty_raw_, leave for next poll to gather more; for inject/headless consume.
            if (tty_raw_)
                return false;
            byte_buf_.pop_front();
            ev = {};
            ev.kind = InputEvent::Kind::Key;
            ev.ch = kKeyEscape;
            return true;
        }
        // ESC [
        if (byte_buf_.size() >= 2 && byte_buf_[1] == '[') {
            // Collect until final byte 0x40-0x7E
            std::size_t i = 2;
            while (i < byte_buf_.size()) {
                const auto c = byte_buf_[i];
                if (c >= 0x40 && c <= 0x7E) {
                    // Have full CSI: bytes [0..i]
                    std::string csi;
                    for (std::size_t j = 2; j <= i; ++j)
                        csi.push_back(static_cast<char>(byte_buf_[j]));
                    // Drop ESC [ ... final
                    for (std::size_t j = 0; j <= i; ++j)
                        byte_buf_.pop_front();

                    // Arrow: A/B/C/D
                    if (csi.size() == 1) {
                        ev = {};
                        ev.kind = InputEvent::Kind::Key;
                        switch (csi[0]) {
                            case 'A':
                                ev.ch = kKeyArrowUp;
                                break;
                            case 'B':
                                ev.ch = kKeyArrowDown;
                                break;
                            case 'C':
                                ev.ch = kKeyArrowRight;
                                break;
                            case 'D':
                                ev.ch = kKeyArrowLeft;
                                break;
                            default:
                                return false;
                        }
                        return true;
                    }
                    // Mouse SGR: <btn;col;row M/m
                    if (!csi.empty() && csi[0] == '<') {
                        int btn = 0, col = 0, row = 0;
                        char end = csi.back();
                        if (end == 'M' || end == 'm') {
                            // parse "<b;c;rX"
                            const auto body = csi.substr(1, csi.size() - 2);
                            int parts[3] = {0, 0, 0};
                            int pi = 0;
                            int cur = 0;
                            for (char ch : body) {
                                if (ch == ';') {
                                    if (pi < 3)
                                        parts[pi++] = cur;
                                    cur = 0;
                                } else if (ch >= '0' && ch <= '9') {
                                    cur = cur * 10 + (ch - '0');
                                }
                            }
                            if (pi < 3)
                                parts[pi] = cur;
                            btn = parts[0];
                            col = parts[1];
                            row = parts[2];
                            ev = {};
                            ev.kind = InputEvent::Kind::Mouse;
                            ev.btn = btn;
                            ev.col = col;
                            ev.row = row;
                            ev.press = (end == 'M');
                            return true;
                        }
                    }
                    return false;
                }
                ++i;
            }
            return false; // incomplete CSI
        }
        // ESC alone followed by non-[ → Alt+key or bare ESC
        byte_buf_.pop_front();
        if (!byte_buf_.empty() && byte_buf_.front() != '[') {
            // Alt + next key (simplified: mark mods, emit next as key on next call)
            // Consume as bare Escape for Phase 1.
        }
        ev = {};
        ev.kind = InputEvent::Kind::Key;
        ev.ch = kKeyEscape;
        return true;
    }

#if defined(__unix__) || defined(__APPLE__)
    static void sigint_handler(int sig) {
        global_tui_input().emergency_restore();
        ::signal(sig, SIG_DFL);
        ::raise(sig);
    }

    void install_sigint_handler() {
        if (sig_installed_)
            return;
        prev_sigint_ = ::signal(SIGINT, sigint_handler);
        sig_installed_ = true;
    }

    void restore_sigint_handler() {
        if (!sig_installed_)
            return;
        ::signal(SIGINT, prev_sigint_ ? prev_sigint_ : SIG_DFL);
        sig_installed_ = false;
    }

    struct termios orig_termios_{};
    void (*prev_sigint_)(int) = SIG_DFL;
    bool sig_installed_ = false;
#else
    void install_sigint_handler() {}
    void restore_sigint_handler() {}
#endif

    bool raw_mode_ = false;
    bool tty_raw_ = false;
    bool mouse_enabled_ = false;
    int input_fd_ = -1;
    bool owns_tty_fd_ = false;
    std::deque<std::uint8_t> byte_buf_;
};

inline TUIInput& global_tui_input() {
    static TUIInput in;
    return in;
}

} // namespace aura::tui

#endif // AURA_TUI_TUI_INPUT_HH
