// resource_quota.ixx — Issue #1579: dedicated ResourceQuota module.
// Full atomic multi-dimension enforcement lives in resource_quota.hh;
// this module re-exports types for C++ module consumers (evaluator, tests).

module;
#include "core/resource_quota.hh"

export module aura.core.resource_quota;

import std;
import aura.core.error;

export namespace aura::core::resource_quota {

using ::aura::core::resource_quota::Dimension;
using ::aura::core::resource_quota::QuotaError;
using ::aura::core::resource_quota::ResourceQuota;
using FiberToken = ResourceQuota::FiberToken;
using ::aura::core::resource_quota::kResourceQuotaIssue;
using ::aura::core::resource_quota::kResourceQuotaPhase;
using ::aura::core::resource_quota::process_resource_quota;
using ::aura::core::resource_quota::reset_process_resource_quota_for_test;

// Bridge QuotaError → AuraError for AuraResult hot paths.
[[nodiscard]] inline aura::core::AuraError to_aura_error(const QuotaError& e) {
    return aura::core::AuraError{aura::core::AuraErrorKind::ResourceQuotaExceeded, e.message};
}

[[nodiscard]] inline aura::core::VoidResult check_as_void(ResourceQuota& q, Dimension d,
                                                          std::uint64_t amount) {
    if (auto err = q.check(d, amount))
        return aura::core::make_unexpected(aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                           err->message);
    return {};
}

[[nodiscard]] inline aura::core::VoidResult consume_as_void(ResourceQuota& q, Dimension d,
                                                            std::uint64_t amount) {
    if (auto err = q.check_and_consume(d, amount))
        return aura::core::make_unexpected(aura::core::AuraErrorKind::ResourceQuotaExceeded,
                                           err->message);
    return {};
}

// Module-level phase marker for Agent dashboards.
inline std::atomic<std::uint64_t> resource_quota_module_active{1};

} // namespace aura::core::resource_quota
