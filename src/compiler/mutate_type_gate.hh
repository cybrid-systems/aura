// mutate_type_gate.hh — Issue #2219: post-mutate soft TypeError filter policy.
//                       Issue #2279: production lock contract.
//
// Soft (default for unit tests / AURA_SANDBOX=off):
//   Match exhaustiveness + pure TypeError noise soft-pass so selective
//   rebind/set-body fixtures stay green; counters track skips.
// Hard (production defaults / Restricted+Full audit):
//   Match exhaustiveness TypeError hard-rejects mutate (boundary rollback);
//   remaining TypeError rejects; Note/Warning stay non-fatal (AC4).
// Issue #3045: after dirty_propagation / evaluator_typecheck under-mark
// cone-force (variant add / arm delete), Hard still rejects exhaustiveness
// TypeErrors. Soft remains observe-only; Quiet (no ADT) does not change
// the gate.
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

#include "core/runtime_config.hh"
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
    const char* e = ::aura::config::raw(::aura::config::Key::AURA_MUTATE_TYPE_GATE);
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
    const char* e = ::aura::config::raw(::aura::config::Key::AURA_ALLOW_SOFT_TYPE_GATE);
    if (!e || !*e)
        return false;
    std::string_view v(e);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

// Read AURA_HARD_TYPE_GATE_ABORT; returns true if set to a truthy value.
[[nodiscard]] inline bool read_hard_type_gate_abort_env() noexcept {
    const char* e = ::aura::config::raw(::aura::config::Key::AURA_HARD_TYPE_GATE_ABORT);
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
// "match warning: unhandled constructor". #3045: Hard reject still keys off
// these messages after under-mark cone-force.
[[nodiscard]] inline bool is_match_exhaustiveness_msg(std::string_view msg) noexcept {
    return msg.find("missing constructor") != std::string_view::npos ||
           msg.find("missing constructors") != std::string_view::npos ||
           msg.find("unhandled constructor") != std::string_view::npos ||
           msg.find("match:") != std::string_view::npos ||
           msg.find("match warning") != std::string_view::npos;
}

// ———————————————————————————————————————————————————————————————————
// Issue #5317: the structural half of the #5253 constraint set.
//
// std/contract owns the constraint set and its Aura predicates; a predicate
// closure never crosses into C++. The structural half — protected names,
// each name's operator allow list, the set version and the detector
// registration — is published by std/contract as a compact digest on the
// existing #5251 decision trace (target "contract:constraints",
// reason "contract-digest cg1|..."), which is the std -> engine summary
// surface that exists. No new primitive, no new FFI, no second gate: this
// rides the same mutate:* pre-commit point and the same report vocabulary
// as #5255.
//
// Digest (versioned; names are identifiers, ops are operator names, so
// neither carries the "|" "," "=" "+" delimiters):
//
//   cg1|v=<set version>|h=<content hash>|p=<protected[,..]>|
//       a=<name=op+op[..]>|d=<detector/version/ids[,..]>
//
//   p  protected names
//   a  per-name operator allow list (absent name = any operator)
//   d  detector registration digest (name/version/detector ids)
//   v  the set version the digest describes; re-activating a changed set
//      republishes, so a version bump is auditable on the trace and visible
//      here.
//
// Operator identity: the engine sees the mutate:* primitive, not the std
// wrapper that called it (agent:round calls mutate :rebind), and no channel
// names the enclosing std operator. So:
//   * an EMPTY allow list means "no operator may change it" and is refused
//     here for every mutate:* — the direct-call bypass #5317 closes;
//   * an allow list that names ENGINE primitives ("mutate:rebind", ...) is
//     enforced here (the primitive must be listed);
//   * an allow list that names only std operators ("agent:round", ...) is
//     left to the std layer, which already ran contract:check-mutation on
//     that path — the engine cannot attribute the call to it, and refusing
//     would break the very path the allow list admits.
//
// With no digest (no activated set, or a process that only ever ran without
// constraints) the whole section is one relaxed atomic load and behavior is
// byte-identical to before #5317.
inline constexpr int kMutateContractGateIssue = 5317;
struct ContractAllowRow {
    std::string name;
    std::vector<std::string> ops;
};

struct ContractSummary {
    bool active = false;
    std::uint64_t version = 0;
    std::string hash;
    std::vector<std::string> protected_names;
    std::vector<ContractAllowRow> allow;
    std::vector<std::string> detectors;
    std::string raw;
};

inline std::atomic<std::uint64_t> g_contract_check_total{0};
inline std::atomic<std::uint64_t> g_contract_reject_total{0};
inline std::atomic<std::uint64_t> g_contract_deferred_total{0};
inline std::atomic<std::uint64_t> g_contract_parse_error_total{0};

inline std::atomic<std::uint64_t>& contract_installed_gen() noexcept {
    static std::atomic<std::uint64_t> g{0};
    return g;
}

inline std::atomic<bool>& contract_active_flag() noexcept {
    static std::atomic<bool> a{false};
    return a;
}

inline std::atomic<std::shared_ptr<const ContractSummary>>& contract_summary() noexcept {
    static std::atomic<std::shared_ptr<const ContractSummary>> s{
        std::make_shared<const ContractSummary>()};
    return s;
}

[[nodiscard]] inline std::uint64_t installed_contract_gen() noexcept {
    return contract_installed_gen().load(std::memory_order_acquire);
}

namespace contract_digest_detail {

    inline std::vector<std::string> split(std::string_view text, char sep) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t at = text.find(sep, start);
            const std::size_t end = at == std::string_view::npos ? text.size() : at;
            if (end > start)
                out.emplace_back(text.substr(start, end - start));
            if (at == std::string_view::npos)
                break;
            start = at + 1;
        }
        return out;
    }

    [[nodiscard]] inline bool parse_u64(std::string_view text, std::uint64_t& out) {
        if (text.empty() || text.size() > 20)
            return false;
        std::uint64_t v = 0;
        for (const char c : text) {
            if (c < '0' || c > '9')
                return false;
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        out = v;
        return true;
    }

} // namespace contract_digest_detail

