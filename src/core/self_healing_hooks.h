// self_healing_hooks.h — Issue #1203 Phase 1 + #1582 policy-driven engine.
// Plain header (not a module) so quota-check / panic paths can include it
// without module graph churn.

#ifndef AURA_CORE_SELF_HEALING_HOOKS_H
#define AURA_CORE_SELF_HEALING_HOOKS_H

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

namespace aura::core::self_heal {

struct HealErrorView {
    std::string_view kind;
    std::string_view message;
    std::uint64_t code = 0;
};

enum class HealAction : std::uint8_t {
    None = 0,
    LimitedSelfMutate = 1,
    Degrade = 2,
    GracefulDrain = 3,
};

struct PolicyResult {
    HealAction action = HealAction::None;
    bool healed = false;
};

using SelfHealingHook = std::function<bool(const HealErrorView&)>;
// Function pointer policy (no heap on hot path).
using SelfHealPolicyFn = PolicyResult (*)(const HealErrorView&) noexcept;

inline std::mutex g_hooks_mu;
inline std::vector<SelfHealingHook> g_hooks;
inline std::atomic<SelfHealPolicyFn> g_policy_fn{nullptr};

inline std::atomic<std::uint64_t> g_triggers{0};
inline std::atomic<std::uint64_t> g_hook_ok{0};
inline std::atomic<std::uint64_t> g_hook_fail{0};
inline std::atomic<std::uint64_t> g_self_heal_success_total{0};
inline std::atomic<std::uint64_t> g_self_heal_degrade_total{0};
inline std::atomic<std::uint64_t> g_self_heal_graceful_drain_total{0};
inline std::atomic<std::uint64_t> g_self_heal_policy_runs{0};
inline std::atomic<std::uint64_t> g_self_heal_limited_mutate_total{0};

inline std::atomic<bool> g_graceful_drain_active{false};
inline std::atomic<std::uint64_t> g_graceful_drain_requests{0};
inline std::atomic<std::uint64_t> g_graceful_drain_completed{0};
inline std::atomic<std::uint64_t> g_graceful_drain_last_code{0};
inline std::atomic<bool> g_degraded_mode{false};
inline std::atomic<std::uint64_t> g_degrade_enters{0};

[[nodiscard]] inline bool is_quota_kind(std::string_view kind) noexcept {
    return kind == "quota-violation" || kind == "ResourceQuotaExceeded" || kind == "quota";
}
[[nodiscard]] inline bool is_recoverable_panic_kind(std::string_view kind) noexcept {
    return kind == "recoverable-panic" || kind == "panic-checkpoint" || kind == "panic-restore" ||
           kind == "panic";
}
[[nodiscard]] inline bool is_quota_or_recoverable_panic(const HealErrorView& err) noexcept {
    return is_quota_kind(err.kind) || is_recoverable_panic_kind(err.kind);
}
[[nodiscard]] inline bool is_graceful_drain_request(const HealErrorView& err) noexcept {
    return err.kind == "graceful-drain" || err.message == "drain" ||
           err.message == "graceful-drain";
}

inline bool request_graceful_drain(std::uint64_t reason_code = 0) noexcept {
    g_graceful_drain_requests.fetch_add(1, std::memory_order_relaxed);
    g_graceful_drain_last_code.store(reason_code, std::memory_order_relaxed);
    bool expected = false;
    if (g_graceful_drain_active.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
        g_self_heal_graceful_drain_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return true;
}
[[nodiscard]] inline bool is_graceful_drain_active() noexcept {
    return g_graceful_drain_active.load(std::memory_order_acquire);
}
inline void complete_graceful_drain() noexcept {
    if (g_graceful_drain_active.exchange(false, std::memory_order_acq_rel))
        g_graceful_drain_completed.fetch_add(1, std::memory_order_relaxed);
}
[[nodiscard]] inline bool is_degraded_mode() noexcept {
    return g_degraded_mode.load(std::memory_order_acquire);
}
inline bool enter_degraded_mode() noexcept {
    bool expected = false;
    if (g_degraded_mode.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        g_degrade_enters.fetch_add(1, std::memory_order_relaxed);
        g_self_heal_degrade_total.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return true;
}
inline void clear_degraded_mode() noexcept {
    g_degraded_mode.store(false, std::memory_order_release);
}

inline PolicyResult default_self_heal_policy(const HealErrorView& err) noexcept {
    if (is_graceful_drain_request(err))
        return {HealAction::GracefulDrain, request_graceful_drain(err.code)};
    if (!is_quota_or_recoverable_panic(err))
        return {HealAction::None, false};
    constexpr std::uint64_t kDegradeThreshold = 1'000'000;
    if (is_quota_kind(err.kind) && err.code >= kDegradeThreshold)
        return {HealAction::Degrade, enter_degraded_mode()};
    g_self_heal_limited_mutate_total.fetch_add(1, std::memory_order_relaxed);
    return {HealAction::LimitedSelfMutate, true};
}

inline void register_self_healing_hook(SelfHealingHook h) {
    std::lock_guard lock(g_hooks_mu);
    g_hooks.push_back(std::move(h));
}

inline void set_self_heal_policy_fn(SelfHealPolicyFn fn) noexcept {
    g_policy_fn.store(fn, std::memory_order_release);
}
inline void clear_self_heal_policy() noexcept {
    g_policy_fn.store(nullptr, std::memory_order_release);
}

// #1582: policy engine closed loop.
inline bool run_self_heal_engine(const HealErrorView& err) {
    g_self_heal_policy_runs.fetch_add(1, std::memory_order_relaxed);
    g_triggers.fetch_add(1, std::memory_order_relaxed);

    SelfHealPolicyFn pfn = g_policy_fn.load(std::memory_order_acquire);
    PolicyResult policy_res = pfn ? pfn(err) : default_self_heal_policy(err);

    std::vector<SelfHealingHook> copy;
    {
        std::lock_guard lock(g_hooks_mu);
        copy = g_hooks;
    }
    bool hooks_ok = false;
    for (auto& hook : copy) {
        bool ok = false;
        try {
            ok = hook(err);
        } catch (...) {
            ok = false;
        }
        if (ok) {
            g_hook_ok.fetch_add(1, std::memory_order_relaxed);
            hooks_ok = true;
        } else {
            g_hook_fail.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const bool healed = policy_res.healed || hooks_ok;
    if (healed)
        g_self_heal_success_total.fetch_add(1, std::memory_order_relaxed);
    return healed;
}

// Phase 1 entry — routes through the engine (#1582).
inline void trigger_self_healing(const HealErrorView& err) {
    (void)run_self_heal_engine(err);
}

[[nodiscard]] inline std::uint64_t self_heal_success_total() noexcept {
    return g_self_heal_success_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t self_heal_degrade_total() noexcept {
    return g_self_heal_degrade_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t self_heal_graceful_drain_total() noexcept {
    return g_self_heal_graceful_drain_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t self_heal_policy_runs() noexcept {
    return g_self_heal_policy_runs.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t self_heal_triggers() noexcept {
    return g_triggers.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t self_heal_limited_mutate_total() noexcept {
    return g_self_heal_limited_mutate_total.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t graceful_drain_requests() noexcept {
    return g_graceful_drain_requests.load(std::memory_order_relaxed);
}
[[nodiscard]] inline std::uint64_t graceful_drain_completed() noexcept {
    return g_graceful_drain_completed.load(std::memory_order_relaxed);
}

inline void reset_self_heal_state_for_test() {
    std::lock_guard lock(g_hooks_mu);
    g_hooks.clear();
    g_policy_fn.store(nullptr, std::memory_order_relaxed);
    g_triggers.store(0, std::memory_order_relaxed);
    g_hook_ok.store(0, std::memory_order_relaxed);
    g_hook_fail.store(0, std::memory_order_relaxed);
    g_self_heal_success_total.store(0, std::memory_order_relaxed);
    g_self_heal_degrade_total.store(0, std::memory_order_relaxed);
    g_self_heal_graceful_drain_total.store(0, std::memory_order_relaxed);
    g_self_heal_policy_runs.store(0, std::memory_order_relaxed);
    g_self_heal_limited_mutate_total.store(0, std::memory_order_relaxed);
    g_graceful_drain_active.store(false, std::memory_order_relaxed);
    g_graceful_drain_requests.store(0, std::memory_order_relaxed);
    g_graceful_drain_completed.store(0, std::memory_order_relaxed);
    g_graceful_drain_last_code.store(0, std::memory_order_relaxed);
    g_degraded_mode.store(false, std::memory_order_relaxed);
    g_degrade_enters.store(0, std::memory_order_relaxed);
}

} // namespace aura::core::self_heal

#endif // AURA_CORE_SELF_HEALING_HOOKS_H
