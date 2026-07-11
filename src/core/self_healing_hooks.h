// self_healing_hooks.h — Issue #1203 Phase 1: SelfHealingHook registration API.
// Plain header (not a module) so quota-check / panic paths can include it
// without module graph churn. Full AuraError payload peel follows.

#ifndef AURA_CORE_SELF_HEALING_HOOKS_H
#define AURA_CORE_SELF_HEALING_HOOKS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace aura::core::self_heal {

// Lightweight error view for Phase 1 (avoids importing AuraError module).
struct HealErrorView {
    std::string_view kind;
    std::string_view message;
    std::uint64_t code = 0;
};

using SelfHealingHook = std::function<bool(const HealErrorView&)>;

inline std::mutex g_hooks_mu;
inline std::vector<SelfHealingHook> g_hooks;
inline std::atomic<std::uint64_t> g_triggers{0};
inline std::atomic<std::uint64_t> g_hook_ok{0};
inline std::atomic<std::uint64_t> g_hook_fail{0};

inline void register_self_healing_hook(SelfHealingHook h) {
    std::lock_guard lock(g_hooks_mu);
    g_hooks.push_back(std::move(h));
}

inline void trigger_self_healing(const HealErrorView& err) {
    g_triggers.fetch_add(1, std::memory_order_relaxed);
    std::vector<SelfHealingHook> copy;
    {
        std::lock_guard lock(g_hooks_mu);
        copy = g_hooks;
    }
    for (auto& hook : copy) {
        bool ok = false;
        try {
            ok = hook(err);
        } catch (...) {
            ok = false;
        }
        if (ok)
            g_hook_ok.fetch_add(1, std::memory_order_relaxed);
        else
            g_hook_fail.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace aura::core::self_heal

#endif // AURA_CORE_SELF_HEALING_HOOKS_H
