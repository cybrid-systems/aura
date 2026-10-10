// tenant_exec_fs_jail.hh — Issue #4413: filesystem jail for the exec child.
//
// chdir under the tenant root is not a jail. A scanner-clean command
// (python chr(47), a planted symlink) still has the host mount namespace.
// When the #4233 policy is active the child:
//   1. Landlock-restricts filesystem open/read/write/create/remove/refer/
//      truncate/execute to the tenant root, plus read+execute on the
//      interpreter trees (/usr /bin /lib /lib64 /sbin) so a relative
//      command can start. /etc, /proc, /tmp, and sibling tenants are not
//      granted. /dev/null is the one device node granted (read/write),
//      because git opens it itself after exec. Landlock does not revoke
//      fds already open, so pipes and the stderr /dev/null dup are wired
//      before restrict.
//   2. Emulates stat/lstat/newfstatat/statx in the parent. Landlock's own
//      uapi does not restrict stat(2). The notifier never uses
//      SECCOMP_USER_NOTIF_FLAG_CONTINUE — the kernel performs no stat on
//      the child's pointer. The parent stats only a path it has resolved
//      under the tenant root, an interpreter tree, or /dev/null, and
//      copies that buffer back.
//
// No second capability and no new SecurityEvent reason. A kernel without
// Landlock ABI 1 or without a stat notifier fails closed through the
// existing IsolationDeny tenant-path-escape row (kEffectExec) before
// fork. A missing tenant directory is not that deny: the child chdir
// fails and never execs (#4233 AC3).

