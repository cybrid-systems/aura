// evaluator_primitives_file.cpp — P0 step 25: read / read-file / write-file / file-* / shell /
// directory-list aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.

module;

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "runtime_shared.h"
#include "security_capabilities.h"
#include "security_side_effect.hh" // #2057
#include "tenant_host_path.hh"     // #3802 tenant FS path-prefix
#include "tenant_exec_fs_jail.hh"  // #4413 landlock + stat emulation
#include "write_scope.hh"          // #4912 write:out/
#include "core/ownership.hh"       // #4936 unique_file / immortal<T>
#include "compiler/chunk_io.hh"    // #4936 std::array read/copy chunks

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function_ref<void(std::string, PrimFn)>;

// Issue #918 Phase 1: explicit using-declarations (no `using namespace`).
using types::as_bool;
using types::as_cell_id;
using types::as_closure_id;
using types::as_float;
using types::as_hash_idx;
using types::as_int;
using types::as_pair_idx;
using types::as_primitive_slot;
using types::as_string_idx;
using types::as_vector_idx;
using types::EvalValue;
using types::is_bool;
using types::is_cell;
using types::is_closure;
using types::is_error;
using types::is_float;
using types::is_hash;
using types::is_int;
using types::is_pair;
using types::is_primitive;
using types::is_string;
using types::is_vector;
using types::is_void;
using types::make_bool;
using types::make_cell;
using types::make_closure;
using types::make_error;
using types::make_float;
using types::make_hash;
using types::make_int;
using types::make_pair;
using types::make_primitive;
using types::make_string;
using types::make_vector;
using types::make_void;

void register_file_primitives(PrimRegistrar add, Evaluator& ev) {
    // Issue #3174: read-file / write-file stay on the core boot list.
    // file-*/directory-list/shell/command-* defer until std/io or std/process.
    // Issue #5033: registration order is the original single-function order.
    RegSplitParts::register_file_read_primitives(add, ev);
    RegSplitParts::register_file_write_primitives(add, ev);
    RegSplitParts::register_file_probe_primitives(add, ev);
    RegSplitParts::register_file_entry_primitives(add, ev);
    RegSplitParts::register_exec_primitives(add, ev);
    RegSplitParts::register_directory_list_primitive(add, ev);
}

// Issue #5033: the file-IO capability policy shared by the parts below.
// The primitives outlive registration, so these are static helpers taking
// the Evaluator by reference (never a captured sibling closure).
EvalValue RegSplitParts::file_deny_io(Evaluator& ev, std::string_view cap, std::string_view msg) {
    if (!ev.sandbox_mode() || ev.has_capability(cap) ||
        ev.has_capability(aura::compiler::security::kCapIo) ||
        ev.has_capability(aura::compiler::security::kCapWildcard))
        return make_void();
    ev.bump_capability_denial();
    return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                ev.primitive_error_counter_ptr());
}

EvalValue RegSplitParts::file_deny_exec(Evaluator& ev, std::string_view msg) {
    if (!ev.sandbox_mode() || ev.has_capability(aura::compiler::security::kCapExec) ||
        ev.has_capability(aura::compiler::security::kCapWildcard))
        return make_void();
    ev.bump_capability_denial();
    return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                ev.primitive_error_counter_ptr());
}

