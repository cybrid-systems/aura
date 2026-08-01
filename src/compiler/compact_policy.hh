// compact_policy.hh — Issue #2500: Agent compact action policy from
// existing mutation-concurrency health + arena frag + GC defer + hold.
//
// Pure, read-only recommendation. Does NOT mutate Guard / Soft-gate /
// Force hard-mutex / densify semantics (AC4). Agents consume the hash
// as advisory unless a future AURA_COMPACT_POLICY=enforce path is added.
//
// ── Mode (AC1 table) ──
//
//   soft        — Soft-only window (safe under hold / pin / low health)
//   force       — Force window candidate (high frag + clear defer)
//   skip        — no compact needed (healthy, low frag)
//   split-batch — hold estimate > 0.7×budget (#2405 align)
//
// ── Priority (first match wins) ──
//
//   1. recommend_split                     → split-batch
//   2. should_defer | active_guard | hold  → soft   (AC2: never Force)
//   3. pin_contract_fail | densify_fail    → soft
//   4. health_bp < health_budget_bp        → soft
//   5. frag_bp > 4000 (0.4) + clear path   → force  (AC3)
//   6. frag_bp <= 1000 + health high       → skip
//   7. default                             → soft
//
// Fragmentation is basis points: 4000 = 0.4, 10000 = 1.0.

#ifndef AURA_COMPILER_COMPACT_POLICY_HH
#define AURA_COMPILER_COMPACT_POLICY_HH

#include <cstdint>
#include <string_view>

namespace aura::compiler {

inline constexpr int kCompactPolicyIssue = 2500;
// frag > 0.4 → Force candidate when path is clear.
inline constexpr std::uint64_t kCompactPolicyForceFragBp = 4000;
// frag ≤ 0.1 with healthy score → skip.
inline constexpr std::uint64_t kCompactPolicySkipFragBp = 1000;

enum class CompactPolicyMode : std::uint8_t {
    Soft = 0,
    Force = 1,
    Skip = 2,
    SplitBatch = 3,
};

struct CompactPolicyInput {
    std::uint64_t health_bp = 10000;
    std::uint64_t health_budget_bp = 8000;
    std::uint64_t frag_bp = 0; // 0..10000
    bool should_defer_destructive_gc = false;
    std::uint64_t active_guard_depth = 0;
    bool mutation_hold_active = false; // Guard depth or mutation-hold defer bit
    std::uint64_t pin_contract_fail_total = 0;
    std::uint64_t densify_consistency_fail_total = 0;
    std::uint64_t force_blocked_by_pin_total = 0;
    std::uint64_t force_blocked_by_envframe_total = 0;
    bool recommend_split = false; // hold estimate > 0.7×budget
};

struct CompactPolicyResult {
    CompactPolicyMode mode = CompactPolicyMode::Soft;
    std::int64_t mode_code = 0;
    std::string_view mode_name = "soft";
    std::string_view reason = "default-soft";
    CompactPolicyInput inputs{};
};

[[nodiscard]] inline std::string_view compact_policy_mode_name(CompactPolicyMode m) noexcept {
    switch (m) {
        case CompactPolicyMode::Force:
            return "force";
        case CompactPolicyMode::Skip:
            return "skip";
        case CompactPolicyMode::SplitBatch:
            return "split-batch";
        case CompactPolicyMode::Soft:
        default:
            return "soft";
    }
}

[[nodiscard]] inline bool compact_policy_path_clear(const CompactPolicyInput& in) noexcept {
    if (in.should_defer_destructive_gc)
        return false;
    if (in.active_guard_depth > 0 || in.mutation_hold_active)
        return false;
    if (in.pin_contract_fail_total > 0 || in.densify_consistency_fail_total > 0)
        return false;
    if (in.force_blocked_by_pin_total > 0 || in.force_blocked_by_envframe_total > 0)
        return false;
    return true;
}

// Pure policy from a snapshot (no atomics — unit-testable AC1–AC3).
[[nodiscard]] inline CompactPolicyResult
compute_compact_policy(const CompactPolicyInput& in) noexcept {
    CompactPolicyResult r;
    r.inputs = in;

    // 1. Hold estimate → split batch (#2405).
    if (in.recommend_split) {
        r.mode = CompactPolicyMode::SplitBatch;
        r.mode_code = 3;
        r.mode_name = compact_policy_mode_name(r.mode);
        r.reason = "hold-estimate-split";
        return r;
    }

    // 2. Defer / live hold / EnvFrame guard → Soft only (AC2 never Force).
    if (in.should_defer_destructive_gc || in.active_guard_depth > 0 || in.mutation_hold_active) {
        r.mode = CompactPolicyMode::Soft;
        r.mode_code = 0;
        r.mode_name = compact_policy_mode_name(r.mode);
        r.reason = "defer-or-hold";
        return r;
    }

    // 3. Pin contract / densify consistency failures → Soft only.
    if (in.pin_contract_fail_total > 0 || in.densify_consistency_fail_total > 0 ||
        in.force_blocked_by_pin_total > 0 || in.force_blocked_by_envframe_total > 0) {
        r.mode = CompactPolicyMode::Soft;
        r.mode_code = 0;
        r.mode_name = compact_policy_mode_name(r.mode);
        r.reason = "pin-or-densify";
        return r;
    }

    // 4. Low mutation-concurrency health → Soft only.
    if (in.health_bp < in.health_budget_bp) {
        r.mode = CompactPolicyMode::Soft;
        r.mode_code = 0;
        r.mode_name = compact_policy_mode_name(r.mode);
        r.reason = "low-health";
        return r;
    }

    // 5. High frag + clear path → Force window (AC3).
    if (in.frag_bp > kCompactPolicyForceFragBp && compact_policy_path_clear(in)) {
        r.mode = CompactPolicyMode::Force;
        r.mode_code = 1;
        r.mode_name = compact_policy_mode_name(r.mode);
        r.reason = "high-frag-clear";
        return r;
    }

    // 6. Low frag + healthy → skip.
    if (in.frag_bp <= kCompactPolicySkipFragBp && in.health_bp >= in.health_budget_bp) {
        r.mode = CompactPolicyMode::Skip;
        r.mode_code = 2;
        r.mode_name = compact_policy_mode_name(r.mode);
        r.reason = "healthy-low-frag";
        return r;
    }

    // 7. Default Soft (conservative).
    r.mode = CompactPolicyMode::Soft;
    r.mode_code = 0;
    r.mode_name = compact_policy_mode_name(r.mode);
    r.reason = "default-soft";
    return r;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_COMPACT_POLICY_HH