#ifndef AURA_COMPILER_TENANT_EXEC_FS_JAIL_HH
#define AURA_COMPILER_TENANT_EXEC_FS_JAIL_HH

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace aura::compiler::security {

enum class TenantExecFsJailPreflight : std::uint8_t {
    Ok = 0,     // directory is a real directory and a ruleset accepts it
    Absent = 1, // ENOENT — caller still returns the root; child chdir fails
    Deny = 2,   // no jail mechanism, symlink, or the kernel rejected the rule
};

namespace tenant_exec_detail {

    inline constexpr const char* kInterpRoots[] = {"/usr", "/bin", "/lib", "/lib64", "/sbin"};

    [[nodiscard]] inline int landlock_abi() noexcept {
        const long v = ::syscall(SYS_landlock_create_ruleset, nullptr, static_cast<std::size_t>(0),
                                 LANDLOCK_CREATE_RULESET_VERSION);
        if (v < 1 || v > 64)
            return 0;
        return static_cast<int>(v);
    }

    [[nodiscard]] inline unsigned long long handled_access(int abi) noexcept {
        unsigned long long h = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
                               LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
                               LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
                               LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                               LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
                               LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
                               LANDLOCK_ACCESS_FS_MAKE_SYM;
        if (abi >= 2)
            h |= LANDLOCK_ACCESS_FS_REFER;
        if (abi >= 3)
            h |= LANDLOCK_ACCESS_FS_TRUNCATE;
        if (abi >= 5)
            h |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
        return h;
    }

    [[nodiscard]] inline unsigned long long jail_access(int abi) noexcept {
        // Device nodes stay denied inside the tenant root. MAKE_SOCK stays so a
        // unix socket under the root still works.
        return handled_access(abi) &
               ~(static_cast<unsigned long long>(LANDLOCK_ACCESS_FS_MAKE_CHAR) |
                 static_cast<unsigned long long>(LANDLOCK_ACCESS_FS_MAKE_BLOCK));
    }

    [[nodiscard]] inline bool stat_notifier_available() noexcept {
        __u32 action = SECCOMP_RET_USER_NOTIF;
        if (::syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) != 0)
            return false;
        seccomp_notif_sizes sizes{};
        if (::syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) != 0)
            return false;
        return sizes.seccomp_notif == sizeof(seccomp_notif) &&
               sizes.seccomp_notif_resp == sizeof(seccomp_notif_resp);
    }

    [[nodiscard]] inline std::uint32_t audit_arch() noexcept {
#if defined(__aarch64__)
        return AUDIT_ARCH_AARCH64;
#elif defined(__x86_64__)
        return AUDIT_ARCH_X86_64;
#else
        return 0;
#endif
    }

    [[nodiscard]] inline bool path_under(const std::string& path, const std::string& root) {
        if (root.empty() || path.empty())
            return false;
        if (path == root)
            return true;
        return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
               path[root.size()] == '/';
    }

    [[nodiscard]] inline bool path_allowed(const std::string& path, const std::string& jail_a,
                                           const std::string& jail_b) {
        if (path_under(path, jail_a) || path_under(path, jail_b))
            return true;
        for (const char* root : kInterpRoots) {
            if (path_under(path, root))
                return true;
        }
        // git opens the null device itself. The inode is not a tenant path
        // and not an interpreter tree; allow this one file, not /dev.
        return path == "/dev/null";
    }

    [[nodiscard]] inline std::string lexical_absolute(std::string path) {
        std::vector<std::string> parts;
        std::string cur;
        auto flush = [&]() {
            if (cur.empty() || cur == ".") {
                cur.clear();
                return;
            }
            if (cur == "..") {
                if (!parts.empty())
                    parts.pop_back();
                cur.clear();
                return;
            }
            parts.push_back(std::move(cur));
            cur.clear();
        };
        for (char c : path) {
            if (c == '/')
                flush();
            else
                cur.push_back(c);
        }
        flush();
        std::string out = "/";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i != 0)
                out.push_back('/');
            out += parts[i];
        }
        return out;
    }

    [[nodiscard]] inline std::string readlink_fd(int fd) {
        char buf[4096];
        const std::string link = "/proc/self/fd/" + std::to_string(fd);
        const ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
        if (n < 0 || n >= static_cast<ssize_t>(sizeof buf - 1))
            return {};
        buf[n] = '\0';
        std::string s(buf, static_cast<std::size_t>(n));
        const std::string deleted = " (deleted)";
        if (s.size() > deleted.size() &&
            s.compare(s.size() - deleted.size(), deleted.size(), deleted) == 0)
            s.resize(s.size() - deleted.size());
        return s;
    }

    [[nodiscard]] inline std::string canonical_dir(const char* path) {
        const int fd = ::open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0)
            return path ? std::string(path) : std::string();
        std::string link = readlink_fd(fd);
        ::close(fd);
        if (link.empty())
            return path ? std::string(path) : std::string();
        return link;
    }

    [[nodiscard]] inline bool read_cstr(pid_t pid, std::uint64_t addr, std::string& out) {
        out.clear();
        if (addr == 0)
            return false;
        char buf[256];
        for (int off = 0; off < 4096; off += static_cast<int>(sizeof buf)) {
            iovec local{};
            local.iov_base = buf;
            local.iov_len = sizeof buf;
            iovec remote{};
            remote.iov_base = reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr) +
                                                      static_cast<std::uintptr_t>(off));
            remote.iov_len = sizeof buf;
            const ssize_t n = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
            if (n <= 0)
                return false;
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\0')
                    return true;
                out.push_back(buf[i]);
            }
        }
        return false;
    }

    inline void notif_reply(int nfd, std::uint64_t id, int neg_errno) noexcept {
        seccomp_notif_resp resp{};
        resp.id = id;
        resp.error = neg_errno < 0 ? neg_errno : 0;
        resp.val = 0;
        resp.flags = 0;
        (void)::ioctl(nfd, SECCOMP_IOCTL_NOTIF_SEND, &resp);
    }

    inline bool write_remote(pid_t pid, std::uint64_t addr, const void* src,
                             std::size_t n) noexcept {
        iovec local{};
        local.iov_base = const_cast<void*>(src);
        local.iov_len = n;
        iovec remote{};
        remote.iov_base = reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr));
        remote.iov_len = n;
        return ::process_vm_writev(pid, &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
    }

    inline bool add_landlock_path(int ruleset, const char* path, unsigned long long access,
                                  bool nofollow) noexcept {
        const int flags = O_PATH | O_CLOEXEC | (nofollow ? O_NOFOLLOW : 0);
        const int fd = ::open(path, flags);
        if (fd < 0)
            return false;
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            ::close(fd);
            return false;
        }
        landlock_path_beneath_attr beneath{};
        beneath.allowed_access = access;
        beneath.parent_fd = fd;
        const long rc =
            ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0);
        ::close(fd);
        return rc == 0;
    }

    // A single file or device node (not a directory). /dev/null is a char
    // device; directory bits on that rule are rejected by the kernel.
    inline bool add_landlock_file(int ruleset, const char* path,
                                  unsigned long long access) noexcept {
        const int fd = ::open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return false;
        struct stat st{};
        if (::fstat(fd, &st) != 0 || (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode))) {
            ::close(fd);
            return false;
        }
        landlock_path_beneath_attr beneath{};
        beneath.allowed_access = access;
        beneath.parent_fd = fd;
        const long rc =
            ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0);
        ::close(fd);
        return rc == 0;
    }

    [[nodiscard]] inline int install_stat_filter() noexcept {
        const std::uint32_t arch = audit_arch();
        if (arch == 0)
            return -1;
        std::uint32_t nrs[4];
        int nn = 0;
        nrs[nn++] = __NR_statx;
        nrs[nn++] = __NR_newfstatat;
#if defined(__NR_stat)
        nrs[nn++] = __NR_stat;
#endif
#if defined(__NR_lstat)
        nrs[nn++] = __NR_lstat;
#endif
        sock_filter filter[16]{};
        unsigned short n = 0;
        filter[n++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
        filter[n++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0);
        filter[n++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
        filter[n++] = BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
        for (int i = 0; i < nn; ++i) {
            const auto skip = static_cast<std::uint8_t>(nn - i);
            filter[n++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nrs[i], skip, 0);
        }
        filter[n++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
        filter[n++] = BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF);
        sock_fprog prog{};
        prog.len = n;
        prog.filter = filter;
        return static_cast<int>(::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
                                          SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog));
    }

    inline bool send_fd(int sock, int fd) noexcept {
        char byte = 'F';
        iovec iov{};
        iov.iov_base = &byte;
        iov.iov_len = 1;
        char cbuf[CMSG_SPACE(sizeof(int))];
        std::memset(cbuf, 0, sizeof cbuf);
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof cbuf;
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg == nullptr)
            return false;
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
        return ::sendmsg(sock, &msg, 0) == 1;
    }

    inline void scrub_child_env(const char* jail_root) noexcept {
        static char path_env[] =
            "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
        static char home_env[4100];
        static char tmp_env[4100];
        static char* envp[4];
        auto put = [](char* dst, std::size_t cap, const char* prefix, const char* value) -> bool {
            std::size_t i = 0;
            for (const char* p = prefix; *p != '\0'; ++p) {
                if (i + 1 >= cap)
                    return false;
                dst[i++] = *p;
            }
            for (const char* p = value; *p != '\0'; ++p) {
                if (i + 1 >= cap)
                    return false;
                dst[i++] = *p;
            }
            dst[i] = '\0';
            return true;
        };
        if (!put(home_env, sizeof home_env, "HOME=", jail_root) ||
            !put(tmp_env, sizeof tmp_env, "TMPDIR=", jail_root))
            return;
        envp[0] = path_env;
        envp[1] = home_env;
        envp[2] = tmp_env;
        envp[3] = nullptr;
        // unistd.h declares the process environment at global scope. A
        // block-scope extern here would name a member of this namespace.
        ::environ = envp;
    }

    inline void close_inherited_fds() noexcept {
#if defined(SYS_close_range)
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) == 0)
            return;
