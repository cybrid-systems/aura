// zero_copy_output.ixx — Issues #1178/#1561: Arena-backed true zero-copy views.
// Full implementation lives in zero_copy_output.hh (header form for renderer TUs).
// This module exports the phase constant + lightweight scaffold for module importers.

module;

export module aura.core.zero_copy_output;

import std;

export namespace aura::core::zero_copy {

inline constexpr int kZeroCopyOutputPhase = 2; // #1561 Arena path
inline constexpr int kZeroCopyOutputIssue = 1561;

// Module-local scaffold (process-wide atomics + FrameBumpArena in .hh).
struct ZeroCopyFramebuffer {
    std::vector<std::byte> storage;
    std::uint64_t acquire_count = 0;
    std::uint64_t release_count = 0;
    std::uint64_t arena_alloc_bytes = 0;
    std::uint64_t hit_in_render = 0;

    std::span<std::byte> acquire_view(std::size_t size) {
        if (storage.size() < size)
            storage.resize(size);
        ++acquire_count;
        return {storage.data(), size};
    }

    // Arena-like: bump from storage tail (monotonic within this object).
    // Prefer zero_copy_output.hh FrameBumpArena / ASTArena template path in production.
    std::span<std::byte> acquire_view_arena(std::size_t size) {
        const std::size_t off = storage.size();
        storage.resize(off + size);
        ++acquire_count;
        arena_alloc_bytes += size;
        return {storage.data() + off, size};
    }

    void release_view(std::span<std::byte> /*v*/) noexcept { ++release_count; }
};

inline ZeroCopyFramebuffer g_zero_copy_fb{};

} // namespace aura::core::zero_copy
