// batch_terminal.ixx — Issue #1175 Phase 1: renderer module scaffold.
// Future: efficient batch ANSI sequence construction + terminal dirty regions.

module;

export module aura.renderer.batch_terminal;

import std;

export namespace aura::renderer {

// Phase 1 sentinel: module is linkable; real batching lands in follow-up PRs.
inline constexpr int kBatchTerminalPhase = 1;

struct BatchTerminalStats {
    std::uint64_t sequences_emitted = 0;
    std::uint64_t dirty_rects = 0;
};

inline BatchTerminalStats g_batch_terminal_stats{};

} // namespace aura::renderer
