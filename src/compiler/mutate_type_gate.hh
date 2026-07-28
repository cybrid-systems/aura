// mutate_type_gate.hh — Issue #2219: post-mutate soft TypeError filter policy.
//                       Issue #2279: production lock contract.
//
// Soft (default for unit tests / AURA_SANDBOX=off):
//   Match exhaustiveness + pure TypeError noise soft-pass so selective
//   rebind/set-body fixtures stay green; counters track skips.
// Hard (production defaults / Restricted+Full audit):
//   Match exhaustiveness TypeError hard-rejects mutate (boundary rollback);
//   remaining TypeError rejects; Note/Warning stay non-fatal (AC4).
//
// Env: AURA_MUTATE_TYPE_GATE=soft|hard  (always wins when set)
// Env: AURA_ALLOW_SOFT_TYPE_GATE=1      (production-only opt-out — #2279)
// Env: AURA_HARD_TYPE_GATE_ABORT=1      (process abort on Soft-in-prod, #2279)
// Production: apply_production_security_defaults → Hard unless sandbox=off
//             or AURA_ALLOW_SOFT_TYPE_GATE=1; lock is set in either case.
//
// #2279 Decision table (Soft vs Hard vs env):
//   ┌──────────────────────────┬───────────┬──────────────────────┬──────────┐
//   │ Context                  │ Mode      │ production_locked    │ Behavior │
//   ├──────────────────────────┼───────────┼──────────────────────┼──────────┤
//   │ Unit test (default)      │ Soft      │ 0 (no prod apply)    │ soft-pass│
//   │ AURA_SANDBOX=off         │ Soft      │ 0 (dev sandbox)      │ soft-pass│
//   │ Production (no env)      │ Hard      │ 1                    │ hard-rej │
//   │ Prod + AURA_MUTATE_TYPE_ │ Hard      │ 1                    │ hard-rej │
//   │  GATE=hard               │           │                      │          │
//   │ Prod + AURA_MUTATE_TYPE_ │ Soft*     │ 1 (forced)           │ ALARM*   │
//   │  GATE=soft (no override) │           │                      │          │
//   │ Prod + AURA_ALLOW_SOFT_  │ Soft      │ 1                    │ soft-pass│
//   │  TYPE_GATE=1             │           │ (override allowed)   │ (with    │
//   │                          │           │                      │  alarm)  │
//   │ Prod + AURA_HARD_TYPE_   │ Soft      │ 1                    │ ABORT    │
//   │  GATE_ABORT=1 + Soft     │           │                      │          │
//   │ Prod + AURA_ALLOW_SOFT_  │ Soft      │ 1                    │ ABORT    │
//   │  TYPE_GATE=1 + AURA_HARD │           │                      │          │
//   │  _TYPE_GATE_ABORT=1      │           │                      │          │
//   └──────────────────────────┴───────────┴──────────────────────┴──────────┘
//   * AC1: `apply_production_defaults` upgrades Soft → Hard when !override,
//     so the "Prod + AURA_MUTATE_TYPE_GATE=soft (no override)" row only
//     persists for the brief window before apply_production_security_defaults
//     runs (the issue's *intent*). After that, mode is forced Hard.

#ifndef AURA_COMPILER_MUTATE_TYPE_GATE_HH
#define AURA_COMPILER_MUTATE_TYPE_GATE_HH

#include <cstdio>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace aura::compiler::mutate_type_gate {

inline constexpr int kMutateTypeGateIssue = 2219;
inline constexpr int kMutateTypeGateLockIssue = 2279;

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
// Issue #2279: production lock + soft-override + alarm counters. Lock
// transitions 0→1 only via set_production_locked (called from
// apply_production_security_defaults). soft_override_allowed is set from
// AURA_ALLOW_SOFT_TYPE_GATE at production-defaults apply time. alarm
// counter bumps whenever production_locked && current mode == Soft —
// catches a mis-deployed binary that soft-skips TypeError / match
// exhaustiveness under production (e.g. AURA_MUTATE_TYPE_GATE=soft
// in a prod env without override).
inline std::atomic<std::uint8_t> g_production_locked{0};
inline std::atomic<std::uint8_t> g_soft_override_allowed{0};
inline std::atomic<std::uint64_t> g_soft_in_production_alarm_total{0};

