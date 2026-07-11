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
inline void ansi_sgr(std::string& out, int code) {
    out.push_back('\033');
    out.push_back('[');
    out.append(std::to_string(code));
    out.push_back('m');
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_cursor_move(std::string& out, int row, int col) {
    out.push_back('\033');
    out.push_back('[');
    out.append(std::to_string(row));
    out.push_back(';');
    out.append(std::to_string(col));
    out.push_back('H');
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

inline void ansi_clear_screen(std::string& out) {
    out.append("\033[2J");
    ++g_batch_terminal_stats.ansi_builds;
    ++g_batch_terminal_stats.sequences_emitted;
}

} // namespace aura::renderer
