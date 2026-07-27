// mutate_type_gate.hh — Issue #2219: post-mutate soft TypeError filter policy.
//
// Soft (default for unit tests / AURA_SANDBOX=off):
//   Match exhaustiveness + pure TypeError noise soft-pass so selective
//   rebind/set-body fixtures stay green; counters track skips.
// Hard (production defaults / Restricted+Full audit):
//   Match exhaustiveness TypeError hard-rejects mutate (boundary rollback);
//   remaining TypeError rejects; Note/Warning stay non-fatal (AC4).
//
// Env: AURA_MUTATE_TYPE_GATE=soft|hard  (always wins when set)
// Production: apply_production_security_defaults → Hard unless sandbox=off.

#ifndef AURA_COMPILER_MUTATE_TYPE_GATE_HH
#define AURA_COMPILER_MUTATE_TYPE_GATE_HH

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler::mutate_type_gate {

inline constexpr int kMutateTypeGateIssue = 2219;

enum class MutateTypeGate : std::uint8_t { Soft = 0, Hard = 1 };

// Process-wide mode. Default Soft so unit tests / fuzzer do not mass-break.
inline std::atomic<std::uint8_t>& g_mode() noexcept {
    static std::atomic<std::uint8_t> m{static_cast<std::uint8_t>(MutateTypeGate::Soft)};
    return m;
}

// Observability (process-wide freestanding atomics — avoid static Metrics
// aggregate which interacted badly with DefineType load on aarch64 tests).
inline std::atomic<std::uint64_t> g_soft_type_skip_total{0};
inline std::atomic<std::uint64_t> g_exhaustiveness_reject_total{0};
inline std::atomic<std::uint64_t> g_hard_type_error_reject_total{0};
inline std::atomic<std::uint64_t> g_gate_check_total{0};
inline std::atomic<std::uint64_t> g_wired{1};

inline void set_mode(MutateTypeGate m) noexcept {
    g_mode().store(static_cast<std::uint8_t>(m), std::memory_order_relaxed);
}

[[nodiscard]] inline MutateTypeGate mode() noexcept {
    return static_cast<MutateTypeGate>(g_mode().load(std::memory_order_relaxed));
}

[[nodiscard]] inline bool is_hard() noexcept {
    return mode() == MutateTypeGate::Hard;
}

// Apply AURA_MUTATE_TYPE_GATE env when set; returns true if env applied.
inline bool apply_env_override() noexcept {
    const char* e = std::getenv("AURA_MUTATE_TYPE_GATE");
    if (!e || !*e)
        return false;
    std::string_view v(e);
    if (v == "hard" || v == "1" || v == "true" || v == "on" || v == "strict") {
        set_mode(MutateTypeGate::Hard);
        return true;
    }
    if (v == "soft" || v == "0" || v == "false" || v == "off") {
        set_mode(MutateTypeGate::Soft);
        return true;
    }
    return false;
}

// Production path: Hard unless AURA_SANDBOX=off (dev Soft ergonomics).
// AURA_MUTATE_TYPE_GATE always wins when set.
inline void apply_production_defaults(bool dev_sandbox_off) noexcept {
    if (apply_env_override())
        return;
    set_mode(dev_sandbox_off ? MutateTypeGate::Soft : MutateTypeGate::Hard);
}

// Match exhaustiveness / ADT soft-filter message detection (shared with TC).
// Covers type_checker format_match_exhaustiveness_message + eval_flat runtime
// "match warning: unhandled constructor".
[[nodiscard]] inline bool is_match_exhaustiveness_msg(std::string_view msg) noexcept {
    return msg.find("missing constructor") != std::string_view::npos ||
           msg.find("missing constructors") != std::string_view::npos ||
           msg.find("unhandled constructor") != std::string_view::npos ||
           msg.find("match:") != std::string_view::npos ||
           msg.find("match warning") != std::string_view::npos;
}

struct Snapshot {
    std::int64_t mode = 0; // 0 soft, 1 hard
    std::uint64_t soft_type_skip_total = 0;
    std::uint64_t exhaustiveness_reject_total = 0;
    std::uint64_t hard_type_error_reject_total = 0;
    std::uint64_t gate_check_total = 0;
    std::uint64_t wired = 1;
    int schema = kMutateTypeGateIssue;
};

[[nodiscard]] inline Snapshot snapshot() noexcept {
    Snapshot s;
    s.mode = static_cast<std::int64_t>(g_mode().load(std::memory_order_relaxed));
    s.soft_type_skip_total = g_soft_type_skip_total.load(std::memory_order_relaxed);
    s.exhaustiveness_reject_total = g_exhaustiveness_reject_total.load(std::memory_order_relaxed);
    s.hard_type_error_reject_total = g_hard_type_error_reject_total.load(std::memory_order_relaxed);
    s.gate_check_total = g_gate_check_total.load(std::memory_order_relaxed);
    s.wired = g_wired.load(std::memory_order_relaxed);
    s.schema = kMutateTypeGateIssue;
    return s;
}

inline void reset_for_test() noexcept {
    set_mode(MutateTypeGate::Soft);
    g_soft_type_skip_total.store(0, std::memory_order_relaxed);
    g_exhaustiveness_reject_total.store(0, std::memory_order_relaxed);
    g_hard_type_error_reject_total.store(0, std::memory_order_relaxed);
    g_gate_check_total.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler::mutate_type_gate

#endif // AURA_COMPILER_MUTATE_TYPE_GATE_HH
