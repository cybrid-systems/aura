// ffi_hot_path.ixx — Issue #1177 Phase 1: FFI hot-path specialization scaffold.
// Full batch c-draw / c-present dispatch lands with render_ffi integration.

module;

export module aura.compiler.ffi_hot_path;

import std;

export namespace aura::compiler::ffi_hot {

// Phase 1 sentinel: module is linkable; metrics wired via CompilerMetrics.
inline constexpr int kFfiHotPathPhase = 1;

struct FFIBatchHotPathStats {
    std::uint64_t hit_total = 0;
    std::uint64_t miss_total = 0;
    std::uint64_t batch_dispatch_total = 0;
};

inline FFIBatchHotPathStats g_ffi_hot_path_stats{};

// Cached-signature fast path skeleton. Real function pointers land in #1182.
struct FFIBatchHotPath {
    std::uint64_t cached_sig_hash = 0;
    void* cached_func_ptr = nullptr;

    [[nodiscard]] bool cached_sig_match(std::uint64_t sig_hash) const noexcept {
        return cached_func_ptr != nullptr && cached_sig_hash == sig_hash;
    }

    void record_hit() noexcept { ++g_ffi_hot_path_stats.hit_total; }
    void record_miss() noexcept { ++g_ffi_hot_path_stats.miss_total; }
};

} // namespace aura::compiler::ffi_hot