// Helper: check path is a regular file (skip directories). Uses lstat so
// symlink targets are not followed for the type check (#1171 TOCTOU family).
bool RegSplitParts::file_is_regular(const std::string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Issue #1163/#1164/#1165/#2487: shared path_is_denied policy
// (security_capabilities.h) — used by read/write/file-* and sys-open.
bool RegSplitParts::file_path_is_denied(std::string_view path) {
    return aura::compiler::security::path_is_denied(path);
}

// Issue #4917: every write-file failure is an error value naming the
// path and the reason ("io error: write-file /x/y: ENOENT: directory
// does not exist"). It used to be (), indistinguishable from success.
EvalValue RegSplitParts::file_io_fail(Evaluator& ev, std::string_view op, std::string_view path,
                                      std::string_view why) {
    std::string msg(op);
    if (!path.empty()) {
        msg += ' ';
        msg += path;
    }
    msg += ": ";
    msg += why;
    auto err = make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    ev.mark_error_kind(err, Evaluator::kErrorMarkIo);
    return err;
}

std::string RegSplitParts::file_errno_reason(int e) {
    switch (e) {
        case ENOENT:
            return "ENOENT: directory does not exist";
        case EACCES:
            return "EACCES: permission denied";
        case ELOOP:
            return "ELOOP: refusing to follow a symlink";
        case EISDIR:
            return "EISDIR: is a directory";
        case ENOTDIR:
            return "ENOTDIR: a path component is not a directory";
        case ENOSPC:
            return "ENOSPC: no space left on device";
        case EROFS:
            return "EROFS: read-only file system";
        default:
            return std::format("errno {}: {}", e, std::strerror(e));
    }
}

// Issue #4912: `--grant write:out/` / the ci preset scope Write to
// directories. void = in scope (or no scope); else the refusal value.
EvalValue RegSplitParts::file_deny_write_scope(Evaluator& ev, std::string_view op,
                                               std::string_view path, std::string_view resolved) {
    if (aura::compiler::security::path_in_write_scope(resolved))
        return make_void();
    ev.bump_capability_denial();
    return make_primitive_error(ev.string_heap_, ev.error_values_,
                                aura::compiler::security::write_scope_deny_reason(op, path),
                                ev.primitive_error_counter_ptr());
}

// Issue #5033: read / read-file — stdin and file content ingestion.
void RegSplitParts::register_file_read_primitives(PrimRegistrar add, Evaluator& ev) {
    add("read", [&ev](const auto&) {
        // Issue #4504: shared-process stdin is a host io-read channel (same
        // class as /proc/self/cmdline, #2478). Fence BEFORE getline so a
        // denied read consumes nothing from stdin.
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                       "capability denied: io-read required");
            is_error(denied))
            return denied;
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = ev.string_heap_.mint(std::move(line));
        return make_string(id);
    });
    add("read-file", [&ev](const auto& a) {
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                       "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& path = ev.mutable_string_at(idx);
        // Issue #1163/#1171: deny sensitive paths; open with O_NOFOLLOW to
        // collapse lstat+open TOCTOU (symlink swap between check and open).
        if (file_path_is_denied(path))
            return make_void();
        // Issue #3835: Restricted+MT / Strict → resolve under tenant root;
        // cross-tenant / escape → deny + IsolationDeny SE (mirror #3802 write-file).
        std::string resolved;
        if (!ev.check_tenant_host_path(path, resolved, "read-file"))
            return make_void();
        int fd = ::open(resolved.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return make_void();
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            return make_void();
        }
        const ::aura::core::unique_file fp{::fdopen(fd, "r")}; // owns fd from here
        if (!fp) {
            ::close(fd);
            return make_void();
        }
        std::string content;
        std::array<char, aura::compiler::io::kReadChunk> buf{};
        while (std::size_t n = ::fread(buf.data(), 1, buf.size(), fp.get()))
            content.append(buf.data(), n);
        auto id = ev.string_heap_.mint(std::move(content));
        return make_string(id);
    });
}

