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

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace aura::compiler::security {

inline constexpr int kTenantHostPathIsolationIssue = 3802;
// Issue #3994: check_tenant_host_path Typed correlate stamps tenant_id.
inline constexpr int kHostPathCorrelateTenantIssue = 3994;
// Auditable SE reason (IsolationDeny) — keep ≤63 chars for SecurityEvent.
inline constexpr const char* kTenantPathEscapeReason = "tenant-path-escape";
inline constexpr const char* kEnvTenantFsRoot = "AURA_TENANT_FS_ROOT";
// Issue #4233: exec-face jail for shell / command-output (require_effect
// siblings) shares the #3802 policy predicate and deny reason.
inline constexpr int kTenantExecJailIssue = 4233;

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

// Issue #4233: exec-face escape scan for shell / command-output command
// strings. The exec child runs chdir'd under the tenant root (relative-only
// contract), so any ABSOLUTE path token, ".." segment, or expansion escape
// ($VAR / `cmd` / ~user) can land outside the caller's root — deny at the
// fence (IsolationDeny tenant-path-escape, zero exec). Shell metacharacters
// (; | & < > ( ) { }) split tokens too, so redirect-prefixed (>/abs,
// 2>/abs), ';'-joined (cmd;cat /etc/passwd) and '='-joined
// (--opt=/abs) forms are caught. Fail-closed: a token that merely looks
// absolute (quoted prose) denies too — jail posture over convenience.
// Issue #4380: quotes are NOT meta, so an absolute path can hide
// mid-token — `python3 -c 'open("/etc/passwd").read()'`,
// `perl -e 'open(F,"/etc/passwd")'`, `--lib=/etc/x`. The scan stays
// byte-local (no shell grammar): any `/` that is not clearly part of a
// relative path denies — token-initial, or preceded by a byte outside
// [A-Za-z0-9._-] (quote, '=', '(', ',', ':', ...). Plain `sub/file` and
// `./x` keep the relative-only contract.
// Soft/Off / single-tenant Restricted never call this (the policy
// predicate in check_tenant_exec_jail owns the arm).
[[nodiscard]] inline bool tenant_exec_cmd_escapes_root(std::string_view cmd) noexcept {
    if (cmd.empty() || cmd.find('\0') != std::string_view::npos)
        return true;
    if (cmd.find("..") != std::string_view::npos || cmd.find('$') != std::string_view::npos ||
        cmd.find('`') != std::string_view::npos || cmd.find('~') != std::string_view::npos)
        return true;
    auto is_meta = [](char c) noexcept {
        return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '|' || c == '&' || c == '<' ||
               c == '>' || c == '(' || c == ')' || c == '{' || c == '}';
    };
    std::size_t tok_begin = 0;
    for (std::size_t i = 0; i <= cmd.size(); ++i) {
        if (i == cmd.size() || is_meta(cmd[i])) {
            if (i > tok_begin) {
                const std::string_view tok = cmd.substr(tok_begin, i - tok_begin);
                // Issue #4380: quote-blind byte-local scan — every `/` must
                // sit inside a plain relative path. Token-initial `/` is the
                // #4233 absolute token; a `/` after any byte that cannot
                // continue a relative path ('"', '=', '(', ',', ':', '%',
                // redirect-adjacent joins) is an embedded absolute path
                // (interpreter argument / quoted prose / --opt=/abs form —
                // the '=' special case is subsumed by the set below).
                for (std::size_t j = 0; j < tok.size(); ++j) {
                    if (tok[j] != '/')
                        continue;
                    if (j == 0)
                        return true; // absolute path token
                    const char prev = tok[j - 1];
                    const bool rel_cont =
                        (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                        (prev >= '0' && prev <= '9') || prev == '.' || prev == '_' || prev == '-';
                    if (!rel_cont)
                        return true; // embedded absolute path (#4380)
                }
            }
            tok_begin = i + 1;
        }
    }
    return false;
}

// Issue #4380: lexical resolve + O_NOFOLLOW guard only the FINAL path
// component. A symlink PRE-PLACED under the tenant root (off-jail
// provisioning path — the #4380 scanner above now denies exec-time
// creation) turns a relative `e/passwd` into a host read/write outside
// the prefix: O_NOFOLLOW does not apply to intermediate components.
// Fail-closed: lstat every component BELOW the tenant root with
// AT_SYMLINK_NOFOLLOW; any symlink (including the final one, which the
// open would only fail on silently) reports escape so the caller emits
// IsolationDeny tenant-path-escape at the fence. Missing components are
// not an escape — the open/create path reports them. Caller:
// Evaluator::check_tenant_host_path under active policy only — Soft/Off
// / single-tenant Restricted stay zero-cost passthrough.
[[nodiscard]] inline bool tenant_path_has_symlink_component(std::string_view rooted_path,
                                                            std::string_view root) {
    if (root.empty() || rooted_path.size() <= root.size() || !rooted_path.starts_with(root))
        return false; // not a rooted tenant path — nothing to walk
    std::size_t i = root.size();
    while (i < rooted_path.size()) {
        if (rooted_path[i] == '/') {
            ++i;
            continue;
        }
        const auto next = rooted_path.find('/', i);
        const auto end = next == std::string_view::npos ? rooted_path.size() : next;
        const std::string probe(rooted_path.substr(0, end));
        struct stat st{};
        if (::lstat(probe.c_str(), &st) != 0)
            return false; // missing component — the open/create reports it
        if (S_ISLNK(st.st_mode))
            return true;
        i = end;
    }
    return false;
}

// Issue #4382: http-* scheme allowlist for http-get / http-post — under
// the tenant jail only http:// and https:// may be named. file:// and any
// other scheme is a host-FS read/write in URL clothing (a Network grant
// must not reach /etc/passwd), and redirects onto them are refused by the
// CURLOPT_REDIR_PROTOCOLS / --proto-redir layers. Case-insensitive per
// RFC 3986 (scheme normalization). Soft/Off / single-tenant Restricted
// never consult this (the policy predicate in the Evaluator fence owns
// the arm).
[[nodiscard]] inline bool tenant_http_url_scheme_allowed(std::string_view url) noexcept {
    auto starts_with_ci = [](std::string_view u, std::string_view scheme) noexcept {
        if (u.size() < scheme.size())
            return false;
        for (std::size_t i = 0; i < scheme.size(); ++i) {
            char c = u[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
            if (c != scheme[i])
                return false;
        }
        return true;
    };
    return starts_with_ci(url, "http://") || starts_with_ci(url, "https://");
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

// Issue #5212: lstat-then-open is a TOCTOU. These helpers open, create, and
// unlink strictly beneath an already-opened tenant-root directory fd.
// Soft/Off and single-tenant Restricted never call them (the policy
// predicate stays the zero-cost gate). Linux >= 5.6 uses openat2
// RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS. Older
// kernels walk with openat(O_PATH | O_NOFOLLOW | O_DIRECTORY) and
// O_NOFOLLOW on the last component. Create uses mkdirat / openat from
// that parent fd — a post-open readlink is too late once O_CREAT or
// mkdir has already landed outside the jail.

#ifdef SYS_openat2
inline constexpr long kSysOpenat2 = SYS_openat2;
#else
inline constexpr long kSysOpenat2 = 437;
#endif

inline constexpr std::uint64_t kResolveNoMagiclinks = 0x02;
inline constexpr std::uint64_t kResolveNoSymlinks = 0x04;
inline constexpr std::uint64_t kResolveBeneath = 0x08;

struct TenantOpenHow {
    std::uint64_t flags = 0;
    std::uint64_t mode = 0;
    std::uint64_t resolve = 0;
};
static_assert(sizeof(TenantOpenHow) == 24, "openat2 how must match the kernel ABI");

// ELOOP: symlink (RESOLVE_NO_SYMLINKS / O_NOFOLLOW). EXDEV: ".." or an
// absolute component left the root (RESOLVE_BENEATH). EPERM: a component
// this walk rejected ("..", empty, absolute). ENOTDIR is NOT an escape:
// O_DIRECTORY on a regular file returns it, and so does O_DIRECTORY on a
// final symlink — the lexical lstat fence still denies a stable symlink.
[[nodiscard]] inline bool tenant_fd_errno_is_escape(int err) noexcept {
    return err == ELOOP || err == EXDEV || err == EPERM;
}

[[nodiscard]] inline bool tenant_rel_has_dotdot(std::string_view rel) noexcept {
    std::size_t i = 0;
    while (i < rel.size()) {
        if (rel[i] == '/') {
            ++i;
            continue;
        }
        const auto slash = rel.find('/', i);
        const auto end = slash == std::string_view::npos ? rel.size() : slash;
        const auto name = rel.substr(i, end - i);
        if (name.empty() || name == "." || name == ".." ||
            name.find('\0') != std::string_view::npos)
            return true;
        i = end;
    }
    return false;
}

// Relative remainder of `resolved` under `root`. "." means the root itself.
// False when `resolved` is not under `root` (caller treats that as escape).
[[nodiscard]] inline bool tenant_rel_under_root(std::string_view resolved, std::string_view root,
                                                std::string& rel) {
    rel.clear();
    if (!path_is_under_root(resolved, root))
        return false;
    if (resolved.size() == root.size()) {
        rel = ".";
        return true;
    }
    rel.assign(resolved.substr(root.size() + 1));
    return !rel.empty();
}

[[nodiscard]] inline int open_under_tenant_root_walk(int root_fd, std::string_view rel, int flags,
                                                     mode_t mode) noexcept {
    if (tenant_rel_has_dotdot(rel)) {
        errno = EPERM;
        return -1;
    }
    int dirfd = ::openat(root_fd, ".", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0)
        return -1;
    std::size_t i = 0;
    bool any = false;
    while (i < rel.size()) {
        if (rel[i] == '/') {
            ++i;
            continue;
        }
        const auto slash = rel.find('/', i);
        const auto end = slash == std::string_view::npos ? rel.size() : slash;
        const bool last = slash == std::string_view::npos;
        const auto name = rel.substr(i, end - i);
        if (name.size() > 255) {
            ::close(dirfd);
            errno = ENAMETOOLONG;
            return -1;
        }
        char leaf[256];
        for (std::size_t n = 0; n < name.size(); ++n)
            leaf[n] = name[n];
        leaf[name.size()] = '\0';
        any = true;
        if (!last) {
            const int next = ::openat(dirfd, leaf, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
            const int err = errno;
            ::close(dirfd);
            if (next < 0) {
                // A symlink with O_DIRECTORY | O_NOFOLLOW is ENOTDIR. That
                // intermediate is an escape, not a type mismatch on the leaf.
                errno = err == ENOTDIR ? ELOOP : err;
                return -1;
            }
            dirfd = next;
            i = end;
            continue;
        }
        const int fd = ::openat(dirfd, leaf, flags, mode);
        const int err = errno;
        ::close(dirfd);
        if (fd < 0)
            errno = err;
        return fd;
    }
    if (!any) {
        const int fd = ::openat(dirfd, ".", flags, mode);
        const int err = errno;
        ::close(dirfd);
        if (fd < 0)
            errno = err;
        return fd;
    }
    ::close(dirfd);
    errno = ENOENT;
    return -1;
}

// Open `rel` beneath `root_fd`. Returns a fd, or -1 with errno. Does not
// emit a security event — the caller does, on tenant_fd_errno_is_escape.
// O_NOFOLLOW is forced. O_PATH is not used here: with RESOLVE_NO_SYMLINKS
// the kernel still returns a symlink fd for O_PATH, which would report the
// link as present instead of failing the escape.
[[nodiscard]] inline int open_under_tenant_root(int root_fd, std::string_view rel, int flags,
                                                mode_t mode) noexcept {
    if (root_fd < 0) {
        errno = EBADF;
        return -1;
    }
    const int use = flags | O_CLOEXEC | O_NOFOLLOW;
    // "." is the root directory itself (directory-list / unlink parent), not
    // a component the walk rejects.
    if (rel == ".") {
        const int fd = ::openat(root_fd, ".", use, mode);
        return fd;
    }
    if (rel.empty() || rel.front() == '/' || rel.find('\0') != std::string_view::npos ||
        tenant_rel_has_dotdot(rel)) {
        errno = EPERM;
        return -1;
    }
    const std::string path(rel);
    TenantOpenHow how{};
    how.flags = static_cast<std::uint64_t>(use);
    how.mode = (use & (O_CREAT | O_TMPFILE)) != 0 ? static_cast<std::uint64_t>(mode) : 0;
    how.resolve = kResolveNoMagiclinks | kResolveNoSymlinks | kResolveBeneath;
    const int fd =
        static_cast<int>(::syscall(kSysOpenat2, root_fd, path.c_str(), &how, sizeof how));
    if (fd >= 0 || (errno != ENOSYS && errno != EOPNOTSUPP))
        return fd;
    return open_under_tenant_root_walk(root_fd, rel, use, mode);
}

// Host base of `<base>/t-<id>` may be created by path (it is not inside a
// tenant). The `t-<id>` leaf is mkdirat + O_NOFOLLOW, so a symlink planted
// at the tenant root is not followed.
[[nodiscard]] inline int open_tenant_root_dirfd(std::string_view root, bool create) noexcept {
    const auto slash = root.rfind('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= root.size()) {
        errno = EPERM;
        return -1;
    }
    const std::string base(root.substr(0, slash));
    const std::string leaf(root.substr(slash + 1));
    if (create) {
        std::string cur;
        cur.reserve(base.size());
        for (std::size_t i = 0; i < base.size(); ++i) {
            cur.push_back(base[i]);
            if (base[i] == '/' || i + 1 == base.size()) {
                if (cur.size() > 1)
                    ::mkdir(cur.c_str(), 0755);
            }
        }
    }
    const int base_fd = ::open(base.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (base_fd < 0)
        return -1;
    if (create && ::mkdirat(base_fd, leaf.c_str(), 0755) != 0 && errno != EEXIST) {
        const int err = errno;
        ::close(base_fd);
        errno = err;
        return -1;
    }
    const int fd = ::openat(base_fd, leaf.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const int err = errno;
    ::close(base_fd);
    if (fd < 0) {
        errno = (err == ENOTDIR) ? ELOOP : err;
        return -1;
    }
    return fd;
}

// mkdir -p of `rel` beneath `root_fd`. 0 on success, -1 with errno.
[[nodiscard]] inline int mkdir_under_tenant_root(int root_fd, std::string_view rel,
                                                 mode_t mode) noexcept {
    if (rel.empty() || rel == ".")
        return 0;
    if (tenant_rel_has_dotdot(rel) || rel.front() == '/') {
        errno = EPERM;
        return -1;
    }
    int dirfd = ::openat(root_fd, ".", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0)
        return -1;
    std::size_t i = 0;
    while (i < rel.size()) {
        if (rel[i] == '/') {
            ++i;
            continue;
        }
        const auto slash = rel.find('/', i);
        const auto end = slash == std::string_view::npos ? rel.size() : slash;
        const auto name = rel.substr(i, end - i);
        if (name.empty() || name == "." || name == ".." || name.size() > 255) {
            ::close(dirfd);
            errno = name.size() > 255 ? ENAMETOOLONG : EPERM;
            return -1;
        }
        char leaf[256];
        for (std::size_t n = 0; n < name.size(); ++n)
            leaf[n] = name[n];
        leaf[name.size()] = '\0';
        if (::mkdirat(dirfd, leaf, mode) != 0 && errno != EEXIST) {
            const int err = errno;
            ::close(dirfd);
            errno = err;
            return -1;
        }
        const int next = ::openat(dirfd, leaf, O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC);
        const int err = errno;
        ::close(dirfd);
        if (next < 0) {
            errno = err == ENOTDIR ? ELOOP : err;
            return -1;
        }
        dirfd = next;
        i = end;
    }
    ::close(dirfd);
    return 0;
}

// 1 = unlinked, 0 = directory or missing (not an escape), -1 = error.
[[nodiscard]] inline int unlink_under_tenant_root(int root_fd, std::string_view rel) noexcept {
    if (rel.empty() || rel == "." || rel == ".." || tenant_rel_has_dotdot(rel)) {
        errno = EPERM;
        return -1;
    }
    const auto slash = rel.rfind('/');
    const std::string parent =
        slash == std::string::npos ? std::string(".") : std::string(rel.substr(0, slash));
    const std::string leaf =
        slash == std::string::npos ? std::string(rel) : std::string(rel.substr(slash + 1));
    if (leaf.empty() || leaf == "." || leaf == "..") {
        errno = EPERM;
        return -1;
    }
    const int parent_fd =
        open_under_tenant_root(root_fd, parent, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
    if (parent_fd < 0)
        return -1;
    struct stat st{};
    if (::fstatat(parent_fd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        const int err = errno;
        ::close(parent_fd);
        errno = err;
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        ::close(parent_fd);
        return 0;
    }
    const int rc = ::unlinkat(parent_fd, leaf.c_str(), 0);
    const int err = errno;
    ::close(parent_fd);
    if (rc != 0) {
        errno = err;
        return -1;
    }
    return 1;
}

} // namespace aura::compiler::security

#endif // AURA_COMPILER_TENANT_HOST_PATH_HH
