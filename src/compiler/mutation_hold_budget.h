// mutation_hold_budget.h — Issue #2313: hold-budget threshold accessor.
// Shared header so MutationBoundaryGuard dtor + query:mutation-boundary-hold-stats
// share one env-cached source of truth (AURA_MUTATION_HOLD_BUDGET_US).

#ifndef AURA_COMPILER_MUTATION_HOLD_BUDGET_H
#define AURA_COMPILER_MUTATION_HOLD_BUDGET_H

#include <cstdint>
#include <cstdlib>

namespace aura::compiler {

// Default 100_000 µs (100 ms). Lazy-init from AURA_MUTATION_HOLD_BUDGET_US.
// C-style digit parse (no exceptions). Cached once per process.
[[nodiscard]] inline std::uint64_t mutation_hold_budget_us() noexcept {
    static const std::uint64_t cached = []() noexcept -> std::uint64_t {
        const char* e = std::getenv("AURA_MUTATION_HOLD_BUDGET_US");
        if (e == nullptr || e[0] == '\0')
            return 100000ULL;
        std::uint64_t v = 0;
        for (const char* p = e; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + static_cast<std::uint64_t>(*p - '0');
        }
        return v > 0 ? v : 100000ULL;
    }();
    return cached;
}

} // namespace aura::compiler

#endif // AURA_COMPILER_MUTATION_HOLD_BUDGET_H
