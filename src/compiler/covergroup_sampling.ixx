// covergroup_sampling.ixx — Issue #1235 Phase 1: covergroup sampling / bin hits scaffold.

module;

export module aura.compiler.covergroup_sampling;

import std;

export namespace aura::compiler::covergroup {

inline constexpr int kCovergroupSamplingPhase = 1;

struct CoverpointSample {
    std::uint32_t coverpoint_id = 0;
    std::uint64_t value = 0;
};

struct CovergroupStats {
    std::uint64_t samples = 0;
    std::uint64_t bin_hits = 0;
    std::uint64_t cross_hits = 0;
};

inline CovergroupStats g_covergroup_stats{};

// Phase 1: record sample + bin hit counters (real IR peel follows).
inline void sample_coverpoint(std::uint32_t cp_id, std::uint64_t value) {
    ++g_covergroup_stats.samples;
    ++g_covergroup_stats.bin_hits;
    (void)cp_id;
    (void)value;
}

inline void sample_cross(std::uint32_t /*a*/, std::uint32_t /*b*/) {
    ++g_covergroup_stats.cross_hits;
}

} // namespace aura::compiler::covergroup