// Issue #5033: write-file / make-directory — Write-gated host output.
void RegSplitParts::register_file_write_primitives(PrimRegistrar add, Evaluator& ev) {
    // AURA_SIDE_EFFECT_PRIM — write-file requires Write effect (#2057).
    // Returns 1 on success (documented in lib/std/INDEX.aura).
    add("write-file", [&ev](std::span<const EvalValue> a) {
        using aura::compiler::security::kEffectWrite;
        if (!ev.require_effect(kEffectWrite, "write-file")) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        aura::compiler::security::format_deny_reason(
                                            kEffectWrite, ev.capability_tenant_id(), "write-file"),
                                        ev.primitive_error_counter_ptr());
        }
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoWrite,
                                       "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.size() < 2 || !is_string(a[0]))
            return file_io_fail(ev, "write-file", "", "expected (write-file path-string content)");
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return file_io_fail(ev, "write-file", "", "invalid path string");
        auto& path = ev.mutable_string_at(idx);
        // Issue #1163: refuse /proc/self/mem and other process-memory paths.
        if (file_path_is_denied(path))
            return file_io_fail(ev, "write-file", path, "denied: protected path");
        // Issue #3802: Restricted+MT / Strict → resolve under tenant root;
        // cross-tenant / escape → deny + IsolationDeny SE, zero write.
        std::string resolved_path;
        if (!ev.check_tenant_host_path(path, resolved_path, "write-file"))
            return file_io_fail(ev, "write-file", path, "denied: tenant path");
        if (auto denied = file_deny_write_scope(ev, "write-file", path, resolved_path);
            is_error(denied))
            return denied;
        const std::string& use_path = resolved_path;
        std::string content;
        if (is_string(a[1])) {
            auto cidx = as_string_idx(a[1]);
            if (cidx < ev.string_heap_.size()) {
                // Issue #4559: a Write grant is not a grant to copy another
                // principal's stamped string. In-range before the bytes
                // reach the host buffer; deny leaves the file untouched.
                if (!ev.inrange_string_tenant_allows(cidx, kEffectWrite, "write-file"))
                    return file_io_fail(ev, "write-file", path, "denied: tenant string");
                content = ev.string_at(cidx);
            }
        } else if (is_int(a[1])) {
            content = std::to_string(as_int(a[1]));
        } else {
            return file_io_fail(ev, "write-file", path, "content must be a string or integer");
        }
        // O_NOFOLLOW | O_CREAT | O_WRONLY — no symlink follow to sensitive targets.
        // Issue #3802: use_path is tenant-rooted under Restricted+MT / Strict.
        int fd =
            ::open(use_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0)
            return file_io_fail(ev, "write-file", path, file_errno_reason(errno));
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            return file_io_fail(ev, "write-file", path, "not a regular file");
        }
        const char* p = content.data();
        std::size_t left = content.size();
        while (left > 0) {
            auto n = ::write(fd, p, left);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                const int e = n < 0 ? errno : EIO;
                ::close(fd);
                return file_io_fail(ev, "write-file", path, file_errno_reason(e));
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        if (::close(fd) != 0)
            return file_io_fail(ev, "write-file", path, file_errno_reason(errno));
        return make_int(1);
    });
    // Issue #4917: create an output directory without a shell. Same
    // Write gate and path policy as write-file; creates parents (mkdir -p).
    // 1 when the directory exists afterwards, else an io error value.
    add("make-directory", [&ev](std::span<const EvalValue> a) {
        using aura::compiler::security::kEffectWrite;
        if (!ev.require_effect(kEffectWrite, "make-directory")) {
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                aura::compiler::security::format_deny_reason(
                    kEffectWrite, ev.capability_tenant_id(), "make-directory"),
                ev.primitive_error_counter_ptr());
        }
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoWrite,
                                       "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return file_io_fail(ev, "make-directory", "", "expected (make-directory path-string)");
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return file_io_fail(ev, "make-directory", "", "invalid path string");
        auto& path = ev.mutable_string_at(idx);
        if (path.empty())
            return file_io_fail(ev, "make-directory", "", "empty path");
        if (file_path_is_denied(path))
            return file_io_fail(ev, "make-directory", path, "denied: protected path");
        std::string resolved;
        if (!ev.check_tenant_host_path(path, resolved, "make-directory"))
            return file_io_fail(ev, "make-directory", path, "denied: tenant path");
        if (auto denied = file_deny_write_scope(ev, "make-directory", path, resolved);
            is_error(denied))
            return denied;
        // mkdir -p, one component at a time; never follow a symlink as the
        // final component (lstat check after each step).
        std::size_t pos = resolved[0] == '/' ? 1 : 0;
        while (true) {
            const auto slash = resolved.find('/', pos);
            const std::string part =
                slash == std::string::npos ? resolved : resolved.substr(0, slash);
            if (!part.empty() && part != "." && part != "..") {
                if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
                    return file_io_fail(ev, "make-directory", path, file_errno_reason(errno));
                struct stat st{};
                if (::lstat(part.c_str(), &st) != 0)
                    return file_io_fail(ev, "make-directory", path, file_errno_reason(errno));
                if (!S_ISDIR(st.st_mode))
                    return file_io_fail(ev, "make-directory", path,
                                        S_ISLNK(st.st_mode) ? file_errno_reason(ELOOP)
                                                            : file_errno_reason(ENOTDIR));
            }
            if (slash == std::string::npos)
                break;
            pos = slash + 1;
        }
        return make_int(1);
    });
}