// -> true when text is a well-formed cg1 digest. A malformed one leaves the
// summary inactive (fail-open + counter): a broken mirror must not wedge
// every mutate in the process.
[[nodiscard]] inline bool parse_contract_digest(std::string_view text, ContractSummary& out) {
    out = ContractSummary{};
    out.raw = std::string(text);
    if (!text.starts_with("cg1|"))
        return false;
    bool have_version = false;
    bool have_hash = false;
    for (const auto& seg : contract_digest_detail::split(text.substr(4), '|')) {
        const std::size_t eq = seg.find('=');
        if (eq == std::string::npos)
            return false;
        const std::string_view key = std::string_view(seg).substr(0, eq);
        const std::string_view val = std::string_view(seg).substr(eq + 1);
        if (key == "v") {
            if (!contract_digest_detail::parse_u64(val, out.version))
                return false;
            have_version = true;
        } else if (key == "h") {
            out.hash = std::string(val);
            have_hash = true;
        } else if (key == "p") {
            out.protected_names = contract_digest_detail::split(val, ',');
        } else if (key == "a") {
            for (const auto& entry : contract_digest_detail::split(val, ',')) {
                ContractAllowRow row;
                const std::size_t at = entry.find('=');
                if (at == std::string::npos)
                    return false;
                row.name = entry.substr(0, at);
                row.ops =
                    contract_digest_detail::split(std::string_view(entry).substr(at + 1), '+');
                out.allow.push_back(std::move(row));
            }
        } else if (key == "d") {
            out.detectors = contract_digest_detail::split(val, ',');
        }
        // Forward-compatible: an unknown member is ignored, not fatal.
    }
    if (!have_version || !have_hash)
        return false;
    out.active = true;
    return true;
}

// Install the newest digest std/contract published. Called only when the
// trace generation moved, so the steady state is one relaxed load per mutate.
inline void install_contract_digest(std::string_view text, std::uint64_t gen) {
    auto next = std::make_shared<ContractSummary>();
    if (!parse_contract_digest(text, *next)) {
        // "off" is std/contract's deactivate marker, not a malformed digest.
        if (!text.empty() && text != "off")
            g_contract_parse_error_total.fetch_add(1, std::memory_order_relaxed);
        next->active = false;
    }
    contract_summary().store(std::shared_ptr<const ContractSummary>(std::move(next)),
                             std::memory_order_release);
    contract_active_flag().store(contract_summary().load(std::memory_order_relaxed)->active,
                                 std::memory_order_release);
    contract_installed_gen().store(gen, std::memory_order_release);
}

struct ContractRuling {
    bool rejected = false;
    std::string name;
    std::string detail;
    std::vector<std::string> allowed;
    std::uint64_t version = 0;
    std::string digest;
};

// Only these mutate:* primitives address a definition BY NAME in their first
// argument. A node id, a literal, a snapshot tag or a policy name in that
// slot is not a define name, so the name-based protection must not read it
// as one (mutate :snapshot "gate" is a tag; mutate :tweak-literal "gate" is
// not a rebind of gate).
[[nodiscard]] inline bool contract_name_target(std::string_view prim) noexcept {
    return prim == "mutate:rebind" || prim == "mutate:set-body" || prim == "mutate:rename-symbol";
}

