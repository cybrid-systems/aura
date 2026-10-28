// ── sharded hot-path counter (#4717 P4 step 3) ────────────────────────────
// A hot-path counter that every evaluator fiber bumps is a single shared
// cache line: on aarch64 each bump is an exclusive ldadd on that word, so
// 16 bodies serialize on one line no matter how much real work they do.
// (Measured: 41 of 714 gdb stack samples landed in __aarch64_ldadd8 inside
// is_valid / eval_flat_body / as_int at n=16.)
//
// Sharding by calling thread keeps the bump on a thread-private line. load()
// still returns an EXACT total (it sums the shards), so every observability
// reader — (query:cpp26-contracts-stats), (engine:metrics), the arena
// dashboards and the unit tests — keeps its contract; only the cost changes.
//
// The member API deliberately mirrors std::atomic's spelling
// (fetch_add / load / store) so existing hot-path call sites and the
// text-based hot-path linters are unchanged.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aura::core {

class ShardedCounter {
public:
    static constexpr std::size_t kShards = 16;

    struct alignas(64) Shard {
        std::atomic<std::uint64_t> v{0};
    };

    ShardedCounter() noexcept = default;
    explicit ShardedCounter(std::uint64_t v) noexcept {
        shards_[0].v.store(v, std::memory_order_relaxed);
    }
    ShardedCounter(const ShardedCounter& o) noexcept
        : ShardedCounter(o.load()) {}
    ShardedCounter& operator=(const ShardedCounter& o) noexcept {
        store(o.load());
        return *this;
    }
    ShardedCounter(ShardedCounter&& o) noexcept
        : ShardedCounter(o.load()) {}
    ShardedCounter& operator=(ShardedCounter&& o) noexcept {
        if (this != &o)
            store(o.load());
        return *this;
    }

    void fetch_add(std::uint64_t n, std::memory_order = std::memory_order_relaxed) noexcept {
        shard_ref().v.fetch_add(n, std::memory_order_relaxed);
    }

    void store(std::uint64_t v, std::memory_order = std::memory_order_relaxed) noexcept {
        for (std::size_t i = 1; i < kShards; ++i)
            shards_[i].v.store(0, std::memory_order_relaxed);
        shards_[0].v.store(v, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t load(std::memory_order = std::memory_order_relaxed) const noexcept {
        std::uint64_t sum = 0;
        for (const auto& s : shards_)
            sum += s.v.load(std::memory_order_relaxed);
        return sum;
    }

private:
    static std::size_t claim_shard() noexcept {
        static std::atomic<std::uint64_t> seq{0};
        return static_cast<std::size_t>(seq.fetch_add(1, std::memory_order_relaxed) % kShards);
    }
    static std::size_t this_shard() noexcept {
        static thread_local std::size_t idx = claim_shard();
        return idx;
    }
    Shard& shard_ref() noexcept { return shards_[this_shard()]; }

    std::array<Shard, kShards> shards_{};
};

} // namespace aura::core