// Issue #5033: command-line / file-exists? — host recon probes.
void RegSplitParts::register_file_probe_primitives(PrimRegistrar add, Evaluator& ev) {
    // Issue #2478: require io-read — /proc/self/cmdline can leak secrets
    // (API keys, DSNs) from process argv. Same gate as file-exists? recon.
    ev.defer_std_host_prim("command-line", [&ev](const auto&) -> EvalValue {
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                       "capability denied: io-read required");
            is_error(denied))
            return denied;
        // Returns list of command-line argument strings, NOT including argv[0].
        // Parsed from /proc/self/cmdline on Linux.
        std::ifstream f("/proc/self/cmdline");
        if (!f)
            return make_void();
        std::string raw;
        std::getline(f, raw, '\0'); // skip argv[0]
        std::vector<std::string> items;
        while (std::getline(f, raw, '\0')) {
            if (!raw.empty())
                items.push_back(raw);
        }
        EvalValue result = make_void();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            auto sidx = ev.string_heap_.mint(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });
    ev.defer_std_host_prim("file-exists?", [&ev](std::span<const EvalValue> a) {
        // Issue #1172: require io-read for existence probes (recon).
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                       "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_bool(false);
        auto& path = ev.mutable_string_at(idx);
        if (file_path_is_denied(path))
            return make_bool(false);
        // Issue #3835: Restricted+MT / Strict tenant root (mirror
        // write-file).
        std::string resolved;
        if (!ev.check_tenant_host_path(path, resolved, "file-exists?"))
            return make_bool(false);
        struct stat st;
        // Issue #4919: a predicate — #t / #f, as std/fs documents.
        return make_bool(::lstat(resolved.c_str(), &st) == 0);
    });
    // Issue #5005: R7RS (scheme process-context) environment access.
    // Deferred with the other recon probes and io-read-gated at the
    // call (#2478: environment variables may hold secrets).  A missing
    // variable is #f, not an error; get-environment-variable(s) never
    // mutate.
    ev.defer_std_host_prim(
        "get-environment-variable", [&ev](std::span<const EvalValue> a) -> EvalValue {
            if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                           "capability denied: io-read required");
                is_error(denied))
                return denied;
            if (a.empty() || !is_string(a[0]))
                return make_bool(false);
            auto idx = as_string_idx(a[0]);
            if (idx >= ev.string_heap_.size())
                return make_bool(false);
            const std::string name = ev.string_at(idx);
            if (const char* v = ::getenv(name.c_str()))
                return make_string(ev.string_heap_.mint(v));
            return make_bool(false);
        });
    ev.defer_std_host_prim("get-environment-variables", [&ev](const auto&) -> EvalValue {
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                       "capability denied: io-read required");
            is_error(denied))
            return denied;
        EvalValue out = make_void(); // () is void in Aura; build backwards.
        for (char** e = environ; e && *e; ++e) {
            const std::string_view entry(*e);
            const auto eq = entry.find('=');
            if (eq == std::string_view::npos)
                continue;
            const auto name_idx = ev.string_heap_.mint(entry.substr(0, eq));
            const auto val_idx = ev.string_heap_.mint(entry.substr(eq + 1));
            const auto pidx = ev.pairs_.size();
            ev.pairs_.push_back({make_string(name_idx), make_string(val_idx)});
            const auto lidx = ev.pairs_.size();
            ev.pairs_.push_back({make_pair(pidx), out});
            out = make_pair(lidx);
        }
        return out;
    });
}

