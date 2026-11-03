// offload/offload_quota.cpp — Issue #4885: offload ResourceQuota adapter.
#include "offload_quota.h"

namespace aura::offload {

std::optional<OffloadQuota::QuotaError> OffloadQuota::try_acquire(TenantId tenant) noexcept {
    if (!quota_)
        return std::nullopt;
    auto err =
        quota_->check_and_consume(core::resource_quota::Dimension::OffloadInflight, 1, tenant);
    if (err && err->message.empty())
        err->message = "offload inflight quota exceeded";
    return err;
}

void OffloadQuota::release(TenantId tenant, std::uint64_t exec_us) noexcept {
    if (!quota_)
        return;
    quota_->release(core::resource_quota::Dimension::OffloadInflight, 1, tenant);
    if (exec_us != 0) {
        // TimeUs is cumulative usage, not a reservation: ignore a reject here
        // (the task already ran and the overrun is reported by quota stats).
        (void)quota_->check_and_consume(core::resource_quota::Dimension::TimeUs, exec_us, tenant);
    }
}

} // namespace aura::offload