#endif
        for (int fd = 3; fd < 1024; ++fd)
            ::close(fd);
    }

    inline int dup_child_fd(pid_t pid, int fd) noexcept {
        const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
        if (pidfd < 0)
            return -1;
        const int duped = static_cast<int>(::syscall(SYS_pidfd_getfd, pidfd, fd, 0));
        ::close(pidfd);
        return duped;
    }

    inline void copy_stat_result(int nfd, const seccomp_notif& req, int src, bool is_statx,
                                 unsigned int mask, std::uint64_t stat_ptr) noexcept {
        struct stat st{};
        struct statx sx{};
        const void* blob = &st;
        std::size_t blob_n = sizeof st;
        long rc;
        if (is_statx) {
            rc = ::syscall(SYS_statx, src, "", AT_EMPTY_PATH, mask, &sx);
            blob = &sx;
            blob_n = sizeof sx;
        } else {
            rc = ::fstat(src, &st);
        }
        if (rc != 0) {
            notif_reply(nfd, req.id, -errno);
            return;
        }
        if (!write_remote(static_cast<pid_t>(req.pid), stat_ptr, blob, blob_n)) {
            notif_reply(nfd, req.id, -EFAULT);
            return;
        }
        notif_reply(nfd, req.id, 0);
    }

    inline void handle_stat(int nfd, const seccomp_notif& req, const std::string& jail_a,
                            const std::string& jail_b) {
        __u64 id = req.id;
        if (::ioctl(nfd, SECCOMP_IOCTL_NOTIF_ID_VALID, &id) != 0)
            return;
        const bool is_statx = req.data.nr == __NR_statx;
        bool old_stat = false;
        bool old_lstat = false;
#if defined(__NR_stat)
        old_stat = req.data.nr == __NR_stat;
#endif
#if defined(__NR_lstat)
        old_lstat = req.data.nr == __NR_lstat;
#endif
        int dirfd = AT_FDCWD;
        std::uint64_t path_ptr = 0;
        std::uint64_t stat_ptr = 0;
        int flags = 0;
        unsigned int mask = STATX_BASIC_STATS;
        if (is_statx) {
            dirfd = static_cast<int>(req.data.args[0]);
            path_ptr = req.data.args[1];
            flags = static_cast<int>(req.data.args[2]);
            mask = static_cast<unsigned int>(req.data.args[3]);
            stat_ptr = req.data.args[4];
        } else if (old_stat || old_lstat) {
            path_ptr = req.data.args[0];
            stat_ptr = req.data.args[1];
            flags = old_lstat ? AT_SYMLINK_NOFOLLOW : 0;
        } else if (req.data.nr == __NR_newfstatat) {
            dirfd = static_cast<int>(req.data.args[0]);
            path_ptr = req.data.args[1];
            stat_ptr = req.data.args[2];
            flags = static_cast<int>(req.data.args[3]);
        } else {
            notif_reply(nfd, req.id, -EPERM);
            return;
        }
        const pid_t pid = static_cast<pid_t>(req.pid);
        std::string path;
        if (path_ptr != 0 && !read_cstr(pid, path_ptr, path)) {
            notif_reply(nfd, req.id, -EFAULT);
            return;
        }
        int duped = -1;
        if (dirfd != AT_FDCWD)
            duped = dup_child_fd(pid, dirfd);
        if (path.empty() && (flags & AT_EMPTY_PATH) != 0) {
            int src = duped;
            int owned = -1;
            if (dirfd == AT_FDCWD) {
                char cwd[4096];
                const std::string link = "/proc/" + std::to_string(pid) + "/cwd";
                const ssize_t n = ::readlink(link.c_str(), cwd, sizeof cwd - 1);
                if (n < 0) {
                    notif_reply(nfd, req.id, -EACCES);
                    return;
                }
                cwd[n] = '\0';
                const std::string cwd_s(cwd, static_cast<std::size_t>(n));
                if (!path_allowed(cwd_s, jail_a, jail_b)) {
                    notif_reply(nfd, req.id, -EACCES);
                    return;
                }
                owned = ::open(cwd, O_PATH | O_DIRECTORY | O_CLOEXEC);
                src = owned;
            }
            if (src < 0) {
                notif_reply(nfd, req.id, -EACCES);
                return;
            }
            copy_stat_result(nfd, req, src, is_statx, mask, stat_ptr);
            if (owned >= 0)
                ::close(owned);
            if (duped >= 0)
                ::close(duped);
            return;
        }
        if (path.empty()) {
            if (duped >= 0)
                ::close(duped);
            notif_reply(nfd, req.id, -ENOENT);
            return;
        }
        std::string base;
        if (dirfd == AT_FDCWD) {
            char cwd[4096];
            const std::string link = "/proc/" + std::to_string(pid) + "/cwd";
            const ssize_t n = ::readlink(link.c_str(), cwd, sizeof cwd - 1);
            if (n < 0) {
                notif_reply(nfd, req.id, -EACCES);
                return;
            }
            cwd[n] = '\0';
            base.assign(cwd, static_cast<std::size_t>(n));
        } else {
            base = duped >= 0 ? readlink_fd(duped) : std::string();
            if (duped >= 0)
                ::close(duped);
            duped = -1;
            if (base.empty() || !path_allowed(base, jail_a, jail_b)) {
                notif_reply(nfd, req.id, -EACCES);
                return;
            }
        }
        std::string abs = path;
        if (path[0] != '/') {
            if (base.empty() || base.back() != '/')
                base.push_back('/');
            abs = base + path;
        }
        const std::string norm = lexical_absolute(std::move(abs));
        if (!path_allowed(norm, jail_a, jail_b)) {
            notif_reply(nfd, req.id, -EACCES);
            return;
        }
        int oflags = O_PATH | O_CLOEXEC;
        if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
            oflags |= O_NOFOLLOW;
        const int fd = ::open(norm.c_str(), oflags);
        if (fd < 0) {
            notif_reply(nfd, req.id, -errno);
            return;
        }
        const std::string resolved = readlink_fd(fd);
        if (!path_allowed(resolved, jail_a, jail_b)) {
            ::close(fd);
            notif_reply(nfd, req.id, -EACCES);
            return;
        }
        copy_stat_result(nfd, req, fd, is_statx, mask, stat_ptr);
        ::close(fd);
    }

} // namespace tenant_exec_detail

