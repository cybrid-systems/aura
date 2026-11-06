// offload/offload_quota.h — Issue #4885: ResourceQuota adapter for offload.
//
// One OffloadInflight slot is consumed per accepted task (queued + running)
// and released exactly once on the terminal notification. Execution time is
// charged to the existing TimeUs dimension in release().
#ifndef AURA_OFFLOAD_OFFLOAD_QUOTA_H
#define AURA_OFFLOAD_OFFLOAD_QUOTA_H

#include <cstdint>
#include <optional>

#include "core/resource_quota.hh"

namespace aura::offload {

class OffloadQuota {
public:
    using TenantId = core::resource_quota::TenantId;
    using QuotaError = core::resource_quota::QuotaError;

    explicit OffloadQuota(core::resource_quota::ResourceQuota* quota = nullptr) noexcept
        : quota_(quota) {}

    ~OffloadQuota() = default;
    OffloadQuota(const OffloadQuota&) = delete;
    OffloadQuota& operator=(const OffloadQuota&) = delete;
    OffloadQuota(OffloadQuota&&) = delete;
    OffloadQuota& operator=(OffloadQuota&&) = delete;

    [[nodiscard]] bool enabled() const noexcept { return quota_ != nullptr; }

    [[nodiscard]] core::resource_quota::ResourceQuota* quota() const noexcept { return quota_; }

    // Check + consume one inflight slot. nullopt = admitted.
    [[nodiscard]] std::optional<QuotaError> try_acquire(TenantId tenant) noexcept;

    // Release the inflight slot and charge execution time to TimeUs.
    // Call exactly once per accepted task, on every terminal status.
    void release(TenantId tenant, std::uint64_t exec_us) noexcept;

private:
    core::resource_quota::ResourceQuota* quota_ = nullptr;
};

} // namespace aura::offload

#endif // AURA_OFFLOAD_OFFLOAD_QUOTA_H