// The pre-commit structural ruling for one mutation of a name by prim.
// Empty name (node-id / batch entry) passes: name-based protection cannot
// be attributed without a name, and the std layer's delta check covers it.
[[nodiscard]] inline ContractRuling check_contract_mutation(std::string_view prim,
                                                            std::string_view name) {
    ContractRuling r;
    if (name.empty() || !contract_name_target(prim) ||
        !contract_active_flag().load(std::memory_order_relaxed))
        return r;
    // A re-install can race a check; the shared_ptr load keeps the summary
    // alive for the duration of this decision.
    const auto summary = contract_summary().load(std::memory_order_acquire);
    if (!summary || !summary->active)
        return r;
    const bool protected_name =
        std::find(summary->protected_names.begin(), summary->protected_names.end(), name) !=
        summary->protected_names.end();
    if (!protected_name)
        return r;
    g_contract_check_total.fetch_add(1, std::memory_order_relaxed);
    r.name = std::string(name);
    r.version = summary->version;
    r.digest = summary->hash;
    const ContractAllowRow* row = nullptr;
    for (const auto& cand : summary->allow) {
        if (cand.name == name) {
            row = &cand;
            break;
        }
    }
    if (row == nullptr || row->ops.empty()) {
        // No operator may change it — the #5317 bypass face.
        r.rejected = true;
        r.detail = "protected-no-operator";
    } else {
        r.allowed = row->ops;
        bool names_engine_prim = false;
        bool allowed_here = false;
        for (const auto& op : row->ops) {
            if (op == prim)
                allowed_here = true;
            if (op.starts_with("mutate:"))
                names_engine_prim = true;
        }
        if (!allowed_here) {
            if (names_engine_prim) {
                r.rejected = true;
                r.detail = "protected-op-not-allowed";
            } else {
                // Std-operator allow list: std/contract already ran the
                // pre-check on the path that carries this operator name.
                g_contract_deferred_total.fetch_add(1, std::memory_order_relaxed);
                return r;
            }
        }
    }
    if (r.rejected)
        g_contract_reject_total.fetch_add(1, std::memory_order_relaxed);
    return r;
}

// The queryable reason a refusal carries: the same vocabulary as the #5255
// gate report (kind / version / invariant / subject / detail).
[[nodiscard]] inline std::string contract_refusal_message(const ContractRuling& r,
                                                          std::string_view prim) {
    std::string msg =
        "constraint set v" + std::to_string(r.version) + " (digest " + r.digest + ") protects ";
    msg += r.name;
    msg += ": ";
    msg += r.detail;
    msg += " (op ";
    msg += prim;
    if (r.allowed.empty()) {
        msg += "; no operator may change it)";
    } else {
        msg += "; allowed: ";
        for (std::size_t i = 0; i < r.allowed.size(); ++i) {
            if (i != 0)
                msg += ",";
            msg += r.allowed[i];
        }
        msg += ")";
    }
    return msg;
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
    // Issue #5317: the structural #5253 mirror.
    std::int64_t contract_active = 0;
    std::uint64_t contract_version = 0;
    std::uint64_t contract_generation = 0;
    std::uint64_t contract_check_total = 0;
    std::uint64_t contract_reject_total = 0;
    std::uint64_t contract_deferred_total = 0;
    std::uint64_t contract_parse_error_total = 0;
    int schema_contract = kMutateContractGateIssue;
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
    const auto contract = contract_summary().load(std::memory_order_relaxed);
    s.contract_active = contract && contract->active ? 1 : 0;
    s.contract_version = contract ? contract->version : 0;
    s.contract_generation = installed_contract_gen();
    s.contract_check_total = g_contract_check_total.load(std::memory_order_relaxed);
    s.contract_reject_total = g_contract_reject_total.load(std::memory_order_relaxed);
    s.contract_deferred_total = g_contract_deferred_total.load(std::memory_order_relaxed);
    s.contract_parse_error_total = g_contract_parse_error_total.load(std::memory_order_relaxed);
    s.schema_contract = kMutateContractGateIssue;
    return s;
}

inline void reset_for_test() noexcept {
    set_mode(MutateTypeGate::Soft);
    g_soft_type_skip_total.store(0, std::memory_order_relaxed);
    g_exhaustiveness_reject_total.store(0, std::memory_order_relaxed);
    g_hard_type_error_reject_total.store(0, std::memory_order_relaxed);
    g_gate_check_total.store(0, std::memory_order_relaxed);
    g_production_locked.store(0, std::memory_order_relaxed);
    // #5317: tests start from "no constraint declared" so every pre-existing
    // expectation sees the same bytes as before the mirror existed.
    contract_summary().store(std::make_shared<const ContractSummary>(), std::memory_order_relaxed);
    contract_active_flag().store(false, std::memory_order_relaxed);
    contract_installed_gen().store(0, std::memory_order_relaxed);
    g_contract_check_total.store(0, std::memory_order_relaxed);
    g_contract_reject_total.store(0, std::memory_order_relaxed);
    g_contract_deferred_total.store(0, std::memory_order_relaxed);
    g_contract_parse_error_total.store(0, std::memory_order_relaxed);
    g_soft_override_allowed.store(0, std::memory_order_relaxed);
    g_soft_in_production_alarm_total.store(0, std::memory_order_relaxed);
}

} // namespace aura::compiler::mutate_type_gate

#endif // AURA_COMPILER_MUTATE_TYPE_GATE_HH
