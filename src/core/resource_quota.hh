// resource_quota.hh — Issue #1579: multi-dimension ResourceQuota enforcement.
// Header form for serve/scheduler + evaluator TUs + tests.
// Keep API aligned with resource_quota.ixx module surface.
//
// Note: does not include error.ixx (module). QuotaError is header-safe;
// convert to AuraError at module/evaluator boundary.

#ifndef AURA_CORE_RESOURCE_QUOTA_HH
#define AURA_CORE_RESOURCE_QUOTA_HH

#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace aura::core::resource_quota {

inline constexpr int kResourceQuotaPhase = 1; // #1579 dedicated module
inline constexpr int kResourceQuotaIssue = 1579;

// Matches AuraErrorKind::ResourceQuotaExceeded for Agent mapping.
// Do not renumber without updating error.ixx.
inline constexpr std::uint8_t kResourceQuotaExceededKind = 0; // filled at runtime via bridge

enum class Dimension : std::uint8_t {
    Memory = 0,
    Fibers = 1,
    TimeUs = 2,
    Mutations = 3,
    Count = 4
};

// Lightweight error (header-safe; convert to AuraError at module boundary).
struct QuotaError {
    Dimension dim = Dimension::Memory;
    std::string message;
    std::uint64_t requested = 0;
    std::uint64_t limit = 0;
    std::uint64_t used = 0;
};

// Multi-dimension quota with atomic limits + usage + overflow-safe math.
// limit == 0 means unlimited for that dimension.
struct ResourceQuota {
    // Limits (atomic stores for concurrent set + check; last writer wins).
    std::atomic<std::uint64_t> memory_limit{0};
    std::atomic<std::uint64_t> fibers_limit{0};
    std::atomic<std::uint64_t> time_us_limit{0};
    std::atomic<std::uint64_t> mutation_limit{0};

    // Usage (consumed amounts).
    std::atomic<std::uint64_t> memory_used{0};
    std::atomic<std::uint64_t> fibers_used{0};
    std::atomic<std::uint64_t> time_us_used{0};
    std::atomic<std::uint64_t> mutation_used{0};

    // Stats
    std::atomic<std::uint64_t> checks_total{0};
    std::atomic<std::uint64_t> rejects_total{0};
    std::atomic<std::uint64_t> consumes_total{0};
    std::atomic<std::uint64_t> releases_total{0};
    std::atomic<std::uint64_t> overflow_guards_total{0};
    std::atomic<std::uint64_t> fiber_reservations_active{0};

    void set_limit(Dimension d, std::uint64_t limit) noexcept {
        switch (d) {
            case Dimension::Memory:
                memory_limit.store(limit, std::memory_order_relaxed);
                break;
            case Dimension::Fibers:
                fibers_limit.store(limit, std::memory_order_relaxed);
                break;
            case Dimension::TimeUs:
                time_us_limit.store(limit, std::memory_order_relaxed);
                break;
            case Dimension::Mutations:
                mutation_limit.store(limit, std::memory_order_relaxed);
                break;
            default:
                break;
        }
    }

    [[nodiscard]] std::uint64_t limit(Dimension d) const noexcept {
        switch (d) {
            case Dimension::Memory:
                return memory_limit.load(std::memory_order_relaxed);
            case Dimension::Fibers:
                return fibers_limit.load(std::memory_order_relaxed);
            case Dimension::TimeUs:
                return time_us_limit.load(std::memory_order_relaxed);
            case Dimension::Mutations:
                return mutation_limit.load(std::memory_order_relaxed);
            default:
                return 0;
        }
    }

    [[nodiscard]] std::uint64_t used(Dimension d) const noexcept {
        switch (d) {
            case Dimension::Memory:
                return memory_used.load(std::memory_order_relaxed);
            case Dimension::Fibers:
                return fibers_used.load(std::memory_order_relaxed);
            case Dimension::TimeUs:
                return time_us_used.load(std::memory_order_relaxed);
            case Dimension::Mutations:
                return mutation_used.load(std::memory_order_relaxed);
            default:
                return 0;
        }
    }

    // Saturating add: returns false if a+b would overflow uint64.
    [[nodiscard]] static bool saturating_add(std::uint64_t a, std::uint64_t b,
                                             std::uint64_t& out) noexcept {
        if (b > std::numeric_limits<std::uint64_t>::max() - a)
            return false;
        out = a + b;
        return true;
    }

    // Check only (no consume). nullopt = OK.
    [[nodiscard]] std::optional<QuotaError> check(Dimension d, std::uint64_t amount) noexcept {
        checks_total.fetch_add(1, std::memory_order_relaxed);
        const auto lim = limit(d);
        if (lim == 0)
            return std::nullopt; // unlimited
        const auto u = used(d);
        std::uint64_t sum = 0;
        if (!saturating_add(u, amount, sum)) {
            overflow_guards_total.fetch_add(1, std::memory_order_relaxed);
            rejects_total.fetch_add(1, std::memory_order_relaxed);
            return QuotaError{d, "resource quota overflow guard", amount, lim, u};
        }
        if (sum > lim) {
            rejects_total.fetch_add(1, std::memory_order_relaxed);
            return QuotaError{d, dim_name(d) + std::string(" quota exceeded"), amount, lim, u};
        }
        return std::nullopt;
    }

