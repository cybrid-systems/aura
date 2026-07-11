// eda_commercial_sim.ixx — Issue #1237 Phase 1: commercial EDA simulator integration scaffold.

module;

export module aura.compiler.eda_commercial_sim;

import std;

export namespace aura::compiler::eda_sim {

inline constexpr int kEdaCommercialSimPhase = 1;

enum class SimTool : std::uint8_t { None = 0, Vcs = 1, Questa = 2, Xcelium = 3 };

struct CommercialSimStats {
    std::uint64_t invokes = 0;
    std::uint64_t do_files = 0;
    std::uint64_t waveform_dumps = 0;
    SimTool last_tool = SimTool::None;
};

inline CommercialSimStats g_eda_commercial_sim_stats{};

// Phase 1: record invoke intent; real process spawn peel follows.
[[nodiscard]] inline bool invoke_simulator(SimTool tool, std::string_view /*do_file*/) {
    ++g_eda_commercial_sim_stats.invokes;
    ++g_eda_commercial_sim_stats.do_files;
    g_eda_commercial_sim_stats.last_tool = tool;
    return tool != SimTool::None;
}

} // namespace aura::compiler::eda_sim
