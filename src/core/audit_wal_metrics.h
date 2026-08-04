// audit_wal_metrics.h — Issue #2492: stub header re-exporting AuditWalMetrics
// for tests that #include this path. The actual struct lives in
// mutation_audit_wal.hh (kept under its original name to avoid touching
// the storage layout). Tests reference the short path because they pair
// audit_wal metrics with other core/ headers (capability_model, sandbox,
// workspace_epoch, workspace_isolation). This header is consumed only by
// tests/ — production code should include mutation_audit_wal.hh directly.

#ifndef AURA_CORE_AUDIT_WAL_METRICS_H
#define AURA_CORE_AUDIT_WAL_METRICS_H

#include "core/mutation_audit_wal.hh"

namespace aura::core {

// Re-export at the core:: namespace level for test convenience.
// Production paths should use aura::core::audit_wal::AuditWalMetrics
// directly from mutation_audit_wal.hh.
using AuditWalMetrics = ::aura::core::audit_wal::AuditWalMetrics;

} // namespace aura::core

#endif // AURA_CORE_AUDIT_WAL_METRICS_H