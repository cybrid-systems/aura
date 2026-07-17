// zero_copy_output.hh — Issue #1178/#1559: Arena-backed zero-copy views (header form).
// Keep in sync with zero_copy_output.ixx for module consumers.

#ifndef AURA_CORE_ZERO_COPY_OUTPUT_HH
#define AURA_CORE_ZERO_COPY_OUTPUT_HH

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aura::core::zero_copy {

inline constexpr int kZeroCopyOutputPhase = 1;

// Lightweight view holder. Phase 1 uses a process-local buffer pool
// (no pair allocations on the acquire path after warm-up resize).
struct ZeroCopyFramebuffer {
    std::vector<std::byte> storage;
    std::uint64_t acquire_count = 0;
    std::uint64_t release_count = 0;

    std::span<std::byte> acquire_view(std::size_t size) {
        if (storage.size() < size)
            storage.resize(size);
        ++acquire_count;
        return {storage.data(), size};
    }

    void release_view(std::span<std::byte> /*v*/) noexcept { ++release_count; }
};

inline ZeroCopyFramebuffer g_zero_copy_fb{};

} // namespace aura::core::zero_copy

#endif // AURA_CORE_ZERO_COPY_OUTPUT_HH
