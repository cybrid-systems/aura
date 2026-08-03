// Issue #2626: CellGrid/TUI types removed; ABI kept with opaque pointers.
// ffi_hot_path.hh — Issues #1177 / #1560 / #2216: FFI batch hot-path dispatch.
// Keep in sync with ffi_hot_path.ixx for module consumers.
//
// Issue #2216: CellGrid ABI — zero-copy TermCell* + DirtyRegion* handoff to
// native present backends (no i64 pack/unpack).

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

inline constexpr int kFfiHotPathPhase = 3; // #2216: CellGrid ABI (was 2 for #1560)
inline constexpr int kFfiHotPathIssue = 2216;

// Canonical render-backend ABI for hot-path batch call:
//   ret = fn(args, argc)
using BatchRenderFn = std::int64_t (*)(const std::int64_t* args, std::size_t argc);
// Nullary C backends (void / int return ignored → 0).
using NullaryFn = void (*)();
// Issue #2216: typed cell-grid present (zero-copy from LinearCellGrid / TermBuf).
// dirty_or_null: nullptr means full frame (backend may treat as all dirty).
using CellGridPresentFn = std::int64_t (*)(const void* cells, std::int32_t w, std::int32_t h,
                                           const void* dirty_or_null);

struct FFIBatchHotPathStats {
    std::atomic<std::uint64_t> hit_total{0};
    std::atomic<std::uint64_t> miss_total{0};
    std::atomic<std::uint64_t> batch_dispatch_total{0};
    std::atomic<std::uint64_t> invoke_total{0};
    std::atomic<std::uint64_t> invoke_skip_total{0}; // resolved but ABI not invocable
    // Issue #2136: Render effect gate denials on FFI batch hand-off.
    std::atomic<std::uint64_t> effect_denied_render_total{0};
    std::atomic<std::uint64_t> effect_granted_render_total{0};
    // Issue #2216: CellGrid typed present invokes.
    std::atomic<std::uint64_t> cellgrid_invoke_total{0};
    std::atomic<std::uint64_t> cellgrid_dispatch_total{0};
    // Issue #2474: reader observed hash change mid-hit (double-check fail).
    // Zero in steady state; non-zero under concurrent update_cache storms.
    std::atomic<std::uint64_t> ffi_hot_path_cache_update_race_total{0};
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
//   "cellgrid" / "TermCell*" / "DirtyRegion" → CellGrid (#2216; checked first)
//   "batch" / "(I64*)" / "Batch" → BatchArgs
//   "()" / empty / "Nullary" / "-> Void" with no args → Nullary
//   else → MetricsOnly (resolve + counters, no call)
enum class RenderFfiAbi : std::uint8_t {
    MetricsOnly = 0,
    Nullary = 1,
    BatchArgs = 2,
    CellGrid = 3, // Issue #2216
};

[[nodiscard]] inline RenderFfiAbi abi_from_signature(std::string_view sig) noexcept {
    if (sig.empty())
        return RenderFfiAbi::Nullary;
    // Issue #2216: CellGrid markers (before batch — "batch" may appear in docs).
    if (sig.find("cellgrid") != std::string_view::npos ||
        sig.find("CellGrid") != std::string_view::npos ||
        sig.find("TermCell*") != std::string_view::npos ||
        sig.find("TermCell *") != std::string_view::npos ||
        sig.find("DirtyRegion") != std::string_view::npos)
        return RenderFfiAbi::CellGrid;
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
    //
    // Issue #2474: never publish sig_hash before fn/abi. Readers hit on
    // `fn != nullptr && h == sig_hash`; if hash is stored first they can
    // pair the *new* hash with the *old* fn+abi (wrong invoke / type
    // confusion). Protocol:
    //   1) invalidate hash (0) so concurrent readers miss during the write
    //   2) store abi + fn
    //   3) store sig_hash LAST (publish token; acquire on hash sees fn/abi)
    void update_cache(std::uint64_t sig_hash, void* fn, RenderFfiAbi abi) noexcept {
        // Issue #2474: publish protocol (hash last).
        std::lock_guard<std::mutex> lock(miss_mtx);
        cached_sig_hash.store(0, std::memory_order_release); // invalidate first
        cached_abi.store(static_cast<std::uint8_t>(abi), std::memory_order_release);
        cached_func_ptr.store(fn, std::memory_order_release);       // fn before hash
        cached_sig_hash.store(sig_hash, std::memory_order_release); // hash LAST
    }

    // Invoke according to ABI. Returns 0 for void/nullary, function ret for batch,
    // -1 if fn null or metrics-only. CellGrid must use invoke_cellgrid (typed args).
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
            case RenderFfiAbi::CellGrid:
                // Typed path requires cells pointer — not available via i64 span.
                g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
                return -1;
            case RenderFfiAbi::MetricsOnly:
            default:
                g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
                return 0;
        }
    }

    // Issue #2216: invoke CellGridPresentFn with live grid pointers.
    [[nodiscard]] static std::int64_t invoke_cellgrid(void* fn, const void* cells, std::int32_t w,
                                                      std::int32_t h,
                                                      const void* dirty_or_null) noexcept {
        if (!fn || !cells || w <= 0 || h <= 0) {
            g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        auto* f = reinterpret_cast<CellGridPresentFn>(fn);
        g_ffi_hot_path_stats().invoke_total.fetch_add(1, std::memory_order_relaxed);
        g_ffi_hot_path_stats().cellgrid_invoke_total.fetch_add(1, std::memory_order_relaxed);
        return f(cells, w, h, dirty_or_null);
    }

    // Core dispatch: check likely(cached_sig_match) → direct call; else slow path.
    // `resolved_fn` / `abi` come from the slow-path resolver on miss (or known on first call).
    // On hit, uses cached ptr/abi (ignores resolved_fn unless null cache).
    //
    // Issue #2136: `render_effect_ok` must be true (caller ran require_effect(Render)
    // or sandbox Off). When false, no side-effect invoke occurs; counters bump.
    [[nodiscard]] std::int64_t dispatch_batch(std::uint64_t sig_hash, void* resolved_fn,
                                              RenderFfiAbi abi, std::span<const std::int64_t> args,
                                              bool render_effect_ok = true) noexcept {
        g_ffi_hot_path_stats().batch_dispatch_total.fetch_add(1, std::memory_order_relaxed);
        if (!render_effect_ok) {
            g_ffi_hot_path_stats().effect_denied_render_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        g_ffi_hot_path_stats().effect_granted_render_total.fetch_add(1, std::memory_order_relaxed);

        // Issue #2474: load hash first; re-check after fn/abi so a concurrent
        // update_cache cannot leave us with a mismatched triple.
        const auto cached_h = cached_sig_hash.load(std::memory_order_acquire);
        const auto cached_fn = cached_func_ptr.load(std::memory_order_acquire);
        if (cached_fn != nullptr && cached_h == sig_hash) {
            const auto cabi = static_cast<RenderFfiAbi>(cached_abi.load(std::memory_order_acquire));
            const auto h2 = cached_sig_hash.load(std::memory_order_acquire);
            if (h2 != cached_h) {
                g_ffi_hot_path_stats().ffi_hot_path_cache_update_race_total.fetch_add(
                    1, std::memory_order_relaxed);
                // Fall through to miss path (safe re-resolve).
            } else {
                record_hit();
                return invoke(cached_fn, cabi, args);
            }
        }

        // Slow path: parse/resolve already done by caller; update cache + call.
        record_miss();
        if (resolved_fn)
            update_cache(sig_hash, resolved_fn, abi);
        return invoke(resolved_fn, abi, args);
    }

    // Issue #2216: CellGrid dispatch (lock-free hit path; miss updates cache).
    // Prefer over ANSI build when a native CellGrid backend is registered.
    [[nodiscard]] std::int64_t dispatch_cellgrid(std::uint64_t sig_hash, void* resolved_fn,
                                                 const void* cells, std::int32_t w, std::int32_t h,
                                                 const void* dirty_or_null,
                                                 bool render_effect_ok = true) noexcept {
        g_ffi_hot_path_stats().batch_dispatch_total.fetch_add(1, std::memory_order_relaxed);
        g_ffi_hot_path_stats().cellgrid_dispatch_total.fetch_add(1, std::memory_order_relaxed);
        if (!render_effect_ok) {
            g_ffi_hot_path_stats().effect_denied_render_total.fetch_add(1,
                                                                        std::memory_order_relaxed);
            g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
        g_ffi_hot_path_stats().effect_granted_render_total.fetch_add(1, std::memory_order_relaxed);

        // Issue #2474: same hash-last publish + double-check as dispatch_batch.
        const auto cached_h = cached_sig_hash.load(std::memory_order_acquire);
        const auto cached_fn = cached_func_ptr.load(std::memory_order_acquire);
        if (cached_fn != nullptr && cached_h == sig_hash) {
            const auto cabi = static_cast<RenderFfiAbi>(cached_abi.load(std::memory_order_acquire));
            const auto h2 = cached_sig_hash.load(std::memory_order_acquire);
            if (h2 != cached_h) {
                g_ffi_hot_path_stats().ffi_hot_path_cache_update_race_total.fetch_add(
                    1, std::memory_order_relaxed);
            } else if (cabi != RenderFfiAbi::CellGrid) {
                g_ffi_hot_path_stats().invoke_skip_total.fetch_add(1, std::memory_order_relaxed);
                return -1;
            } else {
                record_hit();
                return invoke_cellgrid(cached_fn, cells, w, h, dirty_or_null);
            }
        }
        record_miss();
        if (resolved_fn)
            update_cache(sig_hash, resolved_fn, RenderFfiAbi::CellGrid);
        return invoke_cellgrid(resolved_fn, cells, w, h, dirty_or_null);
    }

    // Convenience: hash name+sig, dispatch.
    [[nodiscard]] std::int64_t dispatch_named(std::string_view name, std::string_view signature,
                                              void* resolved_fn, std::span<const std::int64_t> args,
                                              bool render_effect_ok = true) noexcept {
        const auto h = ffi_sig_hash(name, signature);
        const auto abi = abi_from_signature(signature);
        return dispatch_batch(h, resolved_fn, abi, args, render_effect_ok);
    }

    void clear_cache() noexcept {
        std::lock_guard<std::mutex> lock(miss_mtx);
        // Issue #2474: invalidate hash first so lock-free readers miss
        // before fn is nulled (symmetric with update_cache publish order).
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

// Issue #2216: process-wide preferred CellGrid present backend (optional).
// present_batch / present-dirty consult this before ANSI path when non-null.
inline std::atomic<void*>& g_cellgrid_present_backend() noexcept {
    static std::atomic<void*> p{nullptr};
    return p;
}
inline void register_cellgrid_present_backend(CellGridPresentFn fn) noexcept {
    g_cellgrid_present_backend().store(reinterpret_cast<void*>(fn), std::memory_order_release);
}
inline void clear_cellgrid_present_backend() noexcept {
    g_cellgrid_present_backend().store(nullptr, std::memory_order_release);
}
[[nodiscard]] inline CellGridPresentFn cellgrid_present_backend() noexcept {
    return reinterpret_cast<CellGridPresentFn>(
        g_cellgrid_present_backend().load(std::memory_order_acquire));
}

// Canonical binding name + signature for Agent discovery / registry.
inline constexpr std::string_view kBindCellGridPresent = "c-present-cellgrid";
inline constexpr std::string_view kCellGridSignature =
    "cellgrid (TermCell*, i32, i32, DirtyRegion*) -> i64";

// Try CellGrid backend: returns true + *out_ret when a backend was invoked.
// Returns false when no backend is registered (caller should use ANSI path).
[[nodiscard]] inline bool try_cellgrid_present(const void* cells, std::int32_t w, std::int32_t h,
                                               void* dirty, std::int64_t* out_ret,
                                               bool render_effect_ok = true) noexcept {
    auto* fn = cellgrid_present_backend();
    if (!fn)
        return false;
    const auto sig_h = ffi_sig_hash(kBindCellGridPresent, kCellGridSignature);
    const auto ret = global_ffi_batch_hot_path().dispatch_cellgrid(
        sig_h, reinterpret_cast<void*>(fn), cells, w, h, dirty, render_effect_ok);
    if (out_ret)
        *out_ret = ret;
    return true;
}

// Snapshot for query hooks (non-atomic copy).
struct FFIBatchHotPathSnapshot {
    std::uint64_t hit_total = 0;
    std::uint64_t miss_total = 0;
    std::uint64_t batch_dispatch_total = 0;
    std::uint64_t invoke_total = 0;
    std::uint64_t invoke_skip_total = 0;
    std::uint64_t effect_denied_render_total = 0;  // #2136
    std::uint64_t effect_granted_render_total = 0; // #2136
    std::uint64_t cellgrid_invoke_total = 0;       // #2216
    std::uint64_t cellgrid_dispatch_total = 0;     // #2216
    std::uint64_t cache_update_race_total = 0;     // #2474
    int phase = kFfiHotPathPhase;
    int issue = kFfiHotPathIssue;
};

[[nodiscard]] inline FFIBatchHotPathSnapshot snapshot_ffi_hot_path() noexcept {
    auto& s = g_ffi_hot_path_stats();
    return FFIBatchHotPathSnapshot{
        s.hit_total.load(std::memory_order_relaxed),
        s.miss_total.load(std::memory_order_relaxed),
        s.batch_dispatch_total.load(std::memory_order_relaxed),
        s.invoke_total.load(std::memory_order_relaxed),
        s.invoke_skip_total.load(std::memory_order_relaxed),
        s.effect_denied_render_total.load(std::memory_order_relaxed),
        s.effect_granted_render_total.load(std::memory_order_relaxed),
        s.cellgrid_invoke_total.load(std::memory_order_relaxed),
        s.cellgrid_dispatch_total.load(std::memory_order_relaxed),
        s.ffi_hot_path_cache_update_race_total.load(std::memory_order_relaxed),
        kFfiHotPathPhase,
        kFfiHotPathIssue,
    };
}

inline void reset_ffi_hot_path_for_test() noexcept {
    auto& s = g_ffi_hot_path_stats();
    s.hit_total.store(0, std::memory_order_relaxed);
    s.miss_total.store(0, std::memory_order_relaxed);
    s.batch_dispatch_total.store(0, std::memory_order_relaxed);
    s.invoke_total.store(0, std::memory_order_relaxed);
    s.invoke_skip_total.store(0, std::memory_order_relaxed);
    s.effect_denied_render_total.store(0, std::memory_order_relaxed);
    s.effect_granted_render_total.store(0, std::memory_order_relaxed);
    s.cellgrid_invoke_total.store(0, std::memory_order_relaxed);
    s.cellgrid_dispatch_total.store(0, std::memory_order_relaxed);
    s.ffi_hot_path_cache_update_race_total.store(0, std::memory_order_relaxed);
    clear_cellgrid_present_backend();
    global_ffi_batch_hot_path().clear_cache();
}

} // namespace aura::compiler::ffi_hot

#endif // AURA_COMPILER_FFI_HOT_PATH_HH