// True when this kernel can both Landlock-restrict the child and emulate
// stat in the parent. False fails the exec closed (no fork).
[[nodiscard]] inline bool tenant_exec_fs_jail_available() noexcept {
    using namespace tenant_exec_detail;
    return landlock_abi() >= 1 && stat_notifier_available() && audit_arch() != 0;
}

// Parent-side probe. Does not restrict the caller and does not create the
// directory. ENOENT is Absent so #4233 AC3 can still hand the root to a
// child that then fails chdir. A symlink final component is Deny (ELOOP
// from O_NOFOLLOW without O_PATH).
[[nodiscard]] inline TenantExecFsJailPreflight
tenant_exec_fs_jail_preflight(const char* jail_root) noexcept {
    using namespace tenant_exec_detail;
    if (jail_root == nullptr || jail_root[0] == '\0')
        return TenantExecFsJailPreflight::Deny;
    const int abi = landlock_abi();
    if (abi < 1 || !stat_notifier_available() || audit_arch() == 0)
        return TenantExecFsJailPreflight::Deny;
    const int fd = ::open(jail_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? TenantExecFsJailPreflight::Absent
                               : TenantExecFsJailPreflight::Deny;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ::close(fd);
        return TenantExecFsJailPreflight::Deny;
    }
    landlock_ruleset_attr attr{};
    attr.handled_access_fs = handled_access(abi);
    const int ruleset = static_cast<int>(
        ::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr.handled_access_fs), 0));
    if (ruleset < 0) {
        ::close(fd);
        return TenantExecFsJailPreflight::Deny;
    }
    landlock_path_beneath_attr beneath{};
    beneath.allowed_access = jail_access(abi);
    beneath.parent_fd = fd;
    const long rc =
        ::syscall(SYS_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &beneath, 0);
    ::close(fd);
    ::close(ruleset);
    return rc == 0 ? TenantExecFsJailPreflight::Ok : TenantExecFsJailPreflight::Deny;
}

