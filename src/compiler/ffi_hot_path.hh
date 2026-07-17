// ffi_hot_path.hh — Issues #1177 / #1560: FFI batch hot-path dispatch (header form).
// Keep in sync with ffi_hot_path.ixx for module consumers.

#ifndef AURA_COMPILER_FFI_HOT_PATH_HH
#define AURA_COMPILER_FFI_HOT_PATH_HH

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace aura::compiler::ffi_hot {

inline constexpr int kFfiHotPathPhase = 2; // #1560: real batch dispatch
inline constexpr int kFfiHotPathIssue = 1560;

// Canonical render-backend ABI for hot-path batch call:
//   ret = fn(args, argc)
using BatchRenderFn = std::int64_t (*)(const std::int64_t* args, std::size_t argc);
// Nullary C backends (void / int return ignored → 0).
using NullaryFn = void (*)();

struct FFIBatchHotPathStats {
    std::atomic<std::uint64_t> hit_total{0};
    std::atomic<std::uint64_t> miss_total{0};
    std::atomic<std::uint64_t> batch_dispatch_total{0};
    std::atomic<std::uint64_t> invoke_total{0};
    std::atomic<std::uint64_t> invoke_skip_total{0}; // resolved but ABI not invocable
};

inline FFIBatchHotPathStats& g_ffi_hot_path_stats() noexcept {
    static FFIBatchHotPathStats s;
    return s;
}

// FNV-1a 64 — stable for binding name / signature hashing.
[[nodiscard]] inline std::uint64_t ffi_sig_hash(std::string_view name,
                                                std::string_view signature = {}) noexcept {
    std::uint64_t h = 14695981039346656037ull;
    auto mix = [&](unsigned char c) {
        h ^= c;
        h *= 1099511628211ull;
    };
    for (char c : name)
        mix(static_cast<unsigned char>(c));
    mix(0);
    for (char c : signature)
        mix(static_cast<unsigned char>(c));
    return h;
}

// Detect ABI from Agent-facing signature string.
//   "batch" / "(I64*)" / "Batch" → BatchArgs
//   "()" / empty / "Nullary" / "-> Void" with no args → Nullary
//   else → MetricsOnly (resolve + counters, no call)
enum class RenderFfiAbi : std::uint8_t { MetricsOnly = 0, Nullary = 1, BatchArgs = 2 };

[[nodiscard]] inline RenderFfiAbi abi_from_signature(std::string_view sig) noexcept {
    if (sig.empty())
        return RenderFfiAbi::Nullary;
    // Explicit batch markers
    if (sig.find("batch") != std::string_view::npos ||
        sig.find("Batch") != std::string_view::npos || sig.find("I64*") != std::string_view::npos ||
        sig.find("int64*") != std::string_view::npos)
        return RenderFfiAbi::BatchArgs;
    // Nullary: () -> ...
    auto arrow = sig.find("->");
    std::string_view args = arrow == std::string_view::npos ? sig : sig.substr(0, arrow);
    // Trim spaces roughly
    while (!args.empty() && (args.front() == ' ' || args.front() == '('))
        args.remove_prefix(1);
    while (!args.empty() && (args.back() == ' ' || args.back() == ')'))
        args.remove_suffix(1);
    if (args.empty() || args == "Void" || args == "void")
        return RenderFfiAbi::Nullary;
    if (sig.find("Nullary") != std::string_view::npos)
        return RenderFfiAbi::Nullary;
    return RenderFfiAbi::MetricsOnly;
}

// Cached-signature fast path with real batch dispatch (#1560).
// Thread-safe: hot path is lock-free read of atomics; miss path takes mutex.
struct FFIBatchHotPath {
    std::atomic<std::uint64_t> cached_sig_hash{0};
    std::atomic<void*> cached_func_ptr{nullptr};
    std::atomic<std::uint8_t> cached_abi{static_cast<std::uint8_t>(RenderFfiAbi::MetricsOnly)};
    std::mutex miss_mtx;

    [[nodiscard]] bool cached_sig_match(std::uint64_t sig_hash) const noexcept {
        const auto ptr = cached_func_ptr.load(std::memory_order_acquire);
        const auto h = cached_sig_hash.load(std::memory_order_acquire);
        return ptr != nullptr && h == sig_hash;
    }

    void record_hit() noexcept {
        g_ffi_hot_path_stats().hit_total.fetch_add(1, std::memory_order_relaxed);
    }
    void record_miss() noexcept {
        g_ffi_hot_path_stats().miss_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Update cache (miss path). Holds miss_mtx.
    void update_cache(std::uint64_t sig_hash, void* fn, RenderFfiAbi abi) noexcept {
        std::lock_guard<std::mutex> lock(miss_mtx);
        cached_sig_hash.store(sig_hash, std::memory_order_release);
        cached_func_ptr.store(fn, std::memory_order_release);
        cached_abi.store(static_cast<std::uint8_t>(abi), std::memory_order_release);
    }

    // Invoke according to ABI. Returns 0 for void/nullary, function ret for batch,
    // -1 if fn null or metrics-only.
    [[nodiscard]] static std::int64_t invoke(void* fn, RenderFfiAbi abi,
                                             std::span<const std::int64_t> args) noexcept {
        if (!fn) {
            g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        switch (abi) {
            case RenderFfiAbi::BatchArgs: {
                auto* f = reinterpret_cast<BatchRenderFn>(fn);
                g_ffi_hot_path_stats().invoke_total.fetch_add(1, std::memory_order_relaxed);
                return f(args.data(), args.size());
            }
            case RenderFfiAbi::Nullary: {
                auto* f = reinterpret_cast<NullaryFn>(fn);
                g_ffi_hot_path_stats().invoke_total.fetch_add(1, std::memory_order_relaxed);
                f();
                return 0;
            }
            case RenderFfiAbi::MetricsOnly:
            default:
                g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
                return 0;
        }
    }

    // Core dispatch: check likely(cached_sig_match) → direct call; else slow path.
    // `resolved_fn` / `abi` come from the slow-path resolver on miss (or known on first call).
    // On hit, uses cached ptr/abi (ignores resolved_fn unless null cache).
    [[nodiscard]] std::int64_t dispatch_batch(std::uint64_t sig_hash, void* resolved_fn,
                                              RenderFfiAbi abi,
                                              std::span<const std::int64_t> args) noexcept {
        g_ffi_hot_path_stats().batch_dispatch_total.fetch_add(1, std::memory_order_relaxed);

        const auto cached_h = cached_sig_hash.load(std::memory_order_acquire);
        const auto cached_fn = cached_func_ptr.load(std::memory_order_acquire);
        if (cached_fn != nullptr && cached_h == sig_hash) {
            record_hit();
            const auto cabi = static_cast<RenderFfiAbi>(cached_abi.load(std::memory_order_acquire));
            return invoke(cached_fn, cabi, args);
        }

        // Slow path: parse/resolve already done by caller; update cache + call.
        record_miss();
        if (resolved_fn)
            update_cache(sig_hash, resolved_fn, abi);
        return invoke(resolved_fn, abi, args);
    }

    // Convenience: hash name+sig, dispatch.
    [[nodiscard]] std::int64_t dispatch_named(std::string_view name, std::string_view signature,
                                              void* resolved_fn,
                                              std::span<const std::int64_t> args) noexcept {
        const auto h = ffi_sig_hash(name, signature);
        const auto abi = abi_from_signature(signature);
        return dispatch_batch(h, resolved_fn, abi, args);
    }

    void clear_cache() noexcept {
        std::lock_guard<std::mutex> lock(miss_mtx);
        cached_sig_hash.store(0, std::memory_order_release);
        cached_func_ptr.store(nullptr, std::memory_order_release);
        cached_abi.store(static_cast<std::uint8_t>(RenderFfiAbi::MetricsOnly),
                         std::memory_order_release);
    }
};

[[nodiscard]] inline FFIBatchHotPath& global_ffi_batch_hot_path() noexcept {
    static FFIBatchHotPath path;
    return path;
}

// Snapshot for query hooks (non-atomic copy).
struct FFIBatchHotPathSnapshot {
    std::uint64_t hit_total = 0;
    std::uint64_t miss_total = 0;
    std::uint64_t batch_dispatch_total = 0;
    std::uint64_t invoke_total = 0;
    std::uint64_t invoke_skip_total = 0;
    int phase = kFfiHotPathPhase;
};

[[nodiscard]] inline FFIBatchHotPathSnapshot snapshot_ffi_hot_path() noexcept {
    auto& s = g_ffi_hot_path_stats();
    return FFIBatchHotPathSnapshot{
        s.hit_total.load(std::memory_order_relaxed),
        s.miss_total.load(std::memory_order_relaxed),
        s.batch_dispatch_total.load(std::memory_order_relaxed),
        s.invoke_total.load(std::memory_order_relaxed),
        s.invoke_skip_total.load(std::memory_order_relaxed),
        kFfiHotPathPhase,
    };
}

inline void reset_ffi_hot_path_for_test() noexcept {
    auto& s = g_ffi_hot_path_stats();
    s.hit_total.store(0, std::memory_order_relaxed);
    s.miss_total.store(0, std::memory_order_relaxed);
    s.batch_dispatch_total.store(0, std::memory_order_relaxed);
    s.invoke_total.store(0, std::memory_order_relaxed);
    s.invoke_skip_total.store(0, std::memory_order_relaxed);
    global_ffi_batch_hot_path().clear_cache();
}

} // namespace aura::compiler::ffi_hot

#endif // AURA_COMPILER_FFI_HOT_PATH_HH
