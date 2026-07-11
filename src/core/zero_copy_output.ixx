// zero_copy_output.ixx — Issue #1178 Phase 1: Arena-backed zero-copy views.
// Framebuffer SoA integration continues in #1186/#1184.

module;

export module aura.core.zero_copy_output;

import std;

export namespace aura::core::zero_copy {

inline constexpr int kZeroCopyOutputPhase = 1;

// Lightweight view holder. Phase 1 uses a process-local buffer pool
// (no pair allocations on the acquire path).
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