// Child only, after chdir and after stdout / stderr have been wired.
// Installs Landlock, replaces the environment, installs the stat filter,
// and sends the notifier fd on `sock`. Closes `sock`. false → _exit(127)
// without exec.
[[nodiscard]] inline bool apply_tenant_exec_fs_jail(const char* jail_root, int sock) noexcept {
    using namespace tenant_exec_detail;
    if (jail_root == nullptr || jail_root[0] == '\0' || sock < 0 || std::strlen(jail_root) > 4000) {
        if (sock >= 0)
            ::close(sock);
        return false;
    }
    const int abi = landlock_abi();
    if (abi < 1) {
        ::close(sock);
        return false;
    }
    landlock_ruleset_attr attr{};
    attr.handled_access_fs = handled_access(abi);
    const int ruleset = static_cast<int>(
        ::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr.handled_access_fs), 0));
    if (ruleset < 0) {
        ::close(sock);
        return false;
    }
    if (!add_landlock_path(ruleset, jail_root, jail_access(abi), /*nofollow=*/true)) {
        ::close(ruleset);
        ::close(sock);
        return false;
    }
    const unsigned long long rx =
        LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE;
    for (const char* dir : kInterpRoots)
        (void)add_landlock_path(ruleset, dir, rx, /*nofollow=*/false);
    // git opens /dev/null read-write after exec. The stderr dup done before
    // restrict is a different fd and does not authorize that open.
    const unsigned long long null_rw = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE;
    (void)add_landlock_file(ruleset, "/dev/null", null_rw);
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        ::syscall(SYS_landlock_restrict_self, ruleset, 0) != 0) {
        ::close(ruleset);
        ::close(sock);
        return false;
    }
    ::close(ruleset);
    const int notify = install_stat_filter();
    if (notify < 0) {
        ::close(sock);
        return false;
    }
    const bool sent = send_fd(sock, notify);
    ::close(notify);
    ::close(sock);
    if (!sent)
        return false;
    scrub_child_env(jail_root);
    close_inherited_fds();
    return true;
}

