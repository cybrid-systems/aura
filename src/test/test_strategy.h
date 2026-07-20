// test_strategy.h — Issue #1887: high-level hot-path & AI self-mod test strategy.
//
// Purpose: single authority for hot-path coverage matrix + process-wide
//   strategy metrics (hit rate, self-mod loop SLO stamps).
// Layer: test framework (header so production query + tests share counters)
// See also: tests/STRATEGY.md, docs/naming_convention.md
//
// Safety Class: P2 (observability for tests / agent dashboards)
// Issue: #1887
// AI-Native Rationale: agents and humans pick scenarios by name/issue and
//   read coverage-bp via query:test-strategy-stats without scanning tests/

#ifndef AURA_TEST_TEST_STRATEGY_H
#define AURA_TEST_TEST_STRATEGY_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aura::test::strategy {

// ── Inventory ──────────────────────────────────────────────
inline constexpr int kTestStrategyIssue = 1887;
inline constexpr int kTestStrategyPhase = 1;
inline constexpr int kTestStrategySchema = 1887;

// Self-mod closed-loop SLO (loops a full strategy suite should stamp).
inline constexpr std::uint64_t kSelfModMinLoopsSlo = 1000;
// Production-readiness: at least half the matrix scenarios hit (bp).
inline constexpr std::uint64_t kHotPathMinCoverageBp = 5000;

// ── Hot-path scenarios (coverage matrix) ───────────────────
//
// Each row links ≥1 P0/P1 production-readiness issue. Prefer domain suites
// (tests/domain/) over new test_issue_*.cpp when extending coverage.
enum class HotPathScenario : std::uint8_t {
    MutateStealGcOldClosure = 0, // mutate + fiber steal + GC + old closure
    InvalidateJitDeopt = 1,      // invalidate + JIT deopt path
    FiberGuardShapeEpoch = 2,    // GuardShape / epoch under fiber
    TypedMutationInvariant = 3,  // TypedMutationAudit invariant suite
    TypePropInvariantCorr = 4,   // TypeProp/memo ↔ invariant (#1884)
    AotHotUpdateAudit = 5,       // AOT hot-update audit (#1882)
    SelfEvolutionLoop = 6,       // self-evolution loop aggregate (#1883)
    RenderHotpathMutation = 7,   // render hot-path under mutation
    Count = 8
};

inline constexpr int kHotPathScenarioCount = static_cast<int>(HotPathScenario::Count);

struct HotPathMatrixEntry {
    HotPathScenario id;
    const char* name;          // stable kebab id for logs / hash keys
    const char* title;         // human title
    int related_issue_primary; // primary tracking issue
    int related_issue_secondary;
    const char* recommended_suite; // domain or focused binary
};

// Coverage matrix — keep in sync with kHotPathScenarioCount.
inline constexpr HotPathMatrixEntry kHotPathMatrix[kHotPathScenarioCount] = {
    {HotPathScenario::MutateStealGcOldClosure, "mutate-steal-gc-old-closure",
     "Mutate + fiber steal + GC + old-closure eval",
     /*#*/ 1624, 1627, "domain/test_domain_fiber_orchestration"},
    {HotPathScenario::InvalidateJitDeopt, "invalidate-jit-deopt",
     "Invalidate function + JIT deopt / re-lower",
     /*#*/ 1623, 740, "test_eval_relower_hotpath_1623"},
    {HotPathScenario::FiberGuardShapeEpoch, "fiber-guard-shape-epoch",
     "GuardShape + epoch under multi-fiber",
     /*#*/ 836, 1627, "domain/test_domain_fiber_orchestration"},
    {HotPathScenario::TypedMutationInvariant, "typed-mutation-invariant",
     "TypedMutationAudit type/linear/provenance invariants",
     /*#*/ 1614, 1544, "domain/test_domain_typed_mutate"},
    {HotPathScenario::TypePropInvariantCorr, "type-prop-invariant-corr",
     "TypePropagation / DCE / memo ↔ invariant correlation",
     /*#*/ 1884, 1872, "test_type_prop_invariant_correlation_1884"},
    {HotPathScenario::AotHotUpdateAudit, "aot-hotupdate-audit",
     "AOT hot-update TypedMutationAudit (sampled ok / always-on fail)",
     /*#*/ 1882, 590, "test_aot_hotupdate_typed_audit_1882"},
    {HotPathScenario::SelfEvolutionLoop, "self-evolution-loop",
     "Self-evolution loop aggregate observability",
     /*#*/ 1883, 595, "domain/test_obs_schema_matrix"},
    {HotPathScenario::RenderHotpathMutation, "render-hotpath-mutation",
     "Render hot-path stability under high-frequency mutation",
     /*#*/ 1563, 1674, "test_render_hotpath_stability_under_mutation"},
};