inline void set_mode(MutateTypeGate m) noexcept {
    g_mode().store(static_cast<std::uint8_t>(m), std::memory_order_relaxed);
}

[[nodiscard]] inline MutateTypeGate mode() noexcept {
    return static_cast<MutateTypeGate>(g_mode().load(std::memory_order_relaxed));
}

[[nodiscard]] inline bool is_hard() noexcept {
    return mode() == MutateTypeGate::Hard;
}

[[nodiscard]] inline bool production_locked() noexcept {
    return g_production_locked.load(std::memory_order_relaxed) != 0;
}

inline void set_production_locked(bool v) noexcept {
    g_production_locked.store(v ? 1 : 0, std::memory_order_relaxed);
}

[[nodiscard]] inline bool allow_soft_override() noexcept {
    return g_soft_override_allowed.load(std::memory_order_relaxed) != 0;
}

inline void set_soft_override_allowed(bool v) noexcept {
    g_soft_override_allowed.store(v ? 1 : 0, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t soft_in_production_alarm_total() noexcept {
    return g_soft_in_production_alarm_total.load(std::memory_order_relaxed);
}

inline void bump_soft_in_production_alarm() noexcept {
    g_soft_in_production_alarm_total.fetch_add(1, std::memory_order_relaxed);
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

// Read AURA_ALLOW_SOFT_TYPE_GATE; returns true if set to a truthy value.
[[nodiscard]] inline bool read_allow_soft_override_env() noexcept {
    const char* e = std::getenv("AURA_ALLOW_SOFT_TYPE_GATE");
    if (!e || !*e)
        return false;
    std::string_view v(e);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Read AURA_HARD_TYPE_GATE_ABORT; returns true if set to a truthy value.
[[nodiscard]] inline bool read_hard_type_gate_abort_env() noexcept {
    const char* e = std::getenv("AURA_HARD_TYPE_GATE_ABORT");
    if (!e || !*e)
        return false;
    std::string_view v(e);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Issue #2279: production startup / first-mutate check. Bumps alarm
// counter when production_locked && mode == Soft. If
// AURA_HARD_TYPE_GATE_ABORT=1, process aborts (fail-closed). Called
// from run_post_mutate_typecheck_no_lock and from
// apply_production_security_defaults after the lock is set.
inline void check_soft_in_production_or_abort() noexcept {
    if (!production_locked())
        return;
    if (is_hard())
        return;
    // Soft under production lock = mis-deployed binary. Bump alarm
    // and either abort (fail-closed) or stderr-warn (operator-visible).
    bump_soft_in_production_alarm();
    if (read_hard_type_gate_abort_env()) {
        std::fprintf(stderr, "[aura] FATAL: Soft MutateTypeGate under production lock — "
                             "AURA_HARD_TYPE_GATE_ABORT=1; aborting.\n");
        std::abort();
    } else {
        static std::atomic<bool> warned{false};
        bool expected = false;
        if (warned.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            std::fprintf(
                stderr,
                "[aura] WARN: Soft MutateTypeGate under production lock — "
                "AURA_ALLOW_SOFT_TYPE_GATE=1 to keep, AURA_MUTATE_TYPE_GATE=hard to force Hard, "
                "AURA_HARD_TYPE_GATE_ABORT=1 to fail-closed.\n");
        }
    }
}

// Production path (Issue #2219 + #2279):
//   1. Read AURA_ALLOW_SOFT_TYPE_GATE → soft_override_allowed.
//   2. Apply AURA_MUTATE_TYPE_GATE env (sets mode if env present; Soft
//      or Hard are both valid outcomes here).
//   3. dev_sandbox_off → keep Soft if env did not set Hard (unit
//      Soft-path ergonomics; dev never locks so alarm never fires).
//   4. Production profile (dev_sandbox_off=false):
//      a. If mode == Soft AND !soft_override_allowed → force Hard
//         (AC1: even an env-set Soft is overridden in production
//         unless the explicit dev-only opt-out was set). This is the
//         core of the #2279 contract — a misconfigured binary
//         (AURA_MUTATE_TYPE_GATE=soft in prod without override) must
//         not ship unsafe Agent mutates.
//      b. If mode == Soft AND soft_override_allowed  → keep Soft
//         (AC1: explicit dev-only escape). Caller (after setting
//         production_locked) bumps alarm on first mutate.
//   5. Otherwise (mode == Hard): nothing to do — env or default set
//      it, production agrees.
inline void apply_production_defaults(bool dev_sandbox_off) noexcept {
    set_soft_override_allowed(read_allow_soft_override_env());
    const bool env_applied = apply_env_override();
    if (dev_sandbox_off) {
        // Dev sandbox: default Soft (unit ergonomics). If env set
        // Hard, keep Hard (explicit). Never force-Hard, never lock,
        // never alarm.
        if (!env_applied) {
            set_mode(MutateTypeGate::Soft);
        }
        return;
    }
    // Production: force Hard if Soft + no explicit override.
    if (mode() == MutateTypeGate::Soft && !allow_soft_override()) {
        set_mode(MutateTypeGate::Hard);
    }
    // If Soft + override: keep Soft; caller (apply_production_security_defaults)
    // sets production_locked and the runtime check_soft_in_production_or_abort
    // bumps alarm on first mutate.
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
    // Issue #2279 lock state.
    std::int64_t production_locked = 0;     // 0/1 mirror of g_production_locked
    std::int64_t soft_override_allowed = 0; // 0/1 mirror of g_soft_override_allowed
    std::uint64_t soft_in_production_alarm_total = 0;
    int schema = kMutateTypeGateIssue;
    int schema_lock = kMutateTypeGateLockIssue;
};

[[nodiscard]] inline Snapshot snapshot() noexcept {
    Snapshot s;
    s.mode = static_cast<std::int64_t>(g_mode().load(std::memory_order_relaxed));
    s.soft_type_skip_total = g_soft_type_skip_total.load(std::memory_order_relaxed);
    s.exhaustiveness_reject_total = g_exhaustiveness_reject_total.load(std::memory_order_relaxed);
    s.hard_type_error_reject_total = g_hard_type_error_reject_total.load(std::memory_order_relaxed);
    s.gate_check_total = g_gate_check_total.load(std::memory_order_relaxed);
    s.wired = g_wired.load(std::memory_order_relaxed);
    s.production_locked =
        static_cast<std::int64_t>(g_production_locked.load(std::memory_order_relaxed));
    s.soft_override_allowed =
        static_cast<std::int64_t>(g_soft_override_allowed.load(std::memory_order_relaxed));
    s.soft_in_production_alarm_total =
        g_soft_in_production_alarm_total.load(std::memory_order_relaxed);
    s.schema = kMutateTypeGateIssue;
    s.schema_lock = kMutateTypeGateLockIssue;
    return s;
}

inline void reset_for_test() noexcept {
    set_mode(MutateTypeGate::Soft);
    g_soft_type_skip_total.store(0, std::memory_order_relaxed);
    g_exhaustiveness_reject_total.store(0, std::memory_order_relaxed);
    g_hard_type_error_reject_total.store(0, std::memory_order_relaxed);
    g_gate_check_total.store(0, std::memory_order_relaxed);
    g_production_locked.store(0, std::memory_order_relaxed);
    g_soft_override_allowed.store(0, std::memory_order_relaxed);
    g_soft_in_production_alarm_total.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler::mutate_type_gate

#endif // AURA_COMPILER_MUTATE_TYPE_GATE_HH