// Parent side of one jailed child. Receives the stat-notifier fd from
// `sock`, serves stat emulation until `pid` exits, and appends captured
// stdout when `stdout_fd >= 0`. Closes `sock` and `stdout_fd`. Returns the
// child's exit code, or -1 when it did not exit normally.
inline int tenant_exec_jail_reap(pid_t pid, int sock, int stdout_fd, std::string* out,
                                 const char* jail_root) {
    using namespace tenant_exec_detail;
    const std::string jail_a = jail_root != nullptr ? jail_root : "";
    const std::string jail_b = canonical_dir(jail_root);
    char byte = 0;
    iovec iov{};
    iov.iov_base = &byte;
    iov.iov_len = 1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    std::memset(cbuf, 0, sizeof cbuf);
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    const ssize_t got = sock >= 0 ? ::recvmsg(sock, &msg, 0) : 0;
    if (sock >= 0)
        ::close(sock);
    int notify = -1;
    if (got > 0) {
        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
                std::memcpy(&notify, CMSG_DATA(cmsg), sizeof notify);
                break;
            }
        }
    }
    if (notify < 0) {
        int status = 0;
        const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped == 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            if (stdout_fd >= 0)
                ::close(stdout_fd);
            return -1;
        }
        if (stdout_fd >= 0)
            ::close(stdout_fd);
        if (reaped == pid && WIFEXITED(status))
            return WEXITSTATUS(status);
        return -1;
    }
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    bool stdout_open = stdout_fd >= 0;
    int status = 0;
    bool reaped = false;
    for (;;) {
        pollfd pf[3]{};
        nfds_t np = 0;
        int out_i = -1;
        int not_i = -1;
        int pid_i = -1;
        if (stdout_open) {
            out_i = static_cast<int>(np);
            pf[np].fd = stdout_fd;
            pf[np].events = POLLIN;
            ++np;
        }
        if (notify >= 0) {
            not_i = static_cast<int>(np);
            pf[np].fd = notify;
            pf[np].events = POLLIN;
            ++np;
        }
        if (pidfd >= 0) {
            pid_i = static_cast<int>(np);
            pf[np].fd = pidfd;
            pf[np].events = POLLIN;
            ++np;
        }
        const int pr = ::poll(pf, np, pidfd >= 0 ? -1 : 200);
        if (pr < 0 && errno == EINTR)
            continue;
        if (not_i >= 0 && (pf[not_i].revents & POLLIN) != 0) {
            seccomp_notif req{};
            if (::ioctl(notify, SECCOMP_IOCTL_NOTIF_RECV, &req) == 0)
                handle_stat(notify, req, jail_a, jail_b);
        }
        if (out_i >= 0 && (pf[out_i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buf[4096];
            const ssize_t n = ::read(stdout_fd, buf, sizeof buf);
            if (n > 0) {
                if (out != nullptr)
                    out->append(buf, static_cast<std::size_t>(n));
            } else if (n < 0 && errno == EINTR) {
                // keep reading
            } else {
                stdout_open = false;
            }
        }
        if (!reaped) {
            int flags = WNOHANG;
            if (pidfd >= 0 && pid_i >= 0 && (pf[pid_i].revents & POLLIN) != 0)
                flags = 0;
            const pid_t r = ::waitpid(pid, &status, flags);
            if (r == pid)
                reaped = true;
        }
        if (reaped && !stdout_open)
            break;
        if (reaped && stdout_fd < 0)
            break;
    }
    if (stdout_fd >= 0) {
        char buf[4096];
        ssize_t n = 0;
        while (stdout_open && (n = ::read(stdout_fd, buf, sizeof buf)) > 0) {
            if (out != nullptr)
                out->append(buf, static_cast<std::size_t>(n));
        }
        ::close(stdout_fd);
    }
    if (notify >= 0)
        ::close(notify);
    if (pidfd >= 0)
        ::close(pidfd);
    if (!reaped) {
        if (::waitpid(pid, &status, 0) != pid)
            return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

} // namespace aura::compiler::security

#endif
