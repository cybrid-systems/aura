// evaluator_primitives_file.cpp — P0 step 25: read / read-file / write-file / file-* / shell /
// directory-list aura.compiler.evaluator module partition; registered via
// evaluator_primitives_registry.cpp.

module;

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "runtime_shared.h"
#include "security_capabilities.h"

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

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

    const auto deny_io = [&ev](std::string_view cap, std::string_view msg) -> EvalValue {
        if (!ev.sandbox_mode() || ev.has_capability(cap) ||
            ev.has_capability(aura::compiler::security::kCapIo) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return make_void();
        ev.bump_capability_denial();
        return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    };
    const auto deny_exec = [&ev](std::string_view msg) -> EvalValue {
        if (!ev.sandbox_mode() || ev.has_capability(aura::compiler::security::kCapExec) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return make_void();
        ev.bump_capability_denial();
        return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    };

    add("read", [&ev](const auto&) {
        std::string line;
        std::getline(std::cin, line);
        if (line.empty())
            return make_void();
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(line));
        return make_string(id);
    });

    // Helper: check path is a regular file (skip directories). Uses lstat so
    // symlink targets are not followed for the type check (#1171 TOCTOU family).
    auto is_regular = [](const std::string& path) -> bool {
        struct stat st;
        return ::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    };
    // Issue #1163/#1164/#1165: refuse dangerous / sensitive paths.
    auto path_is_denied = [](std::string_view path) -> bool {
        if (path.empty())
            return true;
        // Absolute sensitive prefixes
        static constexpr const char* kDenied[] = {
            "/proc/self/mem", "/proc/self/mem/", "/dev/mem", "/dev/kmem", "/proc/kcore",
        };
        for (auto* d : kDenied) {
            if (path == d || path.starts_with(std::string(d) + "/"))
                return true;
        }
        // Any /proc/*/mem style
        if (path.starts_with("/proc/") &&
            (path.ends_with("/mem") || path.find("/mem/") != std::string_view::npos))
            return true;
        return false;
    };

    add("read-file", [&ev, is_regular, path_is_denied, deny_io](const auto& a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& path = ev.string_heap_[idx];
        // Issue #1163/#1171: deny sensitive paths; open with O_NOFOLLOW to
        // collapse lstat+open TOCTOU (symlink swap between check and open).
        if (path_is_denied(path))
            return make_void();
        int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return make_void();
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            return make_void();
        }
        FILE* fp = ::fdopen(fd, "r");
        if (!fp) {
            ::close(fd);
            return make_void();
        }
        std::string content;
        char buf[4096];
        while (std::size_t n = ::fread(buf, 1, sizeof(buf), fp))
            content.append(buf, n);
        ::fclose(fp); // also closes fd
        auto id = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(content));
        return make_string(id);
    });

    add("write-file", [&ev, path_is_denied, deny_io](std::span<const EvalValue> a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoWrite,
                                  "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.size() < 2 || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& path = ev.string_heap_[idx];
        // Issue #1163: refuse /proc/self/mem and other process-memory paths.
        if (path_is_denied(path))
            return make_void();
        std::string content;
        if (is_string(a[1])) {
            auto cidx = as_string_idx(a[1]);
            if (cidx < ev.string_heap_.size())
                content = ev.string_heap_[cidx];
        } else if (is_int(a[1])) {
            content = std::to_string(as_int(a[1]));
        } else {
            return make_void();
        }
        // O_NOFOLLOW | O_CREAT | O_WRONLY — no symlink follow to sensitive targets.
        int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd < 0)
            return make_void();
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            ::close(fd);
            return make_void();
        }
        const char* p = content.data();
        std::size_t left = content.size();
        while (left > 0) {
            auto n = ::write(fd, p, left);
            if (n <= 0) {
                ::close(fd);
                return make_void();
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        ::close(fd);
        return make_int(1);
    });

    add("command-line", [&ev](const auto&) -> EvalValue {
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
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(*it);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), result});
            result = make_pair(pid);
        }
        return result;
    });

    add("file-exists?", [&ev, path_is_denied, deny_io](std::span<const EvalValue> a) {
        // Issue #1172: require io-read for existence probes (recon).
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto& path = ev.string_heap_[idx];
        if (path_is_denied(path))
            return make_int(0);
        struct stat st;
        return make_int(::lstat(path.c_str(), &st) == 0 ? 1 : 0);
    });

    add("file-copy", [&ev, is_regular, path_is_denied, deny_io](const auto& a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoWrite,
                                  "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.size() < 2 || !is_string(a[0]) || !is_string(a[1]))
            return make_void();
        auto sidx = as_string_idx(a[0]), didx = as_string_idx(a[1]);
        if (sidx >= ev.string_heap_.size() || didx >= ev.string_heap_.size())
            return make_void();
        auto& src_path = ev.string_heap_[sidx];
        auto& dst_path = ev.string_heap_[didx];
        // Issue #1165: validate both source and destination paths.
        if (path_is_denied(src_path) || path_is_denied(dst_path))
            return make_void();
        if (!is_regular(src_path))
            return make_void();
        int sfd = ::open(src_path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (sfd < 0)
            return make_void();
        int dfd =
            ::open(dst_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (dfd < 0) {
            ::close(sfd);
            return make_void();
        }
        char buf[8192];
        while (true) {
            auto n = ::read(sfd, buf, sizeof(buf));
            if (n < 0) {
                ::close(sfd);
                ::close(dfd);
                return make_void();
            }
            if (n == 0)
                break;
            auto left = static_cast<std::size_t>(n);
            const char* p = buf;
            while (left > 0) {
                auto w = ::write(dfd, p, left);
                if (w <= 0) {
                    ::close(sfd);
                    ::close(dfd);
                    return make_void();
                }
                p += w;
                left -= static_cast<std::size_t>(w);
            }
        }
        ::close(sfd);
        ::close(dfd);
        return make_int(1);
    });

    add("file-delete", [&ev, path_is_denied, deny_io](std::span<const EvalValue> a) {
        if (auto denied = deny_io(aura::compiler::security::kCapIoWrite,
                                  "capability denied: io-write required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto& path = ev.string_heap_[idx];
        // Issue #1164: deny sensitive paths; refuse directories (no rmdir cascade);
        // use unlinkat AT_FDCWD without following final symlink where possible.
        if (path_is_denied(path))
            return make_int(0);
        struct stat st{};
        if (::lstat(path.c_str(), &st) != 0)
            return make_int(0);
        if (S_ISDIR(st.st_mode))
            return make_int(0); // directories require explicit rmdir API
        return make_int(::unlink(path.c_str()) == 0 ? 1 : 0);
    });

    add("file-size", [&ev, is_regular, path_is_denied, deny_io](const auto& a) {
        // Issue #1173: require io-read for size probes.
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(0);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(0);
        auto& path = ev.string_heap_[idx];
        if (path_is_denied(path) || !is_regular(path))
            return make_int(0);
        struct stat st{};
        if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return make_int(0);
        return make_int(static_cast<std::int64_t>(st.st_size));
    });

    add("shell", [&ev, deny_exec](std::span<const EvalValue> a) -> EvalValue {
        if (auto denied = deny_exec("capability denied: exec required"); is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_int(-1);
        // Issue #1582: fork+exec via /bin/sh -c, then WEXITSTATUS.
        // ::system() returns raw waitpid status (exit_code << 8 + signal),
        // which made `sh` return 256/1280/32512 for exits 1/5/127 instead
        // of the actual exit code. sh-ok? was masked (only checks == 0),
        // but `sh` was unusable for exit-code arithmetic. Mirror the
        // fork+execvp pattern from evaluator_primitives_io.cpp:381-413
        // (git-commit, #473) for proper exit-code extraction.
        pid_t pid = ::fork();
        if (pid == 0) {
            ::execl("/bin/sh", "sh", "-c", ev.string_heap_[idx].c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (pid > 0) {
            int status = 0;
            if (::waitpid(pid, &status, 0) != -1 && WIFEXITED(status))
                return make_int(static_cast<std::int64_t>(WEXITSTATUS(status)));
        }
        return make_int(-1);
    });

    add("command-output", [&ev, deny_exec](std::span<const EvalValue> a) -> EvalValue {
        if (auto denied = deny_exec("capability denied: exec required"); is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& cmd = ev.string_heap_[idx];
        std::array<char, 4096> buf;
        std::string result;
        auto* fp = ::popen(cmd.c_str(), "r");
        if (!fp)
            return make_void();
        while (::fgets(buf.data(), buf.size(), fp) != nullptr)
            result += buf.data();
        ::pclose(fp);
        if (!result.empty() && result.back() == '\n')
            result.pop_back();
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    add("directory-list", [&ev, path_is_denied, deny_io](std::span<const EvalValue> a) {
        // Issue #1162: directory listing is io-read (info disclosure).
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto idx = as_string_idx(a[0]);
        if (idx >= ev.string_heap_.size())
            return make_void();
        auto& dir_path = ev.string_heap_[idx];
        if (path_is_denied(dir_path))
            return make_void();
        EvalValue result = make_void();
        auto dir = opendir(dir_path.c_str());
        if (!dir)
            return make_void();
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..")
                continue;
            auto sid = ev.string_heap_.size();
            ev.string_heap_.push_back(name);
            auto pid = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sid), result});
            result = make_pair(pid);
        }
        closedir(dir);
        return result;
    });
}

} // namespace aura::compiler::primitives_detail