    // Check + consume atomically (CAS loop). nullopt = OK and consumed.
    [[nodiscard]] std::optional<QuotaError> check_and_consume(Dimension d,
                                                              std::uint64_t amount) noexcept {
        checks_total.fetch_add(1, std::memory_order_relaxed);
        const auto lim = limit(d);
        if (lim == 0) {
            // Unlimited: still track usage for observability (saturating).
            auto& counter = used_ref(d);
            auto cur = counter.load(std::memory_order_relaxed);
            for (;;) {
                std::uint64_t next = 0;
                if (!saturating_add(cur, amount, next)) {
                    overflow_guards_total.fetch_add(1, std::memory_order_relaxed);
                    next = std::numeric_limits<std::uint64_t>::max();
                }
                if (counter.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed))
                    break;
            }
            consumes_total.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
        auto& counter = used_ref(d);
        auto cur = counter.load(std::memory_order_relaxed);
        for (;;) {
            std::uint64_t next = 0;
            if (!saturating_add(cur, amount, next)) {
                overflow_guards_total.fetch_add(1, std::memory_order_relaxed);
                rejects_total.fetch_add(1, std::memory_order_relaxed);
                return QuotaError{d, "resource quota overflow guard", amount, lim, cur};
            }
            if (next > lim) {
                rejects_total.fetch_add(1, std::memory_order_relaxed);
                return QuotaError{d, dim_name(d) + std::string(" quota exceeded"), amount, lim,
                                  cur};
            }
            if (counter.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                consumes_total.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }
        }
    }

    void release(Dimension d, std::uint64_t amount) noexcept {
        if (amount == 0)
            return;
        auto& counter = used_ref(d);
        auto cur = counter.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = cur >= amount ? cur - amount : 0;
            if (counter.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                releases_total.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    // ── Fiber RAII reservation ─────────────────────────────────
    struct FiberToken {
        ResourceQuota* quota = nullptr;
        FiberToken() = default;
        explicit FiberToken(ResourceQuota* q) noexcept
            : quota(q) {
            if (quota)
                quota->fiber_reservations_active.fetch_add(1, std::memory_order_relaxed);
        }
        ~FiberToken() { reset(); }
        FiberToken(const FiberToken&) = delete;
        FiberToken& operator=(const FiberToken&) = delete;
        FiberToken(FiberToken&& o) noexcept
            : quota(o.quota) {
            o.quota = nullptr;
        }
        FiberToken& operator=(FiberToken&& o) noexcept {
            if (this != &o) {
                reset();
                quota = o.quota;
                o.quota = nullptr;
            }
            return *this;
        }
        void reset() noexcept {
            if (quota) {
                quota->release(Dimension::Fibers, 1);
                quota->fiber_reservations_active.fetch_sub(1, std::memory_order_relaxed);
                quota = nullptr;
            }
        }
        [[nodiscard]] explicit operator bool() const noexcept { return quota != nullptr; }
        // Detach without releasing (transfer ownership to fiber lifecycle).
        ResourceQuota* release_ownership() noexcept {
            auto* q = quota;
            quota = nullptr;
            return q;
        }
    };

    // Reserve one fiber slot. On success, token releases on dtor unless moved.
    [[nodiscard]] std::optional<FiberToken> try_reserve_fiber() noexcept {
        if (auto err = check_and_consume(Dimension::Fibers, 1))
            return std::nullopt;
        return FiberToken{this};
    }

    // Same with error detail for AuraResult bridges.
    [[nodiscard]] std::pair<std::optional<FiberToken>, std::optional<QuotaError>>
    try_reserve_fiber_detailed() noexcept {
        if (auto err = check_and_consume(Dimension::Fibers, 1))
            return {std::nullopt, std::move(err)};
        return {FiberToken{this}, std::nullopt};
    }

    void reset_usage() noexcept {
        memory_used.store(0, std::memory_order_relaxed);
        fibers_used.store(0, std::memory_order_relaxed);
        time_us_used.store(0, std::memory_order_relaxed);
        mutation_used.store(0, std::memory_order_relaxed);
    }

    void reset_stats() noexcept {
        checks_total.store(0, std::memory_order_relaxed);
        rejects_total.store(0, std::memory_order_relaxed);
        consumes_total.store(0, std::memory_order_relaxed);
        releases_total.store(0, std::memory_order_relaxed);
        overflow_guards_total.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] static std::string dim_name(Dimension d) {
        switch (d) {
            case Dimension::Memory:
                return "memory";
            case Dimension::Fibers:
                return "fibers";
            case Dimension::TimeUs:
                return "time_us";
            case Dimension::Mutations:
                return "mutations";
            default:
                return "unknown";
        }
    }

private:
    [[nodiscard]] std::atomic<std::uint64_t>& used_ref(Dimension d) noexcept {
        switch (d) {
            case Dimension::Memory:
                return memory_used;
            case Dimension::Fibers:
                return fibers_used;
            case Dimension::TimeUs:
                return time_us_used;
            case Dimension::Mutations:
                return mutation_used;
            default:
                return memory_used;
        }
    }
};

// Process-wide quota for scheduler / fiber spawn isolation.
inline ResourceQuota& process_resource_quota() noexcept {
    static ResourceQuota q;
    return q;
}

// Test seam: reset process quota completely.
inline void reset_process_resource_quota_for_test() noexcept {
    auto& q = process_resource_quota();
    q.set_limit(Dimension::Memory, 0);
    q.set_limit(Dimension::Fibers, 0);
    q.set_limit(Dimension::TimeUs, 0);
    q.set_limit(Dimension::Mutations, 0);
    q.reset_usage();
    q.reset_stats();
    q.fiber_reservations_active.store(0, std::memory_order_relaxed);
}

} // namespace aura::core::resource_quota

#endif // AURA_CORE_RESOURCE_QUOTA_HH
