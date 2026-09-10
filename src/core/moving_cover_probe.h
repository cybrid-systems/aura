// moving_cover_probe.h — Issue #3633: moved-vs-covered reconciliation
// probe for the Moving densify window.
//
// RootRemapPass records the OLD address of every slot it successfully
// rewrites (densify-key hit) into a thread_local scratch vector; the
// ASTArena Moving window drains it right after invoke_root_remap_callback_
// and reconciles covered-vs-moved counts (window-exit fail-close: a
// relocated object with NO cover in any rewrite family — external-root
// slot / LifetimePin remap / RootRemapPass — has an unregistered raw
// alias as its only possible referent, which used to dangle silently).
//
// thread_local + last-call semantics (#2376 pattern): cleared at pass
// start, drained by the same thread that ran live_compact(Moving). Not a
// registry — pure per-window scratch, no cross-window retention, no new
// pin/GC model (#3633 non-goals).

#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace aura::core::moving_cover_probe {

inline thread_local std::vector<void*> t_covered_old;

inline void clear() noexcept {
    t_covered_old.clear();
}

inline void record_covered_old(void* old_addr) noexcept {
    t_covered_old.push_back(old_addr);
}

[[nodiscard]] inline std::vector<void*> drain() noexcept {
    std::vector<void*> out;
    out.swap(t_covered_old);
    return out;
}

} // namespace aura::core::moving_cover_probe