// Issue #5033: file-copy / file-delete / file-size — one-path entry operations.
void RegSplitParts::register_file_entry_primitives(PrimRegistrar add, Evaluator& ev) {
    ev.defer_std_host_prim("file-copy", [&ev](const auto& a) {
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoWrite,
                                       "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto sidx = as_string_idx(a[0]), didx = as_string_idx(a[1]);
        if (sidx >= ev.string_heap_.size() || didx >= ev.string_heap_.size())
            return make_void();
        auto& src_path = ev.mutable_string_at(sidx);
        auto& dst_path = ev.mutable_string_at(didx);
        // Issue #1165: validate both source and destination paths.
        if (file_path_is_denied(src_path) || file_path_is_denied(dst_path))
            return make_void();
        // Issue #3802: both ends under caller tenant root (Restricted+MT / Strict).
        std::string src_resolved, dst_resolved;
        if (!ev.check_tenant_host_path(src_path, src_resolved, "file-copy") ||
            !ev.check_tenant_host_path(dst_path, dst_resolved, "file-copy"))
            return make_void();
        if (auto denied = file_deny_write_scope(ev, "file-copy", dst_path, dst_resolved);
            is_error(denied))
            return denied;
        if (!file_is_regular(src_resolved))
            return make_void();
        int sfd = ::open(src_resolved.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (sfd < 0)
            return make_void();
        int dfd = ::open(dst_resolved.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (dfd < 0) {
            ::close(sfd);
            return make_void();
        }
        if (aura::compiler::io::copy_fd(sfd, dfd) != 0) {
            ::close(sfd);
            ::close(dfd);
            return make_void();
        }
        ::close(sfd);
        ::close(dfd);
        return make_int(1);
    });
    ev.defer_std_host_prim("file-delete", [&ev](std::span<const EvalValue> a) {
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoWrite,
                                       "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto& path = ev.mutable_string_at(idx);
        // Issue #1164: deny sensitive paths; refuse directories (no rmdir
        // cascade); use unlinkat AT_FDCWD without following final symlink
        // where possible.
        if (file_path_is_denied(path))
            return make_int(0);
        // Issue #3802: Restricted+MT / Strict tenant root.
        std::string resolved;
        if (!ev.check_tenant_host_path(path, resolved, "file-delete"))
            return make_int(0);
        if (auto denied = file_deny_write_scope(ev, "file-delete", path, resolved);
            is_error(denied))
            return denied;
        struct stat st{};
        if (::lstat(resolved.c_str(), &st) != 0)
            return make_int(0);
        if (S_ISDIR(st.st_mode))
            return make_int(0); // directories require explicit rmdir API
        return make_int(::unlink(resolved.c_str()) == 0 ? 1 : 0);
    });
    ev.defer_std_host_prim("file-size", [&ev](const auto& a) {
        // Issue #1173: require io-read for size probes.
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                       "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto& path = ev.mutable_string_at(idx);
        if (file_path_is_denied(path))
            return make_int(0);
        // Issue #3835: Restricted+MT / Strict tenant root (mirror write-file).
        std::string resolved;
        if (!ev.check_tenant_host_path(path, resolved, "file-size"))
            return make_int(0);
        if (!file_is_regular(resolved))
            return make_int(0);
        struct stat st{};
        if (::lstat(resolved.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return make_int(0);
        return make_int(static_cast<std::int64_t>(st.st_size));
    });
}

// Issue #5033: shell / command-output — Exec-gated child processes.
void RegSplitParts::register_exec_primitives(PrimRegistrar add, Evaluator& ev) {
    ev.defer_std_host_prim("shell", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Issue #3836: force shell through require_effect(Exec) so Restricted
        // sandbox runs fiber-principal / isolation / live-mid (git-commit #2072
        // sibling). String-cap deny_exec Soft/Off (!sandbox_mode()) short-circuit
        // stays contract; effect choke only when sandbox on / production face.
        using aura::compiler::security::kEffectExec;
        if (!ev.require_effect(kEffectExec, "shell")) {
            return make_primitive_error(ev.string_heap_, ev.error_values_,
                                        aura::compiler::security::format_deny_reason(
                                            kEffectExec, ev.capability_tenant_id(), "shell"),
                                        ev.primitive_error_counter_ptr());
        }
        if (auto denied = file_deny_exec(ev, "capability denied: exec required"); is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(-1);
        // Issue #4559: Exec pays for the spawn, not for reading another
        // principal's stamped string into argv — in-range before the jail.
        if (!ev.inrange_string_tenant_allows(idx, kEffectExec, "shell"))
            return make_int(-1);
        // Issue #4233: Restricted+MT / Strict tenant FS jail — absolute /
        // ".." / expansion-escape command tokens deny with IsolationDeny
        // tenant-path-escape + zero exec (the #3802 write-file fence on the
        // exec face); on allow the exec child chdir's under the caller's
        // tenant root (relative-only contract).
        std::string jail_root;
        if (!ev.check_tenant_exec_jail(ev.string_at(idx), jail_root, "shell"))
            return make_int(-1);
        // Issue #1582: fork+exec via /bin/sh -c, then WEXITSTATUS.
        // ::system() returns raw waitpid status (exit_code << 8 + signal),
        // which made `sh` return 256/1280/32512 for exits 1/5/127 instead
        // of the actual exit code. sh-ok? was masked (only checks == 0),
        // but `sh` was unusable for exit-code arithmetic. Mirror the
        // fork+execvp pattern from evaluator_primitives_io.cpp:381-413
        // (git-commit, #473) for proper exit-code extraction.
        int jail_sv[2] = {-1, -1};
        if (!jail_root.empty() &&
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, jail_sv) != 0)
            return make_int(-1);
        pid_t pid = ::fork();
        if (pid == 0) {
            if (jail_sv[0] >= 0)
                ::close(jail_sv[0]);
            // Issue #4233: jail cwd before exec — surviving relative-only
            // commands resolve inside the tenant root.
            if (!jail_root.empty() && ::chdir(jail_root.c_str()) != 0)
                ::_exit(127);
            // Issue #4413: Landlock + stat emulation after cwd, before exec.
            if (!jail_root.empty() && !::aura::compiler::security::apply_tenant_exec_fs_jail(
                                          jail_root.c_str(), jail_sv[1]))
                ::_exit(127);
            // Issue #4643: the jail does not revoke an fd the child already
            // holds, so the exec child kept the caller's stdin/stdout/
            // stderr — the shared host stdio of every other tenant. A
            // scanner-clean `cat` read the host stdin and `echo x >&2` wrote
            // the host terminal. `shell` returns the exit code, so the child
            // must not read the host stdin nor write the host terminal: point
            // all three at /dev/null after the jail, before exec.
            if (!jail_root.empty() && !::aura::compiler::security::null_child_stdio_all())
                ::_exit(127);
            ::execl("/bin/sh", "sh", "-c", ev.string_at(idx).c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (jail_sv[1] >= 0)
            ::close(jail_sv[1]);
        if (pid > 0) {
            if (!jail_root.empty()) {
                const int code = ::aura::compiler::security::tenant_exec_jail_reap(
                    pid, jail_sv[0], /*stdout_fd=*/-1, nullptr, jail_root.c_str());
                return make_int(code >= 0 ? static_cast<std::int64_t>(code)
                                          : static_cast<std::int64_t>(-1));
            }
            int status = 0;
            if (::waitpid(pid, &status, 0) != -1 && WIFEXITED(status))
                return make_int(static_cast<std::int64_t>(WEXITSTATUS(status)));
        }
        if (jail_sv[0] >= 0)
            ::close(jail_sv[0]);
        return make_int(-1);
    });
    ev.defer_std_host_prim("command-output", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Issue #3836: require_effect(Exec) before popen — same dual-track
        // close as shell (string-cap alone skipped isolation / live mid).
        using aura::compiler::security::kEffectExec;
        if (!ev.require_effect(kEffectExec, "command-output")) {
            return make_primitive_error(
                ev.string_heap_, ev.error_values_,
                aura::compiler::security::format_deny_reason(kEffectExec, ev.capability_tenant_id(),
                                                             "command-output"),
                ev.primitive_error_counter_ptr());
        }
        if (auto denied = file_deny_exec(ev, "capability denied: exec required"); is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        // Issue #4559: same data fence as shell — in-range before the
        // command bytes reach argv.
        if (!ev.inrange_string_tenant_allows(idx, kEffectExec, "command-output"))
            return make_void();
        auto& cmd = ev.mutable_string_at(idx);
        // Issue #4233: Restricted+MT / Strict tenant FS jail — same
        // fence as shell above (IsolationDeny tenant-path-escape, zero
        // exec). popen gives no pre-exec hook for the child chdir, so
        // capture runs through the shared fork+pipe pattern (#473
        // sibling) and the child is jailed under the caller's tenant
        // root before execl; stdout capture + trailing-newline trim
        // stay contract.
        std::string jail_root;
        if (!ev.check_tenant_exec_jail(cmd, jail_root, "command-output"))
            return make_void();
        int pfd[2];
        if (::pipe(pfd) != 0)
            return make_void();
        int jail_sv[2] = {-1, -1};
        if (!jail_root.empty() &&
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, jail_sv) != 0) {
            ::close(pfd[0]);
            ::close(pfd[1]);
            return make_void();
        }
        std::string result;
        pid_t pid = ::fork();
        if (pid < 0) {
            ::close(pfd[0]);
            ::close(pfd[1]);
            if (jail_sv[0] >= 0)
                ::close(jail_sv[0]);
            if (jail_sv[1] >= 0)
                ::close(jail_sv[1]);
            return make_void();
        }
        if (pid == 0) {
            if (jail_sv[0] >= 0)
                ::close(jail_sv[0]);
            // Issue #4233: jail cwd before exec — surviving relative-only
            // commands resolve inside the tenant root.
            if (!jail_root.empty() && ::chdir(jail_root.c_str()) != 0)
                ::_exit(127);
            ::close(pfd[0]);
            ::dup2(pfd[1], STDOUT_FILENO);
            ::close(pfd[1]);
            // Issue #4413: Landlock + stat emulation after the stdout pipe.
            if (!jail_root.empty() && !::aura::compiler::security::apply_tenant_exec_fs_jail(
                                          jail_root.c_str(), jail_sv[1]))
                ::_exit(127);
            // Issue #4643: the capture pipe stays on stdout; stdin and
            // stderr go to /dev/null. The jail does not revoke an fd the
            // child already holds, so a scanner-clean `cat` read the
            // host stdin and the child wrote the host stderr — the
            // shared host stdio of every other tenant.
            if (!jail_root.empty() && !::aura::compiler::security::null_child_stdio_keep_stdout())
                ::_exit(127);
            ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        ::close(pfd[1]);
        if (jail_sv[1] >= 0)
            ::close(jail_sv[1]);
        if (!jail_root.empty()) {
            (void)::aura::compiler::security::tenant_exec_jail_reap(pid, jail_sv[0], pfd[0],
                                                                    &result, jail_root.c_str());
        } else {
            std::array<char, 4096> buf;
            for (ssize_t n; (n = ::read(pfd[0], buf.data(), buf.size())) > 0;)
                result.append(buf.data(), static_cast<std::size_t>(n));
            ::close(pfd[0]);
            int status = 0;
            ::waitpid(pid, &status, 0);
        }
        if (!result.empty() && result.back() == '\n')
            result.pop_back();
        auto sid = ev.string_heap_.mint(std::move(result));
        return make_string(sid);
    });
}

// Issue #5033: directory-list — io-read directory enumeration.
void RegSplitParts::register_directory_list_primitive(PrimRegistrar add, Evaluator& ev) {
    ev.defer_std_host_prim("directory-list", [&ev](std::span<const EvalValue> a) {
        // Issue #1162: directory listing is io-read (info disclosure).
        if (auto denied = file_deny_io(ev, aura::compiler::security::kCapIoRead,
                                       "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& dir_path = ev.mutable_string_at(idx);
        if (file_path_is_denied(dir_path))
            return make_void();
        // Issue #3835: Restricted+MT / Strict tenant root (mirror
        // write-file).
        std::string resolved;
        if (!ev.check_tenant_host_path(dir_path, resolved, "directory-list"))
            return make_void();
        EvalValue result = make_void();
        auto dir = opendir(resolved.c_str());
        if (!dir)
            return make_void();
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..")
                continue;
            auto sid = ev.string_heap_.mint(name);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sid), result});
            result = make_pair(pid);
        }
        closedir(dir);
        return result;
    });
}

} // namespace aura::compiler::primitives_detail