[[nodiscard]] inline constexpr const HotPathMatrixEntry& matrix_entry(HotPathScenario s) noexcept {
    return kHotPathMatrix[static_cast<std::size_t>(s)];
}

[[nodiscard]] inline constexpr std::string_view scenario_name(HotPathScenario s) noexcept {
    if (static_cast<std::size_t>(s) >= static_cast<std::size_t>(HotPathScenario::Count))
        return "unknown";
    return kHotPathMatrix[static_cast<std::size_t>(s)].name;
}

// ── Process-wide metrics ───────────────────────────────────
struct TestStrategyCounters {
    std::atomic<std::uint64_t> scenario_hits[kHotPathScenarioCount]{};
    std::atomic<std::uint64_t> scenario_pass[kHotPathScenarioCount]{};
    std::atomic<std::uint64_t> scenario_fail[kHotPathScenarioCount]{};
    std::atomic<std::uint64_t> total_hits{0};
    std::atomic<std::uint64_t> total_pass{0};
    std::atomic<std::uint64_t> total_fail{0};
    std::atomic<std::uint64_t> self_mod_loops{0};
    std::atomic<std::uint64_t> self_mod_loops_ok{0};
    std::atomic<std::uint64_t> self_mod_loops_fail{0};
    std::atomic<std::uint64_t> profile_selections{0};
};

inline TestStrategyCounters g_test_strategy_counters{};

[[nodiscard]] inline bool valid_scenario(HotPathScenario s) noexcept {
    return static_cast<std::uint8_t>(s) < static_cast<std::uint8_t>(HotPathScenario::Count);
}

// Purpose: stamp one matrix scenario run (hit + pass/fail)
// Pre: s is a defined HotPathScenario
// Post: total_hits++; scenario_hits[s]++; pass or fail bucket
// Safety Class: P2
// Issue: #1887
// AI-Native Rationale: closed-loop harnesses report which hot paths executed
inline void note_hotpath_scenario(HotPathScenario s, bool pass = true) noexcept {
    if (!valid_scenario(s))
        return;
    const auto i = static_cast<std::size_t>(s);
    auto& c = g_test_strategy_counters;
    c.scenario_hits[i].fetch_add(1, std::memory_order_relaxed);
    c.total_hits.fetch_add(1, std::memory_order_relaxed);
    if (pass) {
        c.scenario_pass[i].fetch_add(1, std::memory_order_relaxed);
        c.total_pass.fetch_add(1, std::memory_order_relaxed);
    } else {
        c.scenario_fail[i].fetch_add(1, std::memory_order_relaxed);
        c.total_fail.fetch_add(1, std::memory_order_relaxed);
    }
}

// Purpose: stamp AI self-mod closed-loop iteration
inline void note_self_mod_loop(bool ok = true) noexcept {
    auto& c = g_test_strategy_counters;
    c.self_mod_loops.fetch_add(1, std::memory_order_relaxed);
    if (ok)
        c.self_mod_loops_ok.fetch_add(1, std::memory_order_relaxed);
    else
        c.self_mod_loops_fail.fetch_add(1, std::memory_order_relaxed);
}

