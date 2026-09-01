// atomic_fence_port.h — Issue #2931 (tsan-smoke nightly hard gate).
//
// GCC 16 / libstdc++ 16 rejects std::atomic_thread_fence outright under
// -fsanitize=thread (atomic_base.h:147 hard error via -Werror=tsan):
// ThreadSanitizer derives happens-before from atomics and locks only,
// so an explicit fence is a no-op for the tool and cannot be modeled.
//
// Every fence in the tree therefore goes through this port:
//   - real std::atomic_thread_fence on every non-TSan build (semantics
//     unchanged for production / asan / ubsan);
//   - compiled out under TSan — the surrounding release/acquire atomics
//     carry the ordering the sanitizer can actually see. Fence-based
//     orderings (Chase-Lev ws_deque, JIT remount proofs) stay validated
//     by the normal suites; under TSan the tool's atomic-only model is
//     authoritative by definition.
#pragma once

#include <atomic>

#if defined(__SANITIZE_THREAD__)
#define AURA_THREAD_SANITIZER 1
#elif defined(__clang__) && defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define AURA_THREAD_SANITIZER 1
#else
#define AURA_THREAD_SANITIZER 0
#endif
#else
#define AURA_THREAD_SANITIZER 0
#endif

namespace aura::util {

// Port of std::atomic_thread_fence (see header comment).
inline void thread_fence(std::memory_order order) noexcept {
#if AURA_THREAD_SANITIZER
    (void)order; // fences not modeled under TSan — atomics carry ordering.
#else
    std::atomic_thread_fence(order);
#endif
}

} // namespace aura::util
