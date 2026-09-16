// tenant_host_path.hh — Issue #3802: host FS path-prefix isolation under
// Restricted+MT / Strict for EXEMPT_2ARG write-file / sys-* (and siblings).
//
// EXEMPT_2ARG ops call 2-arg require_effect (capability bits) but skip
// require_effect_on_ref / NodeId tenant provenance. Under Restricted+MT,
// Agents sharing a process must not clobber a shared FS namespace via
// absolute paths. Soft/Off and single-tenant Restricted keep passthrough
// (AC2). No NodeId redesign; no new query key (AC3).
//
// Policy (when active):
//   - Derive tenant root from capability_tenant_id_ under
//     AURA_TENANT_FS_ROOT (else AURA_PERSIST_DIR/tenants, else
//     $TMPDIR/aura-tenants) as `<base>/t-<id>`.
//   - Resolve relative paths under that root; absolute paths must stay
//     under the caller's root. Cross-tenant prefix or escape → Deny.
//
// Header-inline so file / io / security TUs share one lexical policy
// without link deps (same family as path_is_denied).

#ifndef AURA_COMPILER_TENANT_HOST_PATH_HH
#define AURA_COMPILER_TENANT_HOST_PATH_HH

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace aura::compiler::security {

inline constexpr int kTenantHostPathIsolationIssue = 3802;
// Auditable SE reason (IsolationDeny) — keep ≤63 chars for SecurityEvent.
inline constexpr const char* kTenantPathEscapeReason = "tenant-path-escape";
inline constexpr const char* kEnvTenantFsRoot = "AURA_TENANT_FS_ROOT";

enum class TenantHostPathVerdict : std::uint8_t {
    Passthrough = 0, // Soft/Off / single-tenant Restricted — use path as-is
    Resolved = 1,    // use `resolved` absolute path under tenant root
    Deny = 2,        // cross-tenant / escape — zero write
};

struct TenantHostPathResult {
    TenantHostPathVerdict verdict = TenantHostPathVerdict::Passthrough;
    std::string resolved{};
};

// True when host-path isolation must arm: Strict, or Restricted+MT.
// Soft/Off (mode 0) and Restricted without MT stay false (AC2).
[[nodiscard]] inline bool tenant_host_path_policy_active(std::uint8_t effect_sandbox_mode,
                                                         bool multi_tenant) noexcept {
    if (effect_sandbox_mode == 2) // Strict
        return true;
    if (effect_sandbox_mode == 1 && multi_tenant) // Restricted+MT
        return true;
    return false;
}

[[nodiscard]] inline std::string tenant_host_roots_base() {
    if (const char* e = std::getenv(kEnvTenantFsRoot); e && e[0] != '\0')
        return std::string(e);
    if (const char* e = std::getenv("AURA_PERSIST_DIR"); e && e[0] != '\0')
        return std::string(e) + "/tenants";
    const char* tmp = std::getenv("TMPDIR");
    return std::string(tmp && tmp[0] != '\0' ? tmp : "/tmp") + "/aura-tenants";
}

[[nodiscard]] inline std::string tenant_host_root_for(std::uint64_t tenant_id) {
    return tenant_host_roots_base() + "/t-" + std::to_string(tenant_id);
}

// Lexical join + collapse of "." / ".." (no symlink resolution).
// Rejects NUL. Absolute inputs stay absolute; relative stay relative
// until the caller prefixes the tenant root.
[[nodiscard]] inline std::string lexical_normalize_path(std::string_view path) {
    if (path.empty() || path.find('\0') != std::string_view::npos)
        return {};
    const bool abs = !path.empty() && path.front() == '/';
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == ".") {
            cur.clear();
            return;
        }
        if (cur == "..") {
            if (!parts.empty() && parts.back() != "..")
                parts.pop_back();
            else if (!abs)
                parts.push_back("..");
            // absolute: ".." at root is a no-op (cannot escape past /)
            cur.clear();
            return;
        }
        parts.push_back(cur);
        cur.clear();
    };
    for (char c : path) {
        if (c == '/')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    std::string out = abs ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out.push_back('/');
        out += parts[i];
    }
    if (out.empty())
        out = abs ? "/" : ".";
    // Preserve a trailing slash only for root "/"
    return out;
}

[[nodiscard]] inline bool path_is_under_root(std::string_view path,
                                             std::string_view root) noexcept {
    if (root.empty() || path.empty())
        return false;
    if (path == root)
        return true;
    if (path.size() > root.size() && path.starts_with(root) && path[root.size()] == '/')
        return true;
    return false;
}

// Extract tenant id from `<base>/t-<id>(/...)` when path sits under base.
// Returns 0 when not a tenant-prefixed path.
[[nodiscard]] inline std::uint64_t tenant_id_from_host_path(std::string_view path,
                                                            std::string_view base) noexcept {
    if (!path_is_under_root(path, base) && path != base)
        return 0;
    const auto prefix = std::string(base) + "/t-";
    if (!path.starts_with(prefix))
        return 0;
    std::size_t i = prefix.size();
    if (i >= path.size() || path[i] < '0' || path[i] > '9')
        return 0;
    std::uint64_t id = 0;
    while (i < path.size() && path[i] >= '0' && path[i] <= '9') {
        id = id * 10 + static_cast<std::uint64_t>(path[i] - '0');
        ++i;
    }
    if (i != path.size() && path[i] != '/')
        return 0; // not a clean t-<id> segment
    return id;
}

// Core resolve. When !policy_active → Passthrough.
// When active + tenant_id==0 → Deny (no root).
// Relative → join under tenant root. Absolute must remain under own root.
[[nodiscard]] inline TenantHostPathResult
resolve_tenant_host_path(std::string_view path, std::uint64_t tenant_id, bool policy_active) {
    TenantHostPathResult r;
    if (!policy_active) {
        r.verdict = TenantHostPathVerdict::Passthrough;
        return r;
    }
    if (tenant_id == 0 || path.empty() || path.find('\0') != std::string_view::npos) {
        r.verdict = TenantHostPathVerdict::Deny;
        return r;
    }
    const auto norm = lexical_normalize_path(path);
    if (norm.empty()) {
        r.verdict = TenantHostPathVerdict::Deny;
        return r;
    }
    const auto base = tenant_host_roots_base();
    const auto root = tenant_host_root_for(tenant_id);
    std::string abs;
    if (!norm.empty() && norm.front() == '/') {
        abs = norm;
    } else {
        // relative → under tenant root
        abs = lexical_normalize_path(root + "/" + norm);
    }
    if (abs.empty()) {
        r.verdict = TenantHostPathVerdict::Deny;
        return r;
    }
    // Cross-tenant: under base/t-<other>
    const auto other = tenant_id_from_host_path(abs, base);
    if (other != 0 && other != tenant_id) {
        r.verdict = TenantHostPathVerdict::Deny;
        return r;
    }
    // Must stay under own root (absolute escape / foreign prefix).
    if (!path_is_under_root(abs, root)) {
        r.verdict = TenantHostPathVerdict::Deny;
        return r;
    }
    r.verdict = TenantHostPathVerdict::Resolved;
    r.resolved = std::move(abs);
    return r;
}

} // namespace aura::compiler::security

#endif // AURA_COMPILER_TENANT_HOST_PATH_HH