// Purpose: strategy-driven profile selection (harness picks a matrix row)
inline void note_profile_selection(HotPathScenario /*s*/) noexcept {
    g_test_strategy_counters.profile_selections.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t scenarios_hit_unique() noexcept {
    std::uint64_t n = 0;
    for (int i = 0; i < kHotPathScenarioCount; ++i) {
        if (g_test_strategy_counters.scenario_hits[static_cast<std::size_t>(i)].load(
                std::memory_order_relaxed) > 0)
            ++n;
    }
    return n;
}

// coverage_hit_rate in basis points: unique_hit / Count * 10000
[[nodiscard]] inline std::uint64_t coverage_hit_rate_bp() noexcept {
    if (kHotPathScenarioCount == 0)
        return 0;
    return (scenarios_hit_unique() * 10000ull) / static_cast<std::uint64_t>(kHotPathScenarioCount);
}

[[nodiscard]] inline bool self_mod_slo_met() noexcept {
    return g_test_strategy_counters.self_mod_loops.load(std::memory_order_relaxed) >=
           kSelfModMinLoopsSlo;
}

[[nodiscard]] inline bool hotpath_coverage_slo_met() noexcept {
    return coverage_hit_rate_bp() >= kHotPathMinCoverageBp;
}

inline void reset_for_test() noexcept {
    auto& c = g_test_strategy_counters;
    for (int i = 0; i < kHotPathScenarioCount; ++i) {
        c.scenario_hits[static_cast<std::size_t>(i)].store(0, std::memory_order_relaxed);
        c.scenario_pass[static_cast<std::size_t>(i)].store(0, std::memory_order_relaxed);
        c.scenario_fail[static_cast<std::size_t>(i)].store(0, std::memory_order_relaxed);
    }
    c.total_hits.store(0, std::memory_order_relaxed);
    c.total_pass.store(0, std::memory_order_relaxed);
    c.total_fail.store(0, std::memory_order_relaxed);
    c.self_mod_loops.store(0, std::memory_order_relaxed);
    c.self_mod_loops_ok.store(0, std::memory_order_relaxed);
    c.self_mod_loops_fail.store(0, std::memory_order_relaxed);
    c.profile_selections.store(0, std::memory_order_relaxed);
}

// Strategy profile: which scenarios a suite should stamp (bitmask).
enum class StrategyProfile : std::uint32_t {
    Minimal = 1u << static_cast<unsigned>(HotPathScenario::TypedMutationInvariant),
    HotPathCore = (1u << static_cast<unsigned>(HotPathScenario::MutateStealGcOldClosure)) |
                  (1u << static_cast<unsigned>(HotPathScenario::InvalidateJitDeopt)) |
                  (1u << static_cast<unsigned>(HotPathScenario::FiberGuardShapeEpoch)),
    AiSelfMod = (1u << static_cast<unsigned>(HotPathScenario::TypedMutationInvariant)) |
                (1u << static_cast<unsigned>(HotPathScenario::TypePropInvariantCorr)) |
                (1u << static_cast<unsigned>(HotPathScenario::AotHotUpdateAudit)) |
                (1u << static_cast<unsigned>(HotPathScenario::SelfEvolutionLoop)),
    Full = 0xFFu, // all scenarios in Count (low 8 bits)
};

[[nodiscard]] inline bool profile_includes(StrategyProfile p, HotPathScenario s) noexcept {
    if (!valid_scenario(s))
        return false;
    return (static_cast<std::uint32_t>(p) & (1u << static_cast<unsigned>(s))) != 0;
}

// Select scenarios for a profile (calls note_profile_selection for each).
inline void select_profile(StrategyProfile p) noexcept {
    for (int i = 0; i < kHotPathScenarioCount; ++i) {
        const auto s = static_cast<HotPathScenario>(i);
        if (profile_includes(p, s))
            note_profile_selection(s);
    }
}

} // namespace aura::test::strategy

#endif // AURA_TEST_TEST_STRATEGY_H
