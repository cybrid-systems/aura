// workspace_isolation.ixx — module re-export of WorkspaceIsolationPolicy SSOT.
//
// Full check_boundary_ex + metrics live in workspace_isolation.hh.
// This unit only re-exports for C++ module consumers (resource_quota /
// lifetime_pin pattern). Prior module scaffold
// (g_workspace_isolation_scaffold) was a second, incomplete policy
// object — never import that path.
//
// Do NOT reintroduce a second WorkspaceIsolationPolicy body here.

module;
#include "core/workspace_isolation.hh"

export module aura.core.workspace_isolation;

import std;

export namespace aura::core::workspace_isolation {

using ::aura::core::workspace_isolation::check_boundary;
using ::aura::core::workspace_isolation::CrossTenantKey;
using ::aura::core::workspace_isolation::CrossTenantKeyHash;
using ::aura::core::workspace_isolation::g_tenant_isolation_metrics;
using ::aura::core::workspace_isolation::g_workspace_isolation;
using ::aura::core::workspace_isolation::IsolationAuditEntry;
using ::aura::core::workspace_isolation::IsolationRefProvenance;
using ::aura::core::workspace_isolation::kWorkspaceIsolationIssue;
using ::aura::core::workspace_isolation::kWorkspaceIsolationPhase;
using ::aura::core::workspace_isolation::PublishedIsolationSlot;
using ::aura::core::workspace_isolation::reset_tenant_isolation_for_test;
using ::aura::core::workspace_isolation::snapshot_tenant_isolation_stats;
using ::aura::core::workspace_isolation::TenantId;
using ::aura::core::workspace_isolation::TenantIsolationMetrics;
using ::aura::core::workspace_isolation::TenantIsolationStatsSnapshot;
using ::aura::core::workspace_isolation::TenantPrincipal;
using ::aura::core::workspace_isolation::WorkspaceIsolationPolicy;

} // namespace aura::core::workspace_isolation
