// resource_quota.hh — Issue #1579: multi-dimension ResourceQuota enforcement.
// Header form for serve/scheduler + evaluator TUs + tests.
// Keep API aligned with resource_quota.ixx module surface.
//
// Note: does not include error.ixx (module). QuotaError is header-safe;
// convert to AuraError at module/evaluator boundary.

#ifndef AURA_CORE_RESOURCE_QUOTA_HH
#define AURA_CORE_RESOURCE_QUOTA_HH

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace aura::core::resource_quota {

inline constexpr int kResourceQuotaPhase = 1; // #1579 dedicated module
inline constexpr int kResourceQuotaIssue = 1579;
// Issue #3049: per-tenant ResourceQuota residual (process-global DoS).
inline constexpr int kQuotaPerTenantIssue = 3049;

using TenantId = std::uint64_t;

// Issue #3049: TLS admission tenant (0 = unkeyed / process-global).
// Scheduler / orch read this (or Fiber::assigned_tenant_id) at reserve.
// Tests set it explicitly. Soft / off never consults the map.
inline thread_local TenantId t_quota_tenant = 0;

[[nodiscard]] inline TenantId current_quota_tenant() noexcept {
    return t_quota_tenant;
}
inline void set_current_quota_tenant(TenantId t) noexcept {
    t_quota_tenant = t;
}

// -1 unknown / no override, 0 off, 1 on.
inline std::atomic<int> g_quota_per_tenant_cached{-1};
inline std::atomic<int> g_quota_per_tenant_test_override{-1};

inline void set_quota_per_tenant_enabled_for_test(bool on) noexcept {
    g_quota_per_tenant_test_override.store(on ? 1 : 0, std::memory_order_release);
}

inline void clear_quota_per_tenant_test_override() noexcept {
    g_quota_per_tenant_test_override.store(-1, std::memory_order_release);
    g_quota_per_tenant_cached.store(-1, std::memory_order_release);
}

[[nodiscard]] inline bool quota_per_tenant_enabled() noexcept {
    const int ov = g_quota_per_tenant_test_override.load(std::memory_order_acquire);
    if (ov >= 0)
        return ov != 0;
    int cached = g_quota_per_tenant_cached.load(std::memory_order_acquire);
    if (cached >= 0)
        return cached != 0;
    // AC3: Soft / AURA_SANDBOX=off never arms the map.
    const char* sandbox = std::getenv("AURA_SANDBOX");
    if (sandbox && sandbox[0] != '\0' && std::string_view(sandbox) == "off") {
        g_quota_per_tenant_cached.store(0, std::memory_order_release);
        return false;
    }
    auto env_on = [](const char* name) noexcept -> bool {
        const char* v = std::getenv(name);
        if (!v || !*v)
            return false;
        const std::string_view sv(v);
        return sv == "1" || sv == "true" || sv == "yes" || sv == "on";
    };
    // Explicit arm, or production multi-tenant profile (#3049 AC1).
    const bool on = env_on("AURA_QUOTA_PER_TENANT") || env_on("AURA_MULTI_TENANT");
    g_quota_per_tenant_cached.store(on ? 1 : 0, std::memory_order_release);
    return on;
}

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
    // Issue #1600: orchestration-layer rejection counters (spawn / parallel_intend).
    std::atomic<std::uint64_t> fiber_spawn_rejected_total{0};
    std::atomic<std::uint64_t> orchestration_quota_exceeded_total{0};
    // Issue #1880: deep orch ResourceQuota (memory/mailbox/arena) rejects +
    // live agent arena reservation bytes (for dashboards / backoff).
    std::atomic<std::uint64_t> orch_resource_quota_rejects_total{0};
    std::atomic<std::uint64_t> agent_arena_usage_bytes{0};
    std::atomic<std::uint64_t> agent_arena_reserve_total{0};
    std::atomic<std::uint64_t> agent_arena_release_total{0};
    // Issue #3049: per-tenant reject breakdown (additive; rejects_total stays).
    std::atomic<std::uint64_t> quota_reject_by_tenant_total{0};
    std::atomic<std::uint64_t> quota_last_reject_tenant{0};

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
    // Issue #3049: optional tenant key. tenant==0 or per-tenant off →
    // process-global path (AC3, zero map lookup).
    [[nodiscard]] std::optional<QuotaError> check(Dimension d, std::uint64_t amount,
                                                  TenantId tenant = 0) noexcept {
        if (tenant != 0 && quota_per_tenant_enabled())
            return check_tenant(d, amount, tenant);
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

    // Issue #1600: remaining capacity for a dimension (0 if unlimited).
    [[nodiscard]] std::uint64_t remaining(Dimension d) const noexcept {
        const auto lim = limit(d);
        if (lim == 0)
            return std::numeric_limits<std::uint64_t>::max();
        const auto u = used(d);
        return u >= lim ? 0 : (lim - u);
    }

    // Issue #1600: check whether `amount` additional fibers can be admitted
    // without consuming. nullopt = OK.
    [[nodiscard]] std::optional<QuotaError>
    check_orchestration_fibers(std::uint64_t amount, TenantId tenant = 0) noexcept {
        auto err = check(Dimension::Fibers, amount, tenant);
        if (err) {
            orchestration_quota_exceeded_total.fetch_add(1, std::memory_order_relaxed);
            if (tenant != 0 && quota_per_tenant_enabled() &&
                err->message.find("quota-exceeded:tenant=") == std::string::npos)
                err->message = tenant_deny_reason(tenant, Dimension::Fibers);
            else if (err->message.find("quota-exceeded:tenant=") == std::string::npos)
                err->message = "orchestration fiber quota exceeded";
        }
        return err;
    }

    // Check + consume atomically (CAS loop). nullopt = OK and consumed.
    // Issue #3049: tenant!=0 + per-tenant on → tenant budget then process
    // ceiling. tenant==0 or per-tenant off → existing process-global path.
    [[nodiscard]] std::optional<QuotaError> check_and_consume(Dimension d, std::uint64_t amount,
                                                              TenantId tenant = 0) noexcept {
        if (tenant != 0 && quota_per_tenant_enabled())
            return check_and_consume_tenant(d, amount, tenant);
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

    void release(Dimension d, std::uint64_t amount, TenantId tenant = 0) noexcept {
        if (amount == 0)
            return;
        if (tenant != 0 && quota_per_tenant_enabled())
            release_tenant(d, amount, tenant);
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
        TenantId tenant = 0; // #3049: release charges the same tenant
        FiberToken() = default;
        explicit FiberToken(ResourceQuota* q, TenantId t = 0) noexcept
            : quota(q)
            , tenant(t) {
            if (quota)
                quota->fiber_reservations_active.fetch_add(1, std::memory_order_relaxed);
        }
        ~FiberToken() { reset(); }
        FiberToken(const FiberToken&) = delete;
        FiberToken& operator=(const FiberToken&) = delete;
        FiberToken(FiberToken&& o) noexcept
            : quota(o.quota)
            , tenant(o.tenant) {
            o.quota = nullptr;
            o.tenant = 0;
        }
        FiberToken& operator=(FiberToken&& o) noexcept {
            if (this != &o) {
                reset();
                quota = o.quota;
                tenant = o.tenant;
                o.quota = nullptr;
                o.tenant = 0;
            }
            return *this;
        }
        void reset() noexcept {
            if (quota) {
                quota->release(Dimension::Fibers, 1, tenant);
                quota->fiber_reservations_active.fetch_sub(1, std::memory_order_relaxed);
                quota = nullptr;
                tenant = 0;
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
    [[nodiscard]] std::optional<FiberToken> try_reserve_fiber(TenantId tenant = 0) noexcept {
        if (auto err = check_and_consume(Dimension::Fibers, 1, tenant))
            return std::nullopt;
        return FiberToken{this, tenant};
    }

    // Same with error detail for AuraResult bridges.
    [[nodiscard]] std::pair<std::optional<FiberToken>, std::optional<QuotaError>>
    try_reserve_fiber_detailed(TenantId tenant = 0) noexcept {
        if (auto err = check_and_consume(Dimension::Fibers, 1, tenant))
            return {std::nullopt, std::move(err)};
        return {FiberToken{this, tenant}, std::nullopt};
    }

    void reset_usage() noexcept {
        memory_used.store(0, std::memory_order_relaxed);
        fibers_used.store(0, std::memory_order_relaxed);
        time_us_used.store(0, std::memory_order_relaxed);
        mutation_used.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(tenant_mtx_);
        for (auto& [id, slot] : tenant_slots_) {
            (void)id;
            if (!slot)
                continue;
            for (auto& u : slot->used)
                u.store(0, std::memory_order_relaxed);
        }
    }

    void reset_stats() noexcept {
        checks_total.store(0, std::memory_order_relaxed);
        rejects_total.store(0, std::memory_order_relaxed);
        consumes_total.store(0, std::memory_order_relaxed);
        releases_total.store(0, std::memory_order_relaxed);
        overflow_guards_total.store(0, std::memory_order_relaxed);
        fiber_spawn_rejected_total.store(0, std::memory_order_relaxed);
        orchestration_quota_exceeded_total.store(0, std::memory_order_relaxed);
        orch_resource_quota_rejects_total.store(0, std::memory_order_relaxed);
        agent_arena_usage_bytes.store(0, std::memory_order_relaxed);
        agent_arena_reserve_total.store(0, std::memory_order_relaxed);
        agent_arena_release_total.store(0, std::memory_order_relaxed);
        quota_reject_by_tenant_total.store(0, std::memory_order_relaxed);
        quota_last_reject_tenant.store(0, std::memory_order_relaxed);
    }

    // Issue #1880: reserve agent arena/mailbox memory for orchestration spawn.
    // nullopt = OK and usage bumped; on reject bumps orch_resource_quota_rejects.
    [[nodiscard]] std::optional<QuotaError> try_consume_agent_arena(std::uint64_t bytes,
                                                                    TenantId tenant = 0) noexcept {
        auto err = check_and_consume(Dimension::Memory, bytes, tenant);
        if (err) {
            orch_resource_quota_rejects_total.fetch_add(1, std::memory_order_relaxed);
            orchestration_quota_exceeded_total.fetch_add(1, std::memory_order_relaxed);
            err->message = "orchestration agent arena/mailbox quota exceeded";
            return err;
        }
        agent_arena_usage_bytes.fetch_add(bytes, std::memory_order_relaxed);
        agent_arena_reserve_total.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    void release_agent_arena(std::uint64_t bytes, TenantId tenant = 0) noexcept {
        if (bytes == 0)
            return;
        release(Dimension::Memory, bytes, tenant);
        // Saturating sub for live usage gauge.
        auto cur = agent_arena_usage_bytes.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = cur >= bytes ? cur - bytes : 0;
            if (agent_arena_usage_bytes.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                                              std::memory_order_relaxed))
                break;
        }
        agent_arena_release_total.fetch_add(1, std::memory_order_relaxed);
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

    // Issue #3049: per-tenant budget. 0 = inherit process limit for that dim.
    void set_tenant_limit(TenantId tenant, Dimension d, std::uint64_t lim) noexcept {
        if (tenant == 0)
            return;
        auto* slot = tenant_slot(tenant, /*create=*/true);
        if (!slot)
            return;
        const auto i = dim_index(d);
        if (i < kDimCount)
            slot->limit[i].store(lim, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t tenant_limit(TenantId tenant, Dimension d) const noexcept {
        if (tenant == 0)
            return limit(d);
        auto* slot = tenant_slot(tenant, /*create=*/false);
        if (!slot)
            return limit(d);
        const auto i = dim_index(d);
        const auto tlim = (i < kDimCount) ? slot->limit[i].load(std::memory_order_relaxed) : 0;
        return tlim != 0 ? tlim : limit(d);
    }

    [[nodiscard]] std::uint64_t tenant_used(TenantId tenant, Dimension d) const noexcept {
        if (tenant == 0)
            return used(d);
        auto* slot = tenant_slot(tenant, /*create=*/false);
        if (!slot)
            return 0;
        const auto i = dim_index(d);
        return i < kDimCount ? slot->used[i].load(std::memory_order_relaxed) : 0;
    }

    void clear_tenant_quotas() noexcept {
        std::lock_guard<std::mutex> lock(tenant_mtx_);
        tenant_slots_.clear();
    }

    [[nodiscard]] static std::string tenant_deny_reason(TenantId tenant, Dimension d) {
        return std::string("quota-exceeded:tenant=") + std::to_string(tenant) +
               ":dim=" + dim_name(d);
    }

private:
    static constexpr std::size_t kDimCount = 4;

    struct TenantSlot {
        std::array<std::atomic<std::uint64_t>, kDimCount> used{};
        std::array<std::atomic<std::uint64_t>, kDimCount> limit{};
        std::atomic<std::uint64_t> rejects{0};
    };

    mutable std::mutex tenant_mtx_;
    mutable std::unordered_map<TenantId, std::unique_ptr<TenantSlot>> tenant_slots_;

    [[nodiscard]] static std::size_t dim_index(Dimension d) noexcept {
        const auto i = static_cast<std::uint8_t>(d);
        return i < kDimCount ? i : 0;
    }

    [[nodiscard]] TenantSlot* tenant_slot(TenantId tenant, bool create) const noexcept {
        if (tenant == 0)
            return nullptr;
        std::lock_guard<std::mutex> lock(tenant_mtx_);
        auto it = tenant_slots_.find(tenant);
        if (it != tenant_slots_.end())
            return it->second.get();
        if (!create)
            return nullptr;
        auto slot = std::make_unique<TenantSlot>();
        auto* p = slot.get();
        tenant_slots_.emplace(tenant, std::move(slot));
        return p;
    }

    [[nodiscard]] std::optional<QuotaError> check_tenant(Dimension d, std::uint64_t amount,
                                                         TenantId tenant) noexcept {
        checks_total.fetch_add(1, std::memory_order_relaxed);
        auto* slot = tenant_slot(tenant, /*create=*/false);
        const auto tlim = tenant_limit(tenant, d);
        const auto tused = slot ? tenant_used(tenant, d) : 0;
        // Tenant budget first (AC2): A hitting its cap does not consume process.
        if (tlim != 0) {
            std::uint64_t sum = 0;
            if (!saturating_add(tused, amount, sum) || sum > tlim) {
                note_tenant_reject(tenant);
                if (!saturating_add(tused, amount, sum))
                    overflow_guards_total.fetch_add(1, std::memory_order_relaxed);
                return QuotaError{d, tenant_deny_reason(tenant, d), amount, tlim, tused};
            }
        }
        // Process ceiling still binds.
        const auto plim = limit(d);
        if (plim != 0) {
            const auto pu = used(d);
            std::uint64_t sum = 0;
            if (!saturating_add(pu, amount, sum) || sum > plim) {
                rejects_total.fetch_add(1, std::memory_order_relaxed);
                return QuotaError{d, dim_name(d) + std::string(" quota exceeded"), amount, plim,
                                  pu};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<QuotaError>
    check_and_consume_tenant(Dimension d, std::uint64_t amount, TenantId tenant) noexcept {
        checks_total.fetch_add(1, std::memory_order_relaxed);
        auto* slot = tenant_slot(tenant, /*create=*/true);
        if (!slot)
            return check_and_consume(d, amount, /*tenant=*/0);
        const auto i = dim_index(d);
        auto& tcounter = slot->used[i];
        const auto tlim_stored = slot->limit[i].load(std::memory_order_relaxed);
        const auto tlim = tlim_stored != 0 ? tlim_stored : limit(d);

        auto tcur = tcounter.load(std::memory_order_relaxed);
        for (;;) {
            std::uint64_t tnext = 0;
            if (!saturating_add(tcur, amount, tnext)) {
                overflow_guards_total.fetch_add(1, std::memory_order_relaxed);
                note_tenant_reject(tenant);
                return QuotaError{d, tenant_deny_reason(tenant, d), amount, tlim, tcur};
            }
            if (tlim != 0 && tnext > tlim) {
                note_tenant_reject(tenant);
                return QuotaError{d, tenant_deny_reason(tenant, d), amount, tlim, tcur};
            }
            if (tcounter.compare_exchange_weak(tcur, tnext, std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
                break;
        }

        // Process ceiling. Rollback tenant on reject.
        const auto plim = limit(d);
        auto& pcounter = used_ref(d);
        auto pcur = pcounter.load(std::memory_order_relaxed);
        for (;;) {
            std::uint64_t pnext = 0;
            if (!saturating_add(pcur, amount, pnext) || (plim != 0 && pnext > plim)) {
                // rollback tenant
                auto tb = tcounter.load(std::memory_order_relaxed);
                for (;;) {
                    const auto back = tb >= amount ? tb - amount : 0;
                    if (tcounter.compare_exchange_weak(tb, back, std::memory_order_acq_rel,
                                                       std::memory_order_relaxed))
                        break;
                }
                if (!saturating_add(pcur, amount, pnext))
                    overflow_guards_total.fetch_add(1, std::memory_order_relaxed);
                rejects_total.fetch_add(1, std::memory_order_relaxed);
                return QuotaError{d, dim_name(d) + std::string(" quota exceeded"), amount, plim,
                                  pcur};
            }
            if (pcounter.compare_exchange_weak(pcur, pnext, std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
                break;
        }
        consumes_total.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    void release_tenant(Dimension d, std::uint64_t amount, TenantId tenant) noexcept {
        auto* slot = tenant_slot(tenant, /*create=*/false);
        if (!slot)
            return;
        auto& tcounter = slot->used[dim_index(d)];
        auto cur = tcounter.load(std::memory_order_relaxed);
        for (;;) {
            const auto next = cur >= amount ? cur - amount : 0;
            if (tcounter.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
                return;
        }
    }

    void note_tenant_reject(TenantId tenant) noexcept {
        rejects_total.fetch_add(1, std::memory_order_relaxed);
        quota_reject_by_tenant_total.fetch_add(1, std::memory_order_relaxed);
        quota_last_reject_tenant.store(tenant, std::memory_order_relaxed);
        if (auto* slot = tenant_slot(tenant, /*create=*/false))
            slot->rejects.fetch_add(1, std::memory_order_relaxed);
    }
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

// Issue #1618: explicit ResourceQuota manager facade for production
// multi-fiber / self-mutating workloads. Surfaces typed rejects with
// optional mutation provenance (agents map to AuraErrorKind::ResourceQuotaExceeded).
// Does not throw; never routes through PanicCheckpoint.
struct ResourceQuotaManager {
    ResourceQuota* quota = nullptr;
    std::uint64_t provenance_mutation_id = 0;

    explicit ResourceQuotaManager(ResourceQuota* q = nullptr) noexcept
        : quota(q ? q : &process_resource_quota()) {}

    void set_provenance(std::uint64_t mutation_id) noexcept {
        provenance_mutation_id = mutation_id;
    }

    void set_limit(Dimension d, std::uint64_t lim) noexcept {
        if (quota)
            quota->set_limit(d, lim);
    }

    [[nodiscard]] std::uint64_t limit(Dimension d) const noexcept {
        return quota ? quota->limit(d) : 0;
    }
    [[nodiscard]] std::uint64_t used(Dimension d) const noexcept {
        return quota ? quota->used(d) : 0;
    }

    // Format machine-readable reason for AuraError messages / agents.
    [[nodiscard]] static std::string format_reason(const QuotaError& e,
                                                   std::uint64_t mutation_id = 0) {
        std::string msg = e.message.empty() ? (dim_name(e.dim) + " quota exceeded") : e.message;
        msg += " [dim=";
        msg += dim_name(e.dim);
        msg += " requested=";
        msg += std::to_string(e.requested);
        msg += " used=";
        msg += std::to_string(e.used);
        msg += " limit=";
        msg += std::to_string(e.limit);
        msg += "]";
        if (mutation_id != 0) {
            msg += " provenance_mutation_id=";
            msg += std::to_string(mutation_id);
        }
        return msg;
    }

    [[nodiscard]] static std::string dim_name(Dimension d) { return ResourceQuota::dim_name(d); }

    // Check + consume; nullopt = OK. On reject, message includes provenance.
    // Issue #3049: optional tenant key (0 = process-global).
    [[nodiscard]] std::optional<QuotaError> check_and_consume(Dimension d, std::uint64_t amount,
                                                              TenantId tenant = 0) noexcept {
        if (!quota)
            return std::nullopt;
        auto err = quota->check_and_consume(d, amount, tenant);
        if (err && provenance_mutation_id != 0) {
            err->message = format_reason(*err, provenance_mutation_id);
        } else if (err) {
            err->message = format_reason(*err, 0);
        }
        return err;
    }

    // Mutation budget convenience (Dimension::Mutations).
    [[nodiscard]] std::optional<QuotaError>
    check_and_consume_mutation(std::uint64_t pending = 1, TenantId tenant = 0) noexcept {
        return check_and_consume(Dimension::Mutations, pending, tenant);
    }

    // Fiber admission convenience.
    [[nodiscard]] std::optional<QuotaError> check_and_consume_fiber(TenantId tenant = 0) noexcept {
        return check_and_consume(Dimension::Fibers, 1, tenant);
    }

    // Memory admission convenience.
    [[nodiscard]] std::optional<QuotaError> check_and_consume_memory(std::uint64_t bytes,
                                                                     TenantId tenant = 0) noexcept {
        return check_and_consume(Dimension::Memory, bytes, tenant);
    }
};

// Process-level manager (Scheduler / Fiber spawn / orch).
inline ResourceQuotaManager& process_resource_quota_manager() noexcept {
    static ResourceQuotaManager mgr{&process_resource_quota()};
    return mgr;
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
    q.clear_tenant_quotas();
    q.fiber_reservations_active.store(0, std::memory_order_relaxed);
    process_resource_quota_manager().set_provenance(0);
    set_current_quota_tenant(0);
    clear_quota_per_tenant_test_override();
}

} // namespace aura::core::resource_quota

#endif // AURA_CORE_RESOURCE_QUOTA_HH
