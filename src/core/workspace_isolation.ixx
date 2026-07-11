// workspace_isolation.ixx — Issues #1180/#1183 Phase 1: Tenant principal scaffold.

module;

export module aura.core.workspace_isolation;

import std;

export namespace aura::core::workspace_isolation {

inline constexpr int kWorkspaceIsolationPhase = 1;

using TenantId = std::uint64_t;

struct TenantPrincipal {
    TenantId id = 0;
    std::string_view name;
    bool allow_cross_tenant = false;
};

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

inline WorkspaceIsolationPolicy g_workspace_isolation{};

} // namespace aura::core::workspace_isolation
