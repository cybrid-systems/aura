// terminal_buffer_registry.hh — Issue #1352: process-wide TermBuf lifecycle + locks.
// Included from global module fragments (header-only) so ~Evaluator can clear.

#ifndef AURA_COMPILER_TERMINAL_BUFFER_REGISTRY_HH
#define AURA_COMPILER_TERMINAL_BUFFER_REGISTRY_HH

#include "renderer/batch_terminal.hh"
#include "renderer/render_pass.hh"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace aura::compiler::primitives_detail {
namespace term_registry {

    using TermCell = aura::renderer::TermCell;

    struct TermBuf {
        std::int32_t w = 0;
        std::int32_t h = 0;
        std::vector<TermCell> cells;
        // Issue #1559: per-buffer dirty region for present_batch short-circuit.
        aura::renderer::DirtyRegion dirty{};
        mutable std::shared_mutex rwlock;
    };

    inline std::shared_mutex s_term_registry_mtx;
    inline std::vector<std::unique_ptr<TermBuf>> s_term_bufs;
    // Freelist of tombstone slots — only filled by compact-terminal-buffers (#1352).
    inline std::vector<std::size_t> s_term_free_ids;
    // Multi-Evaluator (concurrent tests) share the registry; last release clears.
    inline std::atomic<int> s_term_registry_owners{0};

} // namespace term_registry

inline void retain_terminal_buffer_registry() noexcept {
    term_registry::s_term_registry_owners.fetch_add(1, std::memory_order_relaxed);
}

inline void clear_terminal_buffer_registry() noexcept {
    std::unique_lock<std::shared_mutex> lock(term_registry::s_term_registry_mtx);
    term_registry::s_term_bufs.clear();
    term_registry::s_term_free_ids.clear();
}

// Called from ~Evaluator: clear only when no other Evaluator is alive.
inline void release_terminal_buffer_registry() noexcept {
    const int prev = term_registry::s_term_registry_owners.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 1) {
        term_registry::s_term_registry_owners.store(0, std::memory_order_relaxed);
        clear_terminal_buffer_registry();
    }
}

} // namespace aura::compiler::primitives_detail

#endif // AURA_COMPILER_TERMINAL_BUFFER_REGISTRY_HH
