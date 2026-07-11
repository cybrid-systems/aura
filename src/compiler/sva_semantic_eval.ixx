// sva_semantic_eval.ixx — Issue #1238 Phase 1: SVA property/sequence semantic eval scaffold.

module;

export module aura.compiler.sva_semantic_eval;

import std;

export namespace aura::compiler::sva_eval {

inline constexpr int kSvaSemanticEvalPhase = 1;

enum class AssertResult : std::uint8_t { Pass = 0, Fail = 1, Vacuous = 2, Unknown = 3 };

struct SvaEvalStats {
    std::uint64_t properties_evaled = 0;
    std::uint64_t sequences_matched = 0;
    std::uint64_t assertions_pass = 0;
    std::uint64_t assertions_fail = 0;
};

inline SvaEvalStats g_sva_eval_stats{};

// Phase 1 stub: always Unknown until waveform-backed peel lands.
[[nodiscard]] inline AssertResult eval_property(std::uint32_t /*prop_id*/) {
    ++g_sva_eval_stats.properties_evaled;
    return AssertResult::Unknown;
}

[[nodiscard]] inline bool match_sequence(std::uint32_t /*seq_id*/) {
    ++g_sva_eval_stats.sequences_matched;
    return false;
}

} // namespace aura::compiler::sva_eval
