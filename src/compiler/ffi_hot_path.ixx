// ffi_hot_path.ixx — Issues #1177 / #1560: FFI hot-path specialization.
// Real batch dispatch: see ffi_hot_path.hh (header form used by TUs / tests).

module;

export module aura.compiler.ffi_hot_path;

import std;

export namespace aura::compiler::ffi_hot {

inline constexpr int kFfiHotPathPhase = 2; // #1560 real batch dispatch
inline constexpr int kFfiHotPathIssue = 1560;

using BatchRenderFn = std::int64_t (*)(const std::int64_t* args, std::size_t argc);
using NullaryFn = void (*)();

struct FFIBatchHotPathStats {
    std::uint64_t hit_total = 0;
    std::uint64_t miss_total = 0;
    std::uint64_t batch_dispatch_total = 0;
    std::uint64_t invoke_total = 0;
    std::uint64_t invoke_skip_total = 0;
};

// Module-local scaffold counters (process-wide atomics live in .hh).
inline FFIBatchHotPathStats g_ffi_hot_path_stats{};

enum class RenderFfiAbi : std::uint8_t { MetricsOnly = 0, Nullary = 1, BatchArgs = 2 };

struct FFIBatchHotPath {
    std::uint64_t cached_sig_hash = 0;
    void* cached_func_ptr = nullptr;
    RenderFfiAbi cached_abi = RenderFfiAbi::MetricsOnly;

    [[nodiscard]] bool cached_sig_match(std::uint64_t sig_hash) const noexcept {
        return cached_func_ptr != nullptr && cached_sig_hash == sig_hash;
    }

    void record_hit() noexcept { ++g_ffi_hot_path_stats.hit_total; }
    void record_miss() noexcept { ++g_ffi_hot_path_stats.miss_total; }

    // Skeleton parity: real lock-free dispatch is in ffi_hot_path.hh
    // (global_ffi_batch_hot_path().dispatch_batch).
    [[nodiscard]] std::int64_t dispatch_batch(std::uint64_t sig_hash, void* resolved_fn,
                                              RenderFfiAbi abi,
                                              std::span<const std::int64_t> args) noexcept {
        ++g_ffi_hot_path_stats.batch_dispatch_total;
        if (cached_sig_match(sig_hash)) {
            record_hit();
            if (!cached_func_ptr)
                return -1;
            if (cached_abi == RenderFfiAbi::BatchArgs) {
                ++g_ffi_hot_path_stats.invoke_total;
                return reinterpret_cast<BatchRenderFn>(cached_func_ptr)(args.data(), args.size());
            }
            if (cached_abi == RenderFfiAbi::Nullary) {
                ++g_ffi_hot_path_stats.invoke_total;
                reinterpret_cast<NullaryFn>(cached_func_ptr)();
                return 0;
            }
            ++g_ffi_hot_path_stats.invoke_skip_total;
            return 0;
        }
        record_miss();
        cached_sig_hash = sig_hash;
        cached_func_ptr = resolved_fn;
        cached_abi = abi;
        if (!resolved_fn)
            return -1;
        if (abi == RenderFfiAbi::BatchArgs) {
            ++g_ffi_hot_path_stats.invoke_total;
            return reinterpret_cast<BatchRenderFn>(resolved_fn)(args.data(), args.size());
        }
        if (abi == RenderFfiAbi::Nullary) {
            ++g_ffi_hot_path_stats.invoke_total;
            reinterpret_cast<NullaryFn>(resolved_fn)();
            return 0;
        }
        ++g_ffi_hot_path_stats.invoke_skip_total;
        return 0;
    }
};

} // namespace aura::compiler::ffi_hot
