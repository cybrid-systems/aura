// workspace_isolation.ixx — Issues #1180/#1183/#1566: WorkspaceIsolationPolicy.
// Full check_boundary_ex + metrics live in workspace_isolation.hh.

module;

export module aura.core.workspace_isolation;

import std;

export namespace aura::core::workspace_isolation {

inline constexpr int kWorkspaceIsolationPhase = 2; // #1566 enforcement
inline constexpr int kWorkspaceIsolationIssue = 1566;

using TenantId = std::uint64_t;

// Layout-stable principal (name/id/allow_cross — do not change).
struct TenantPrincipal {
    TenantId id = 0;
    std::string_view name;
    bool allow_cross_tenant = false;
};

// Module-visible scaffold; process-wide enforcement is in .hh.
struct WorkspaceIsolationPolicy {
    TenantPrincipal current;
    std::uint64_t boundary_checks = 0;
    std::uint64_t denials = 0;

    [[nodiscard]] bool check_boundary(TenantId target) noexcept {
        ++boundary_checks;
        if (current.id == 0 || current.allow_cross_tenant || current.id == target)
            return true;
        ++denials;
        return false;
    }
};

inline WorkspaceIsolationPolicy g_workspace_isolation_scaffold{};

} // namespace aura::core::workspace_isolation
