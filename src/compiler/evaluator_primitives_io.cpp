// evaluator_primitives_io.cpp — P0 step 17: git:* and network primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.
//
// Issue #1970: git-* is a deferred integration vertical (DOMAIN_STATUS deferred).
// Registration of the 7 git-* names is gated by AURA_ENABLE_GIT (CMake option,
// default ON). Independent of AURA_HAVE_LIBGIT2 (in-process backend vs popen).
// Issue #1975: tcp-* client names gated by AURA_ENABLE_TCP in
// register_network_primitives. Issue #2771: +tcp-listen / tcp-accept /
// tcp-accept-timeout / tcp-local-port (server path for multi-host denseness).
// getenv stays always on (registered unconditionally; reads are io-read
// capability-fenced, #4504). http/tcp/sys/git are deferred (#3174) until
// (require "std/net"|"std/socket"|"std/git"|"std/process"). See docs/git-integration.md +
// docs/tcp.md.

module;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "runtime_shared.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "primitives_meta.h"
#include "security_capabilities.h"
#include "tenant_host_path.hh"        // #3802 tenant FS path-prefix
#include "tenant_exec_fs_jail.hh"     // #4413 landlock + stat emulation
#include "core/provenance_tracker.hh" // #3802 MT active
#include "core/sandbox.hh"            // #4561 production fd-owner face
#include "core/arena_auto_policy_stats.h"
#include "core/gap_buffer.hh"
#include "core/lifetime_pin.hh" // g_lifetime_pin_stats() for owner-transition counts
#include "git_ctx.h"
#include "hash_meta.h"
#include "runtime_paths.h" // #2772 resolve_self_executable

// Default ON when the TU is compiled outside the CMake graph (tools/IDE).
#ifndef AURA_ENABLE_GIT
#define AURA_ENABLE_GIT 1
#endif
#ifndef AURA_ENABLE_TCP
#define AURA_ENABLE_TCP 1
#endif

#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define AURA_HAVE_CURL 1
#else
#include "curl_stub.h"
#endif
#include "messaging_bridge.h"
#include "core/ownership.hh" // Issue #4936: NoCopy

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

// Issue #5274: drop an outermost mutation hold before blocking HTTP.
extern "C" int aura_mutation_hold_release_before_block() noexcept;

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

// ── Runtime-loaded libcurl via dlopen (avoids ld.bfd symbol issues) ──
namespace {
    struct CurlAPI : ::aura::core::NoCopy {
        void* handle = nullptr;
        CURL* (*easy_init)() = nullptr;
        CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
        CURLcode (*easy_perform)(CURL*) = nullptr;
        void (*easy_cleanup)(CURL*) = nullptr;
        struct curl_slist* (*slist_append)(struct curl_slist*, const char*) = nullptr;
        void (*slist_free_all)(struct curl_slist*) = nullptr;
        const char* (*easy_strerror)(CURLcode) = nullptr;

        bool load() {
            if (handle)
                return true;
            // Issue #473 §7: multi-platform libcurl soname fallback.
            // Try the already-loaded image first (covers static linking or
            // distributions where libcurl is dlopen'd by another dep),
            // then walk a small list of platform-specific sonames.
            // The order matters: RTLD_DEFAULT walk via dlopen(NULL) on
            // some systems returns null even when symbols exist, so we
            // still probe individual names afterwards.
            static constexpr const char* kSonames[] = {
                "libcurl.so.4",    // Linux + glibc
                "libcurl.so",      // Linux generic
                "libcurl.4.dylib", // macOS Sonoma+ (and Homebrew)
                "libcurl.dylib",   // macOS legacy
                "libcurl.so.5",    // future-proofing (libcurl 8+)
            };
            // Build a small list of probe candidates — start with the
            // global handle, then the named sonames. We attempt each in
            // turn and fully resolve symbols before accepting it.
            std::vector<void*> candidates;
            if (auto* h = ::dlopen(nullptr, RTLD_LAZY | RTLD_LOCAL))
                candidates.push_back(h);
            for (auto* name : kSonames) {
                if (auto* h = ::dlopen(name, RTLD_LAZY | RTLD_LOCAL))
                    candidates.push_back(h);
            }
            for (auto* h : candidates) {
                auto* ei = reinterpret_cast<CURL* (*)()>(::dlsym(h, "curl_easy_init"));
                auto* es = reinterpret_cast<CURLcode (*)(CURL*, CURLoption, ...)>(
                    ::dlsym(h, "curl_easy_setopt"));
                auto* ep = reinterpret_cast<CURLcode (*)(CURL*)>(::dlsym(h, "curl_easy_perform"));
                auto* ec = reinterpret_cast<void (*)(CURL*)>(::dlsym(h, "curl_easy_cleanup"));
                auto* sa =
                    reinterpret_cast<struct curl_slist* (*)(struct curl_slist*, const char*)>(
                        ::dlsym(h, "curl_slist_append"));
                auto* sf = reinterpret_cast<void (*)(struct curl_slist*)>(
                    ::dlsym(h, "curl_slist_free_all"));
                if (ei && es && ep && ec && sa && sf) {
                    handle = h;
                    easy_init = ei;
                    easy_setopt = es;
                    easy_perform = ep;
                    easy_cleanup = ec;
                    slist_append = sa;
                    slist_free_all = sf;
                    easy_strerror = reinterpret_cast<const char* (*)(CURLcode)>(
                        ::dlsym(h, "curl_easy_strerror"));
                    return true;
                }
            }
            return false;
        }
        ~CurlAPI() {
            if (handle)
                ::dlclose(handle);
        }
    };
    CurlAPI& get_curl() {
        static CurlAPI c;
        return c;
    }
    // Issue #5267: CURLOPT_WRITEFUNCTION must be the write callback.
    // Passing this accessor itself makes libcurl call the accessor as the
    // writer. That call does not append bytes, and the value it returns is
    // not the byte count, so curl aborts with a write error after the
    // request has already left. The buffer stays empty and the primitive
    // used to answer void.
    using CurlWriteFn = std::size_t (*)(char*, std::size_t, std::size_t, void*);
    CurlWriteFn curl_write_callback() {
        return [](char* ptr, std::size_t size, std::size_t nmemb, void* userdata) -> std::size_t {
            if (ptr == nullptr || userdata == nullptr)
                return 0;
            const std::size_t nbytes = size * nmemb;
            static_cast<std::string*>(userdata)->append(ptr, nbytes);
            return nbytes;
        };
    }

    // Issue #5268: the total transfer budget defaults to 120s so a reasoning
    // model can finish. AURA_HTTP_TIMEOUT (whole seconds, 1..86400) is the
    // process default. An explicit argument wins over the variable.
    // The connect budget stays short: 10s, or the total when that is shorter.
    constexpr long kHttpTimeoutDefaultSec = 120;
    constexpr long kHttpTimeoutMaxSec = 86400;
    constexpr long kHttpConnectTimeoutSec = 10;

    long http_default_timeout_sec() {
        if (const char* env = std::getenv("AURA_HTTP_TIMEOUT")) {
            char* end = nullptr;
            errno = 0;
            const long v = std::strtol(env, &end, 10);
            if (end != env && *end == '\0' && errno == 0 && v >= 1 && v <= kHttpTimeoutMaxSec)
                return v;
        }
        return kHttpTimeoutDefaultSec;
    }

    long http_connect_timeout_sec(long total) {
        if (total < 1)
            return 1;
        return total < kHttpConnectTimeoutSec ? total : kHttpConnectTimeoutSec;
    }

    enum class TimeoutArgKind { Absent, Ok, Bad };

    struct TimeoutArg {
        TimeoutArgKind kind = TimeoutArgKind::Absent;
        long seconds = 0;
    };

    TimeoutArg timeout_from_value(const EvalValue& v) {
        TimeoutArg out;
        double raw = 0;
        if (types::is_int(v))
            raw = static_cast<double>(types::as_int(v));
        else if (types::is_float(v))
            raw = types::as_float(v);
        else
            return out;
        out.kind = TimeoutArgKind::Bad;
        if (!(raw >= 1.0) || raw > static_cast<double>(kHttpTimeoutMaxSec))
            return out;
        out.kind = TimeoutArgKind::Ok;
        out.seconds = static_cast<long>(raw);
        return out;
    }

    std::string http_transport_error(const char* op, CURLcode code) {
        if (code == CURLE_OPERATION_TIMEDOUT)
            return std::string(op) + ": timeout";
        if (get_curl().easy_strerror != nullptr) {
            if (const char* text = get_curl().easy_strerror(code);
                text != nullptr && text[0] != '\0')
                return std::string(op) + ": " + text;
        }
        return std::string(op) + ": curl error " + std::to_string(static_cast<int>(code));
    }
} // namespace


namespace {

    // Issue #4381: jailed git child for the Restricted+MT / Strict tenant FS
    // jail (#4233 fence family). Post-fork the child chdir's under the caller's
    // tenant root BEFORE exec — the parent process cwd is never mutated
    // (fiber-safe; the libgit2 in-process backend stays Soft/Off-only because
    // it binds the shared process cwd/repo). argv is explicit (no shell);
    // stderr is silenced like the legacy fallbacks. capture=true collects
    // stdout (raw — the caller keeps its own legacy trailing-newline
    // contract). Returns WEXITSTATUS, or -1 on fork/exec failure.
    int run_git_jailed(const std::vector<std::string>& args, const std::string& jail_root,
                       bool capture, std::string* out) {
        int pfd[2] = {-1, -1};
        if (capture && ::pipe2(pfd, O_CLOEXEC) != 0)
            return -1;
        int jail_sv[2] = {-1, -1};
        if (!jail_root.empty() &&
            ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, jail_sv) != 0) {
            if (capture) {
                ::close(pfd[0]);
                ::close(pfd[1]);
            }
            return -1;
        }
        std::vector<std::string> buf = args; // owned NUL-terminated storage
        pid_t pid = ::fork();
        if (pid < 0) {
            if (capture) {
                ::close(pfd[0]);
                ::close(pfd[1]);
            }
            if (jail_sv[0] >= 0)
                ::close(jail_sv[0]);
            if (jail_sv[1] >= 0)
                ::close(jail_sv[1]);
            return -1;
        }
        if (pid == 0) {
            if (jail_sv[0] >= 0)
                ::close(jail_sv[0]);
            if (!jail_root.empty() && ::chdir(jail_root.c_str()) != 0)
                ::_exit(127);
            if (capture) {
                ::close(pfd[0]);
                ::dup2(pfd[1], STDOUT_FILENO);
                ::close(pfd[1]);
            }
            int devnull = ::open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }
            // Issue #4413: Landlock after /dev/null is wired, before exec.
            if (!jail_root.empty() && !::aura::compiler::security::apply_tenant_exec_fs_jail(
                                          jail_root.c_str(), jail_sv[1]))
                ::_exit(127);
            std::vector<char*> argv;
            argv.reserve(buf.size() + 2);
            argv.push_back(const_cast<char*>("git"));
            for (auto& s : buf)
                argv.push_back(s.data());
            argv.push_back(nullptr);
            ::execvp("git", argv.data());
            ::_exit(127);
        }
        if (jail_sv[1] >= 0)
            ::close(jail_sv[1]);
        if (!jail_root.empty()) {
            if (capture)
                ::close(pfd[1]);
            return ::aura::compiler::security::tenant_exec_jail_reap(
                pid, jail_sv[0], capture ? pfd[0] : -1, capture ? out : nullptr, jail_root.c_str());
        }
        if (capture) {
            ::close(pfd[1]);
            std::array<char, 4096> chunk;
            for (ssize_t n; (n = ::read(pfd[0], chunk.data(), chunk.size())) > 0;)
                out->append(chunk.data(), static_cast<std::size_t>(n));
            ::close(pfd[0]);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) != -1 && WIFEXITED(status))
            return WEXITSTATUS(status);
        return -1;
    }

} // namespace

#if AURA_ENABLE_GIT
void RegSplitParts::register_git_inspect_primitives(Evaluator& ev) {
    // (git-status) → short status string (like "git status --short")
    ev.defer_std_host_prim("git-status", [&ev](const auto&) -> EvalValue {
        // Issue #4381: Restricted+MT / Strict tenant FS jail — the shared
        // process cwd is the HOST repo; under an armed jail every git-*
        // face runs through check_tenant_exec_jail (the #4233 shell fence,
        // no second model) and the child chdir's under the caller's tenant
        // root. Fixed argv (no caller text) so the fence sees a clean
        // command; the libgit2 in-process backend is skipped on the jailed
        // path. Soft/Off keeps the legacy passthrough (zero-cost).
        std::string jail_root;
        if (!ev.check_tenant_exec_jail("status --short", jail_root, "git-status"))
            return make_void();
        if (!jail_root.empty()) {
            std::string out;
            (void)run_git_jailed({"status", "--short"}, jail_root, /*capture=*/true, &out);
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            auto sid = ev.string_heap_.mint(std::move(out));
            return make_string(sid);
        }
        std::string result;
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            result = ctx.status_short();
        }
#endif
        if (result.empty()) {
            // Fallback: popen
            std::array<char, 4096> buf;
            const ::aura::core::unique_pipe pipe{::popen("git status --short 2>/dev/null", "r")};
            if (!pipe)
                return make_void();
            while (::fgets(buf.data(), buf.size(), pipe.get()) != nullptr)
                result += buf.data();
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.mint(std::move(result));
        return make_string(sid);
    });

    // (git-diff ["staged"]) → unified diff
    ev.defer_std_host_prim("git-diff", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool staged = false;
        if (a.size() >= 1 && is_string(a[0])) {
            auto mi = as_string_idx(a[0]);
            if (mi < ev.string_heap_.size()) {
                // Issue #4642: the operand is read to compare against the
                // constant "staged" - a one-bit oracle on another
                // principal's stamped string. In-range before the compare.
                if (!ev.inrange_string_tenant_allows(
                        mi,
                        static_cast<std::uint16_t>(aura::compiler::security::kEffectWrite |
                                                   aura::compiler::security::kEffectExec),
                        "git-diff"))
                    return make_void();
                if (ev.string_at(mi) == "staged") {
                    staged = true;
                }
            }
        }
        // Issue #4381: tenant FS jail — same fence family as git-status
        // above; jailed child runs `git diff` under the caller's root.
        std::string jail_root;
        if (!ev.check_tenant_exec_jail(staged ? "diff --staged" : "diff", jail_root, "git-diff"))
            return make_void();
        if (!jail_root.empty()) {
            std::string out;
            if (staged)
                (void)run_git_jailed({"diff", "--staged"}, jail_root, /*capture=*/true, &out);
            else
                (void)run_git_jailed({"diff"}, jail_root, /*capture=*/true, &out);
            auto sid = ev.string_heap_.mint(std::move(out));
            return make_string(sid);
        }
        std::string result;
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            result = ctx.diff(staged);
        }
#endif
        if (result.empty()) {
            // Fallback: popen
            const char* cmd = staged ? "git diff --staged 2>/dev/null" : "git diff 2>/dev/null";
            std::array<char, 4096> buf;
            const ::aura::core::unique_pipe pipe{::popen(cmd, "r")};
            if (!pipe)
                return make_void();
            while (::fgets(buf.data(), buf.size(), pipe.get()) != nullptr)
                result += buf.data();
        }
        auto sid = ev.string_heap_.mint(std::move(result));
        return make_string(sid);
    });

    // (git-log n) → last n commits, one-line format (n=1..1000, default 10)
    ev.defer_std_host_prim("git-log", [&ev](std::span<const EvalValue> a) -> EvalValue {
        int n = 10;
        if (a.size() >= 1 && is_int(a[0]))
            n = static_cast<int>(as_int(a[0]));
        if (n < 1)
            n = 1;
        if (n > 1000)
            n = 1000;
        std::string result;
        // Issue #4381: tenant FS jail — same fence family; jailed child
        // runs `git log --oneline -n <n>` under the caller's root.
        std::string jail_root;
        if (!ev.check_tenant_exec_jail("log --oneline", jail_root, "git-log"))
            return make_void();
        if (!jail_root.empty()) {
            std::string out;
            (void)run_git_jailed({"log", "--oneline", "-n", std::to_string(n)}, jail_root,
                                 /*capture=*/true, &out);
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            auto sid = ev.string_heap_.mint(std::move(out));
            return make_string(sid);
        }
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            result = ctx.log_oneline(n);
        }
#endif
        if (result.empty()) {
            // Fallback: popen
            std::string cmd = "git log --oneline -n " + std::to_string(n) + " 2>/dev/null";
            std::array<char, 4096> buf;
            const ::aura::core::unique_pipe pipe{::popen(cmd.c_str(), "r")};
            if (!pipe)
                return make_void();
            while (::fgets(buf.data(), buf.size(), pipe.get()) != nullptr)
                result += buf.data();
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.mint(std::move(result));
        return make_string(sid);
    });
}

void RegSplitParts::register_git_index_primitives(Evaluator& ev) {
    // (git-commit "message") → exit code (0 = ok, no shell escape needed)
    ev.defer_std_host_prim("git-commit", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // Issue #2072: force git-commit through capability effect check
        // (exec + network — git-commit forks a child process AND may push
        // to remote). require_effect() bumps capability_effect_denied_total
        // on deny + records the op in the mutation audit ring.
        using aura::compiler::security::kEffectExec;
        using aura::compiler::security::kEffectNetwork;
        if (!ev.require_effect(static_cast<std::uint16_t>(kEffectExec | kEffectNetwork),
                               "git-commit"))
            return make_int(-1);
        // Issue #4381: tenant FS jail (same fence as shell/command-output,
        // #4233 family) AFTER the #2072 effect choke — jailed commit
        // children chdir under the caller's tenant root; libgit2 stays
        // Soft/Off-only (process-global repo state). The message travels
        // as execvp argv (no shell), not a path — the fence owns paths.
        std::string jail_root;
        if (!ev.check_tenant_exec_jail("commit", jail_root, "git-commit"))
            return make_int(-1);
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto mi = as_string_idx(a[0]);
        if (mi >= ev.string_heap_.size())
            return make_int(-1);
        // Issue #4642: the commit message leaves the heap into argv (or the
        // libgit2 commit body). Write|Exec is what the dispatch already
        // required - in-range before the copy, no second require_effect.
        if (!ev.inrange_string_tenant_allows(
                mi,
                static_cast<std::uint16_t>(aura::compiler::security::kEffectWrite |
                                           aura::compiler::security::kEffectExec),
                "git-commit"))
            return make_int(-1);
        const std::string& msg = ev.string_at(mi);
        int rc = -1;
        if (!jail_root.empty())
            return make_int(
                run_git_jailed({"commit", "-m", msg}, jail_root, /*capture=*/false, nullptr));
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            rc = ctx.commit(msg);
        } else
#endif
        {
            // Issue #473 §6: replace `::system()` with explicit fork+execlp.
            // `::system(cmd)` invokes /bin/sh -c and runs the entire `cmd`
            // string through shell interpretation. With single-quote
            // escaping there are still edge cases (NUL bytes, certain
            // locales) where injection can leak through. fork+execlp with
            // explicit argv avoids shell parsing entirely. stderr is
            // redirected to /dev/null to preserve the legacy silent-fail
            // behavior the previous `2>/dev/null` provided.
            std::string msg_copy = msg; // NUL-terminated owned buffer
            pid_t pid = ::fork();
            if (pid == 0) {
                // Child: redirect stderr to /dev/null then exec git.
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    ::dup2(devnull, STDERR_FILENO);
                    ::close(devnull);
                }
                ::execlp("git", "git", "commit", "-m", msg_copy.c_str(),
                         static_cast<char*>(nullptr));
                ::_exit(127); // exec failed
            }
            if (pid > 0) {
                int status = 0;
                if (::waitpid(pid, &status, 0) != -1) {
                    if (WIFEXITED(status))
                        rc = WEXITSTATUS(status);
                } else {
                    rc = -1;
                }
            } else {
                rc = -1; // fork failed
            }
        }
        return make_int(rc);
    });

    // (git-branch-current) → current branch name (empty if detached)
    ev.defer_std_host_prim("git-branch-current", [&ev](const auto&) -> EvalValue {
        // Issue #4381: tenant FS jail — same fence family; jailed child
        // runs `git rev-parse --abbrev-ref HEAD` under the caller's root.
        std::string jail_root;
        if (!ev.check_tenant_exec_jail("rev-parse --abbrev-ref HEAD", jail_root,
                                       "git-branch-current"))
            return make_void();
        if (!jail_root.empty()) {
            std::string out;
            (void)run_git_jailed({"rev-parse", "--abbrev-ref", "HEAD"}, jail_root,
                                 /*capture=*/true, &out);
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            auto sid = ev.string_heap_.mint(std::move(out));
            return make_string(sid);
        }
        std::string result;
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            result = ctx.branch_current();
        }
#endif
        if (result.empty()) {
            std::array<char, 256> buf;
            const ::aura::core::unique_pipe pipe{
                ::popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r")};
            if (!pipe)
                return make_void();
            while (::fgets(buf.data(), buf.size(), pipe.get()) != nullptr)
                result += buf.data();
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.mint(std::move(result));
        return make_string(sid);
    });

    // (git-stage "file1" "file2" ...) → exit code
    ev.defer_std_host_prim("git-stage", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_int(-1);
        // Issue #4381: tenant FS jail — the fence first (#4233 family),
        // then EVERY path argument resolves through check_tenant_host_path
        // (#3802 face): absolute / cross-tenant / prefix-escape paths deny
        // with IsolationDeny tenant-path-escape and zero exec; surviving
        // paths go to the jailed `git add` child as tenant-rooted
        // absolutes (child cwd = tenant root).
        std::string jail_root;
        if (!ev.check_tenant_exec_jail("add", jail_root, "git-stage"))
            return make_int(-1);
        std::vector<std::string> path_bufs;
        path_bufs.reserve(a.size());
        for (const auto& v : a) {
            if (!is_string(v))
                return make_int(-1);
            auto si = as_string_idx(v);
            if (si >= ev.string_heap_.size())
                return make_int(-1);
            // Issue #4642: the path bytes leave the heap into the git add
            // argv. Write|Exec is the dispatch effect - in-range before the
            // copy (the tenant host-path jail below is a separate fence).
            if (!ev.inrange_string_tenant_allows(
                    si,
                    static_cast<std::uint16_t>(aura::compiler::security::kEffectWrite |
                                               aura::compiler::security::kEffectExec),
                    "git-stage"))
                return make_int(-1);
            std::string p(ev.string_at(si));
            if (!jail_root.empty()) {
                std::string resolved;
                if (!ev.check_tenant_host_path(p, resolved, "git-stage"))
                    return make_int(-1);
                path_bufs.push_back(std::move(resolved));
            } else {
                path_bufs.push_back(std::move(p));
            }
        }
        if (!jail_root.empty()) {
            std::vector<std::string> args;
            args.reserve(path_bufs.size() + 1);
            args.push_back("add");
            for (auto& s : path_bufs)
                args.push_back(std::move(s));
            return make_int(run_git_jailed(args, jail_root, /*capture=*/false, nullptr));
        }
        int rc = -1;
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            rc = ctx.stage(path_bufs);
        } else
#endif
        {
            // Issue #1161: fork+execvp — no shell (matches git-commit #473).
            pid_t pid = ::fork();
            if (pid == 0) {
                int devnull = ::open("/dev/null", O_WRONLY);
                if (devnull >= 0) {
                    ::dup2(devnull, STDERR_FILENO);
                    ::close(devnull);
                }
                std::vector<char*> argv;
                argv.push_back(const_cast<char*>("git"));
                argv.push_back(const_cast<char*>("add"));
                for (auto& s : path_bufs)
                    argv.push_back(s.data());
                argv.push_back(nullptr);
                ::execvp("git", argv.data());
                ::_exit(127);
            }
            if (pid > 0) {
                int status = 0;
                if (::waitpid(pid, &status, 0) != -1 && WIFEXITED(status))
                    rc = WEXITSTATUS(status);
            } else {
                rc = -1;
            }
        }
        return make_int(rc);
    });
}

void RegSplitParts::register_git_rev_parse_primitive(Evaluator& ev) {
    // (git-rev-parse) → current HEAD sha (short, 7 chars)
    ev.defer_std_host_prim("git-rev-parse", [&ev](const auto&) -> EvalValue {
        // Issue #4381: tenant FS jail — same fence family; jailed child
        // runs `git rev-parse --short HEAD` under the caller's root.
        std::string jail_root;
        if (!ev.check_tenant_exec_jail("rev-parse --short HEAD", jail_root, "git-rev-parse"))
            return make_void();
        if (!jail_root.empty()) {
            std::string out;
            (void)run_git_jailed({"rev-parse", "--short", "HEAD"}, jail_root,
                                 /*capture=*/true, &out);
            if (!out.empty() && out.back() == '\n')
                out.pop_back();
            auto sid = ev.string_heap_.mint(std::move(out));
            return make_string(sid);
        }
        std::string result;
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            result = ctx.rev_parse_short();
        }
#endif
        if (result.empty()) {
            std::array<char, 64> buf;
            const ::aura::core::unique_pipe pipe{
                ::popen("git rev-parse --short HEAD 2>/dev/null", "r")};
            if (!pipe)
                return make_void();
            while (::fgets(buf.data(), buf.size(), pipe.get()) != nullptr)
                result += buf.data();
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.mint(std::move(result));
        return make_string(sid);
    });
}
#endif

void register_git_primitives(PrimRegistrar add, Evaluator& ev) {
#if !AURA_ENABLE_GIT
    // Issue #1970: integration vertical disabled for this build.
    (void)add;
    (void)ev;
    return;
#else
    // ── Git integration (Issue #96) ─────────────────────────────
    // Backed by libgit2 (in-process) when available, with popen fallback
    // for systems without libgit2. Much faster than fork+exec per call,
    // and avoids shell escape issues for commit messages and paths.
    // Issue #1970: commercial/integration vertical (AURA_ENABLE_GIT).

    RegSplitParts::register_git_inspect_primitives(ev);
    RegSplitParts::register_git_index_primitives(ev);
    RegSplitParts::register_git_rev_parse_primitive(ev);
#endif // AURA_ENABLE_GIT
}

// Issue #4561: the process fd table is shared across tenants. Stamp the
// creating tenant when this process mints an fd. Soft / Off and
// single-tenant Restricted never touch the map. Restricted+MT and Strict
// deny recv/send/close/read when the fd is foreign or was not minted here.
namespace {

    bool fd_owner_face_active(const Evaluator& ev) noexcept {
        const auto mode = ev.effect_sandbox_mode();
        if (mode == 0)
            return false;
        return mode == 2 || ::aura::core::sandbox::is_strict() ||
               ::aura::core::provenance::multi_tenant_env_active();
    }

    // Issue #4922: one record per minted fd. The minting Evaluator is part
    // of the record so teardown closes only fds this instance still owns —
    // a number the kernel re-hands to another tenant after a close keeps
    // its new owner and is not closed twice.
    struct MintedFd {
        std::uint64_t tenant = 0;
        const Evaluator* ev = nullptr;
    };

    struct FdTenantTable {
        std::mutex mu;
        std::unordered_map<int, MintedFd> owner;
        // Issue #4922: fds this Evaluator minted and has not released yet.
        // sys-close drains its own entry; ~Evaluator drains the whole set so
        // a tenant without a sys-close call does not hold an OS fd to exit.
        std::unordered_map<const Evaluator*, std::unordered_set<int>> owned;
    };

    FdTenantTable& fd_tenant_table() {
        static FdTenantTable table;
        return table;
    }

    void stamp_minted_fd_tenant(const Evaluator& ev, int fd) {
        if (!fd_owner_face_active(ev) || fd < 0)
            return;
        auto& table = fd_tenant_table();
        std::scoped_lock<std::mutex> lock(table.mu);
        table.owner[fd] = MintedFd{ev.capability_tenant_id(), &ev};
        table.owned[&ev].insert(fd);
    }

    void erase_minted_fd_tenant(const Evaluator& ev, int fd) {
        if (!fd_owner_face_active(ev) || fd < 0)
            return;
        auto& table = fd_tenant_table();
        std::scoped_lock<std::mutex> lock(table.mu);
        table.owner.erase(fd);
        if (const auto it = table.owned.find(&ev); it != table.owned.end())
            it->second.erase(fd);
    }

    // true: the caller may use fd. false: do not recv, send, close, or read.
    // A mismatch or an fd this process did not mint emits IsolationDeny and
    // still returns false when a cross-grant would have allowed the audit.
    bool minted_fd_tenant_allows(Evaluator& ev, int fd, std::string_view op) {
        if (!fd_owner_face_active(ev))
            return true;
        bool minted = false;
        std::uint64_t owner = 0;
        {
            auto& table = fd_tenant_table();
            std::scoped_lock<std::mutex> lock(table.mu);
            const auto it = table.owner.find(fd);
            if (it != table.owner.end()) {
                minted = true;
                owner = it->second.tenant;
            }
        }
        if (minted && owner == ev.capability_tenant_id())
            return true;
        (void)ev.check_workspace_isolation(ev.capability_tenant_id(), minted ? owner : 0,
                                           /*required_effects=*/0, op);
        return false;
    }

} // namespace

void register_network_primitives(PrimRegistrar add, Evaluator& ev) {

    // Issue #4504: environ and self-path reads are host io-read channels.
    // Same deny_io shape as register_file_primitives (command-line, #2478):
    // allow when the sandbox is off or the principal holds the named cap /
    // io / *; otherwise bump_capability_denial() and return the error with
    // the host value unread. No new cap name / effect bit / query key.
    const auto deny_io = [&ev](std::string_view cap, std::string_view msg) -> EvalValue {
        if (!ev.sandbox_mode() || ev.has_capability(cap) ||
            ev.has_capability(aura::compiler::security::kCapIo) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return make_void();
        ev.bump_capability_denial();
        return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    };

    // ── Environment + HTTP primitives ────────────────────────
    add("getenv", [&ev, deny_io](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        // Issue #4504: fail closed behind the io-read fence BEFORE ::getenv.
        // On deny the value is never copied into string_heap_. #4494 (unset →
        // #f) stays on the allowed path below; Soft/Off still reads because
        // deny_io allows before the cap check.
        if (auto denied = deny_io(aura::compiler::security::kCapIoRead,
                                  "capability denied: io-read required");
            is_error(denied))
            return denied;
        // Issue #4642: the env name leaves the heap into ::getenv. The
        // io-read fence above is a capability check, not a tenant check -
        // in-range before the copy (the dispatch effect stays kEffectNone).
        // The io error shape stays reserved for the io-read denial; a stamp
        // mismatch is an IsolationDeny and the env value is unread.
        const auto nidx = types::as_string_idx(a[0]);
        if (nidx >= ev.string_heap_.size())
            return make_void();
        if (!ev.inrange_string_tenant_allows(nidx, aura::compiler::security::kEffectNone, "getenv"))
            return make_void();
        auto name = ev.string_at(nidx);
        auto* val = ::getenv(name.c_str());
        // Issue #4494: unset → #f (R7RS get-environment-variable), so the
        // (or (getenv "X") default) idiom falls back. Void was truthy.
        if (!val)
            return types::make_bool(false);
        auto sidx = ev.string_heap_.mint(std::string(val));
        return types::make_string(sidx);
    });

    // Issue #2772: absolute path of this process image so denseness
    // multi-process probes can re-exec without relying on a non-exported
    // AURA_BIN shell variable. Prefer /proc/self/exe; fall back to env.
    add("aura-executable-path", [&ev](std::span<const EvalValue>) -> EvalValue {
        auto path = aura::compiler::paths::resolve_self_executable();
        if (path.empty())
            return make_void();
        auto sidx = ev.string_heap_.mint(std::move(path));
        return types::make_string(sidx);
    });

    // Issue #1077: shell-safe quoting for popen fallback (single-quote +
    // escape embedded quotes). Prefer native libcurl when available.
    auto shell_single_quote = [](std::string_view s) -> std::string {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('\'');
        for (char c : s) {
            if (c == '\'')
                out += "'\\''";
            else
                out.push_back(c);
        }
        out.push_back('\'');
        return out;
    };

    // ── HTTP primitives (libcurl preferred; shell fallback is quoted) ─
    // The registrar is the friend that can touch the private heap. A macro
    // keeps the defer_std_host_prim lines in the shape the #4382/#4559
    // source checks locate, and expands inside the lambdas that capture ev.
#define HTTP_FAIL(msg)                                                                             \
    make_primitive_error(ev.string_heap_, ev.error_values_, (msg), ev.primitive_error_counter_ptr())
    ev.defer_std_host_prim(
        "http-get", [&ev, shell_single_quote](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !types::is_string(a[0]))
                return make_void();
            const auto uidx = types::as_string_idx(a[0]);
            if (uidx >= ev.string_heap_.size())
                return make_void();
            // Issue #4559: the URL bytes leave the heap into libcurl —
            // in-range before any copy (Network face).
            if (!ev.inrange_string_tenant_allows(uidx, aura::compiler::security::kEffectNetwork,
                                                 "http-get"))
                return make_void();
            const auto& url = ev.string_at(uidx);
            // Issue #4382: scheme jail — only http/https under the tenant
            // jail; file:// and other schemes deny IsolationDeny
            // tenant-path-escape with zero perform.
            if (!ev.check_tenant_http_scheme(url, "http-get"))
                return make_void();
            // Optional second argument: total timeout in seconds.
            // Absent uses AURA_HTTP_TIMEOUT, then 120.
            long timeout_sec = 0;
            if (a.size() >= 2) {
                const TimeoutArg parsed = timeout_from_value(a[1]);
                if (parsed.kind != TimeoutArgKind::Ok)
                    return HTTP_FAIL("http-get: bad timeout");
                timeout_sec = parsed.seconds;
            } else {
                timeout_sec = http_default_timeout_sec();
            }
            const long connect_sec = http_connect_timeout_sec(timeout_sec);
            const std::string url_copy(url);
            // Issue #5274: libcurl blocks on this thread. Do not keep the
            // mutation hold across that wait. url_copy outlives the abort.
            (void)aura_mutation_hold_release_before_block();

            // Prefer libcurl (no shell) when available.
            if (!get_curl().load()) {
                // Issue #1160: no shell fallback. Without libcurl, refuse
                // rather than interpolate the URL into popen/curl.
                (void)shell_single_quote;
                return HTTP_FAIL("http-get: libcurl unavailable");
            }
            (void)shell_single_quote;
            CURL* curl = get_curl().easy_init();
            if (!curl)
                return HTTP_FAIL("http-get: curl init failed");
            std::string result;
            get_curl().easy_setopt(curl, CURLOPT_URL, url_copy.c_str());
            get_curl().easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback());
            get_curl().easy_setopt(curl, CURLOPT_WRITEDATA, &result);
            get_curl().easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
            get_curl().easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_sec);
            get_curl().easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            get_curl().easy_setopt(curl, CURLOPT_USERAGENT, "aura/1.0");
            get_curl().easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            // Issue #4382: restrict protocols to http/https —
            // redirects onto file:// or other schemes are refused
            // by the library (REDIR_PROTOCOLS).
            get_curl().easy_setopt(curl, CURLOPT_PROTOCOLS,
                                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
            get_curl().easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
            const CURLcode res = get_curl().easy_perform(curl);
            get_curl().easy_cleanup(curl);
            if (res != CURLE_OK)
                return HTTP_FAIL(http_transport_error("http-get", res));
            // CURLE_OK with an empty body is a successful empty string.
            auto sidx = ev.string_heap_.mint(std::move(result));
            return types::make_string(sidx);
        });

    ev.defer_std_host_prim("http-post", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
        // Issue #1077: bounds-check before string_heap_ index.
        const auto uidx = types::as_string_idx(a[0]);
        const auto bidx = types::as_string_idx(a[1]);
        if (uidx >= ev.string_heap_.size() || bidx >= ev.string_heap_.size())
            return make_void();
        // Issue #4559: URL / body / auth all leave the heap (wire or the
        // CLI fallback's header pipe) — in-range each before any copy.
        if (!ev.inrange_string_tenant_allows(uidx, aura::compiler::security::kEffectNetwork,
                                             "http-post"))
            return make_void();
        if (!ev.inrange_string_tenant_allows(bidx, aura::compiler::security::kEffectNetwork,
                                             "http-post"))
            return make_void();
        if (a.size() >= 3 && types::is_string(a[2])) {
            const auto aidx = types::as_string_idx(a[2]);
            if (aidx < ev.string_heap_.size() &&
                !ev.inrange_string_tenant_allows(aidx, aura::compiler::security::kEffectNetwork,
                                                 "http-post"))
                return make_void();
        }

        auto& curl_url = ev.mutable_string_at(uidx);
        auto& curl_body = ev.mutable_string_at(bidx);
        std::string auth;
        if (a.size() >= 3 && types::is_string(a[2])) {
            const auto aidx = types::as_string_idx(a[2]);
            if (aidx < ev.string_heap_.size())
                auth = ev.string_at(aidx);
        }
        // Issue #4382: scheme jail — same fence as http-get, before the
        // async / libcurl / CLI paths (zero perform / zero exec on deny).
        if (!ev.check_tenant_http_scheme(curl_url, "http-post"))
            return make_void();

        // Optional timeout: a number in the 3rd slot (no auth) or the 4th
        // slot (after the bearer). Absent uses AURA_HTTP_TIMEOUT, then 120.
        TimeoutArg explicit_timeout;
        bool saw_timeout = false;
        if (a.size() >= 3 && (types::is_int(a[2]) || types::is_float(a[2]))) {
            explicit_timeout = timeout_from_value(a[2]);
            saw_timeout = true;
        }
        if (a.size() >= 4) {
            explicit_timeout = timeout_from_value(a[3]);
            saw_timeout = true;
        }
        if (saw_timeout && explicit_timeout.kind != TimeoutArgKind::Ok)
            return HTTP_FAIL("http-post: bad timeout");
        const long timeout_sec = explicit_timeout.kind == TimeoutArgKind::Ok
                                     ? explicit_timeout.seconds
                                     : http_default_timeout_sec();
        const long connect_sec = http_connect_timeout_sec(timeout_sec);

        // Async HTTP via thread (fiber-friendly, serve mode only).
        // Issue #4053: copy URL/body before unlocking denseness body mutex
        // (Soft Ready #4048 holds it for the whole denseness apply_closure).
        // Unlock across the wait/yield so N denseness http-post calls can
        // overlap under Soft Ready workers=1; re-lock before touching the
        // Evaluator heap again (same protocol as fiber:join / #2869).
        if (aura::messaging::g_http_post_async) {
            std::string url_copy = curl_url;
            std::string body_copy = curl_body;
            std::string result;
            // Issue #5274: copies are already off the heap. Drop the hold
            // before yield/curl so a 20–60s chat cannot pin the budget.
            (void)aura_mutation_hold_release_before_block();
            {
                aura::messaging::DensenessBodyLockWaitGuard body_http_guard;
                result = aura::messaging::g_http_post_async(url_copy, body_copy, auth, timeout_sec);
            }
            if (!result.empty()) {
                auto sidx = ev.string_heap_.mint(std::move(result));
                return types::make_string(sidx);
            }
        }

        // Try native libcurl (synchronous, default path).
        // Issue #5214: a loaded libcurl that fails, returns an empty body,
        // or whose easy_init fails must not POST again through the CLI.
        // Issue #5267: CURLE_OK with an empty body is a successful empty
        // string. A transport failure is an error value, still with no
        // CLI fallback.
        std::string result;
        const bool curl_loaded = get_curl().load();
        if (curl_loaded) {
            auto& curl_url = ev.mutable_string_at(types::as_string_idx(a[0]));
            auto& curl_body = ev.mutable_string_at(types::as_string_idx(a[1]));

            CURL* curl = get_curl().easy_init();
            if (!curl)
                return HTTP_FAIL("http-post: curl init failed");
            struct curl_slist* headers = nullptr;
            headers = get_curl().slist_append(headers, "Content-Type: application/json");
            if (a.size() >= 3 && types::is_string(a[2])) {
                auto& auth = ev.mutable_string_at(types::as_string_idx(a[2]));
                headers = get_curl().slist_append(
                    headers, (std::string("Authorization: Bearer ") + auth).c_str());
            }

            std::string response;
            // Issue #5274: POSTFIELDS is not copied by libcurl. Snapshot
            // first, then drop the hold, so an abort cannot free the buffer
            // perform is about to read.
            const std::string url_io(curl_url);
            const std::string body_io(curl_body);
            (void)aura_mutation_hold_release_before_block();
            get_curl().easy_setopt(curl, CURLOPT_URL, url_io.c_str());
            get_curl().easy_setopt(curl, CURLOPT_POST, 1L);
            get_curl().easy_setopt(curl, CURLOPT_POSTFIELDS, body_io.c_str());
            get_curl().easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_io.size()));
            get_curl().easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            get_curl().easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback());
            get_curl().easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            get_curl().easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
            get_curl().easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_sec);
            get_curl().easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            get_curl().easy_setopt(curl, CURLOPT_USERAGENT, "aura/1.0");
            // Issue #4382: restrict protocols to http/https — the
            // scheme fence already denied non-http(s) URLs; this
            // keeps redirects and libcurl default schemes in line.
            get_curl().easy_setopt(curl, CURLOPT_PROTOCOLS,
                                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
            get_curl().easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                                   static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));

            const CURLcode res = get_curl().easy_perform(curl);
            get_curl().slist_free_all(headers);
            get_curl().easy_cleanup(curl);
            if (res != CURLE_OK)
                return HTTP_FAIL(http_transport_error("http-post", res));
            result = std::move(response);
        }

        // CLI only when libcurl cannot be loaded. The bearer is written to
        // a pipe and passed as `-H @/dev/fd/N`, never as an argv element.
        // `--config -` would steal stdin from the body, so it is not used.
        if (!curl_loaded) {
            // Issue #5274: own the bytes before fork/wait. A hold released
            // here must not leave the child reading a restored heap.
            const std::string url = ev.string_at(types::as_string_idx(a[0]));
            const std::string body = ev.string_at(types::as_string_idx(a[1]));
            (void)aura_mutation_hold_release_before_block();
            // Soft scheme fence is a pass-through. A leading '-' would be
            // a curl option if it were a positional URL. Reject before exec.
            if (!url.empty() && url.front() == '-')
                return HTTP_FAIL("http-post: url rejected");
            std::string auth_line;
            if (a.size() >= 3 && types::is_string(a[2])) {
                const auto aidx = types::as_string_idx(a[2]);
                if (aidx < ev.string_heap_.size())
                    auth_line = std::string("Authorization: Bearer ") + ev.string_at(aidx) + "\n";
            }
            const bool want_auth = !auth_line.empty();
            std::array<int, 2> in_pipe{-1, -1};
            std::array<int, 2> out_pipe{-1, -1};
            std::array<int, 2> hdr_pipe{-1, -1};
            if (::pipe2(in_pipe.data(), O_CLOEXEC) != 0)
                return HTTP_FAIL("http-post: pipe failed");
            if (::pipe2(out_pipe.data(), O_CLOEXEC) != 0) {
                ::close(in_pipe[0]);
                ::close(in_pipe[1]);
                return HTTP_FAIL("http-post: pipe failed");
            }
            if (want_auth && ::pipe2(hdr_pipe.data(), O_CLOEXEC) != 0) {
                ::close(in_pipe[0]);
                ::close(in_pipe[1]);
                ::close(out_pipe[0]);
                ::close(out_pipe[1]);
                return HTTP_FAIL("http-post: pipe failed");
            }
            // dup2 in the child clears CLOEXEC on this kept fd so curl can
            // read the header after exec. The number is chosen here, before
            // fork, and is not one of the pipe ends.
            int hdr_keep = 100;
            const auto pipe_taken = [&](int fd) {
                return fd == in_pipe[0] || fd == in_pipe[1] || fd == out_pipe[0] ||
                       fd == out_pipe[1] || fd == hdr_pipe[0] || fd == hdr_pipe[1];
            };
            if (pipe_taken(hdr_keep))
                hdr_keep = 101;
            if (pipe_taken(hdr_keep))
                hdr_keep = 102;
            const std::string hdr_arg = std::string("@/dev/fd/") + std::to_string(hdr_keep);
            std::vector<std::string> argv_store;
            argv_store.emplace_back("curl");
            argv_store.emplace_back("-s");
            argv_store.emplace_back("-X");
            argv_store.emplace_back("POST");
            argv_store.emplace_back("--data-binary");
            argv_store.emplace_back("@-");
            argv_store.emplace_back("-H");
            argv_store.emplace_back("Content-Type: application/json");
            argv_store.emplace_back("--max-time");
            argv_store.emplace_back(std::to_string(timeout_sec));
            argv_store.emplace_back("--connect-timeout");
            argv_store.emplace_back(std::to_string(connect_sec));
            // Issue #4382: restrict protocols to http/https (and
            // redirects onto them) — mirrors the libcurl face.
            argv_store.emplace_back("--proto");
            argv_store.emplace_back("=http,https");
            argv_store.emplace_back("--proto-redir");
            argv_store.emplace_back("=http,https");
            if (want_auth) {
                argv_store.emplace_back("-H");
                argv_store.push_back(hdr_arg);
            }
            argv_store.emplace_back("--url");
            argv_store.push_back(url);
            std::vector<char*> argv_ptrs;
            argv_ptrs.reserve(argv_store.size() + 1);
            for (auto& word : argv_store)
                argv_ptrs.push_back(word.data());
            argv_ptrs.push_back(nullptr);
            const pid_t pid = ::fork();
            if (pid < 0) {
                ::close(in_pipe[0]);
                ::close(in_pipe[1]);
                ::close(out_pipe[0]);
                ::close(out_pipe[1]);
                if (want_auth) {
                    ::close(hdr_pipe[0]);
                    ::close(hdr_pipe[1]);
                }
                return HTTP_FAIL("http-post: fork failed");
            }
            if (pid == 0) {
                ::close(in_pipe[1]);
                ::close(out_pipe[0]);
                if (want_auth)
                    ::close(hdr_pipe[1]);
                if (::dup2(in_pipe[0], STDIN_FILENO) < 0 || ::dup2(out_pipe[1], STDOUT_FILENO) < 0)
                    ::_exit(127);
                if (in_pipe[0] != STDIN_FILENO)
                    ::close(in_pipe[0]);
                if (out_pipe[1] != STDOUT_FILENO)
                    ::close(out_pipe[1]);
                if (want_auth) {
                    if (::dup2(hdr_pipe[0], hdr_keep) < 0)
                        ::_exit(127);
                    if (hdr_pipe[0] != hdr_keep)
                        ::close(hdr_pipe[0]);
                }
                ::execvp(argv_ptrs[0], argv_ptrs.data());
                ::_exit(127);
            }
            ::close(in_pipe[0]);
            ::close(out_pipe[1]);
            if (want_auth)
                ::close(hdr_pipe[0]);
            auto write_full = [](int dest, std::string_view bytes) {
                while (!bytes.empty()) {
                    const ssize_t wrote = ::write(dest, bytes.data(), bytes.size());
                    if (wrote < 0) {
                        if (errno == EINTR)
                            continue;
                        return false;
                    }
                    if (wrote == 0)
                        return false;
                    bytes.remove_prefix(static_cast<std::size_t>(wrote));
                }
                return true;
            };
            if (want_auth) {
                (void)write_full(hdr_pipe[1], auth_line);
                ::close(hdr_pipe[1]);
            }
            (void)write_full(in_pipe[1], body);
            ::close(in_pipe[1]);
            std::array<char, 4096> fbuf{};
            while (true) {
                const ssize_t got = ::read(out_pipe[0], fbuf.data(), fbuf.size());
                if (got < 0) {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (got == 0)
                    break;
                result.append(fbuf.data(), static_cast<std::size_t>(got));
            }
            ::close(out_pipe[0]);
            int cstat = 0;
            pid_t waited = -1;
            do {
                waited = ::waitpid(pid, &cstat, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited < 0 || !WIFEXITED(cstat))
                return HTTP_FAIL("http-post: curl failed");
            const int code = WEXITSTATUS(cstat);
            if (code == static_cast<int>(CURLE_OPERATION_TIMEDOUT))
                return HTTP_FAIL("http-post: timeout");
            if (code == 127)
                return HTTP_FAIL("http-post: curl unavailable");
            if (code != 0)
                return HTTP_FAIL("http-post: curl exit " + std::to_string(code));
        }

        // Empty body on success stays a string. Failure returned above.
        auto sidx = ev.string_heap_.mint(std::move(result));
        return types::make_string(sidx);
    });

#undef HTTP_FAIL

#if AURA_ENABLE_TCP
    // ── TCP socket primitives ────────────────────────────────
    // Issue #1975: client path (connect/send/recv/close).
    // Issue #2771: server path (listen/accept/accept-timeout/local-port)
    // for multi-host denseness (Hermes Phase 5 residual). Thin OS E —
    // errors return void/() not process abort. FDs are plain ints;
    // caller owns close for both listener and accepted sockets.
    ev.defer_std_host_prim("tcp-connect", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_int(a[1]))
            return make_void();
        // Issue #4642: the host bytes leave the heap into getaddrinfo (DNS
        // + connect). Network is the dispatch effect - in-range before the
        // copy, no second require_effect.
        const auto hidx = types::as_string_idx(a[0]);
        if (hidx >= ev.string_heap_.size())
            return make_void();
        if (!ev.inrange_string_tenant_allows(hidx, aura::compiler::security::kEffectNetwork,
                                             "tcp-connect"))
            return make_void();
        auto host = ev.string_at(hidx);
        auto port_str = std::to_string(types::as_int(a[1]));
        struct addrinfo hints{}, *res;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
            return make_void();
        int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            ::freeaddrinfo(res);
            return make_void();
        }
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        // Non-blocking connect with 8s timeout
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int conn_ret = ::connect(fd, res->ai_addr, res->ai_addrlen);
        if (conn_ret < 0) {
            if (errno == EINPROGRESS) {
                // Wait for connection with timeout
                struct pollfd pfd = {fd, POLLOUT, 0};
                conn_ret = ::poll(&pfd, 1, 8000);
                if (conn_ret <= 0) {
                    ::close(fd);
                    ::freeaddrinfo(res);
                    return make_void(); // timeout or error
                }
                // Check if connect succeeded
                int so_error = 0;
                socklen_t len = sizeof(so_error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
                    ::close(fd);
                    ::freeaddrinfo(res);
                    return make_void();
                }
            } else {
                ::close(fd);
                ::freeaddrinfo(res);
                return make_void();
            }
        }
        ::fcntl(fd, F_SETFL, flags); // restore blocking
        ::freeaddrinfo(res);
        stamp_minted_fd_tenant(ev, fd);
        return types::make_int(static_cast<std::int64_t>(fd));
    });

    // (tcp-listen port) → listener-fd | ()
    // (tcp-listen port host) → listener-fd | ()
    // One argument binds 127.0.0.1 (loopback denseness host). The optional
    // host is an IPv4 literal such as "0.0.0.0" or "127.0.0.1"; anything
    // else is void. port 0 = ephemeral; use (tcp-local-port listener).
    ev.defer_std_host_prim("tcp-listen", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || a.size() > 2 || !types::is_int(a[0]))
            return make_void();
        const auto port = types::as_int(a[0]);
        if (port < 0 || port > 65535)
            return make_void();
        in_addr_t bind_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 denseness
        if (a.size() == 2) {
            if (!types::is_string(a[1]))
                return make_void();
            const auto hidx = types::as_string_idx(a[1]);
            if (hidx >= ev.string_heap_.size())
                return make_void();
            // Same fence as tcp-connect: the host bytes leave the heap.
            if (!ev.inrange_string_tenant_allows(hidx, aura::compiler::security::kEffectNetwork,
                                                 "tcp-listen"))
                return make_void();
            in_addr parsed{};
            if (::inet_pton(AF_INET, ev.string_at(hidx).c_str(), &parsed) != 1)
                return make_void();
            bind_addr = parsed.s_addr;
        }
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return make_void();
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(port));
        addr.sin_addr.s_addr = bind_addr;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd);
            return make_void();
        }
        if (::listen(fd, 16) < 0) {
            ::close(fd);
            return make_void();
        }
        stamp_minted_fd_tenant(ev, fd);
        return types::make_int(static_cast<std::int64_t>(fd));
    });

    // (tcp-local-port sock) → port | ()  — getsockname for ephemeral listeners.
    ev.defer_std_host_prim("tcp-local-port", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        auto fd = static_cast<int>(types::as_int(a[0]));
        // Issue #4644: the bound port of a listener is tenant state. Probe it
        // only when this tenant owns the fd; a foreign listener stays opaque.
        if (!minted_fd_tenant_allows(ev, fd, "tcp-local-port"))
            return make_void();
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0)
            return make_void();
        return types::make_int(static_cast<std::int64_t>(ntohs(addr.sin_port)));
    });

    // (tcp-accept listener) → client-fd | ()  — blocking accept.
    ev.defer_std_host_prim("tcp-accept", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        auto lfd = static_cast<int>(types::as_int(a[0]));
        // Issue #4644: refuse a foreign listener before ::accept so another
        // tenant's listen queue is never drained and the accepted client fd
        // is never stamped to this tenant.
        if (!minted_fd_tenant_allows(ev, lfd, "tcp-accept"))
            return make_void();
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int cfd = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0)
            return make_void();
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        stamp_minted_fd_tenant(ev, cfd);
        return types::make_int(static_cast<std::int64_t>(cfd));
    });

    // (tcp-accept-timeout listener sec) → client-fd | ()
    // sec is integer seconds (0 = poll once). Uses poll then accept.
    ev.defer_std_host_prim("tcp-accept-timeout", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_int(a[0]) || !types::is_int(a[1]))
            return make_void();
        auto lfd = static_cast<int>(types::as_int(a[0]));
        auto sec = types::as_int(a[1]);
        if (sec < 0)
            sec = 0;
        if (sec > 3600)
            sec = 3600;
        // Issue #4644: refuse a foreign listener before ::poll + ::accept.
        if (!minted_fd_tenant_allows(ev, lfd, "tcp-accept-timeout"))
            return make_void();
        struct pollfd pfd = {lfd, POLLIN, 0};
        int pr = ::poll(&pfd, 1, static_cast<int>(sec * 1000));
        if (pr <= 0)
            return make_void(); // timeout or error
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int cfd = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0)
            return make_void();
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        stamp_minted_fd_tenant(ev, cfd);
        return types::make_int(static_cast<std::int64_t>(cfd));
    });

    ev.defer_std_host_prim("tcp-send", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_int(a[0]) || !types::is_string(a[1]))
            return make_int(-1);
        auto fd = static_cast<int>(types::as_int(a[0]));
        auto sidx = types::as_string_idx(a[1]);
        if (sidx >= ev.string_heap_.size())
            return types::make_int(0);
        // Issue #4559: Network pays for the send, not for reading another
        // principal's stamped string into the socket buffer. In-range
        // before the fd fence so the data fence denies with its own row.
        if (!ev.inrange_string_tenant_allows(sidx, aura::compiler::security::kEffectNetwork,
                                             "tcp-send"))
            return types::make_int(-1);
        if (!minted_fd_tenant_allows(ev, fd, "tcp-send"))
            return types::make_int(-1);
        auto& data = ev.mutable_string_at(sidx);
        auto sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        return types::make_int(static_cast<std::int64_t>(sent));
    });

    ev.defer_std_host_prim("tcp-recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        auto fd = static_cast<int>(types::as_int(a[0]));
        if (!minted_fd_tenant_allows(ev, fd, "tcp-recv"))
            return make_void();
        auto maxlen = static_cast<std::size_t>(
            a.size() >= 2 && types::is_int(a[1]) ? types::as_int(a[1]) : 4096);
        if (maxlen > 65536)
            maxlen = 65536;
        std::string buf(maxlen, '\0');
        auto n = ::recv(fd, buf.data(), maxlen, 0);
        if (n <= 0)
            return make_void();
        buf.resize(static_cast<std::size_t>(n));
        auto sidx = ev.string_heap_.mint(std::move(buf));
        return types::make_string(sidx);
    });

    ev.defer_std_host_prim("tcp-close", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        const auto fd = static_cast<int>(types::as_int(a[0]));
        if (!minted_fd_tenant_allows(ev, fd, "tcp-close"))
            return make_void();
        ::close(fd);
        erase_minted_fd_tenant(ev, fd);
        return make_void();
    });
#endif // AURA_ENABLE_TCP

    // Issue #2626: terminal/TUI buffer + render present surface removed.

    // ── Issues #1329 Phase 1: minimal sys-* C++ bindings for stdlib migration ──
    // Full Phase 4 rewrites lib/std/fs.aura etc. to use these; Phase 1 lands
    // the syscall surface with capability gates (sandbox denies without cap).
    auto deny_sys = [&ev](const char* cap, const char* msg) -> EvalValue {
        // Issue #1565 / #2706: effect-level Write|Ffi check via sole public
        // require_effect gate (isolation + live mid; no direct
        // check_and_record_effect bypass).
        {
            using aura::compiler::security::kEffectWrite;
            using aura::compiler::security::kEffectFfi;
            const auto bits = static_cast<std::uint16_t>(kEffectWrite | kEffectFfi);
            if (!ev.require_effect(bits, cap)) {
                // require_effect already bumps capability_denial_count_ on deny.
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                    m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
                return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                            ev.primitive_error_counter_ptr());
            }
        }
        if (!ev.sandbox_mode() || ev.has_capability(cap) ||
            ev.has_capability(aura::compiler::security::kCapIo) ||
            ev.has_capability(aura::compiler::security::kCapWildcard))
            return make_void(); // not denied
        ev.bump_capability_denial();
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->cap_denial_total.fetch_add(1, std::memory_order_relaxed);
        return make_primitive_error(ev.string_heap_, ev.error_values_, msg,
                                    ev.primitive_error_counter_ptr());
    };

    ev.defer_std_host_prim("sys-open", [&ev, deny_sys](std::span<const EvalValue> a) -> EvalValue {
        // (sys-open path [flags]) → fd | -1
        // Issue #2487 (Option A): production hardening —
        //   - path_is_denied blocks /proc/self/mem, /dev/mem, /proc/kcore, …
        //   - fixed flags O_RDONLY | O_NOFOLLOW | O_CLOEXEC (caller flags ignored;
        //     no TOCTOU symlink follow, no arbitrary O_RDWR|O_CREAT|O_TRUNC)
        //   - no create path → mode irrelevant (0600 would apply if Option B
        //     re-enables O_CREAT with a whitelist)
        if (auto d = deny_sys(aura::compiler::security::kCapSysOpen,
                              "capability denied: sys-open required");
            !is_void(d))
            return d;
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto sidx = as_string_idx(a[0]);
        if (sidx >= ev.string_heap_.size())
            return make_int(-1);
        const auto& path = ev.string_at(sidx);
        if (aura::compiler::security::path_is_denied(path))
            return make_int(-1);
        // Issue #3802: Restricted+MT / Strict → resolve under tenant root.
        std::string resolved;
        if (!ev.check_tenant_host_path(path, resolved, "sys-open"))
            return make_int(-1);
        // Optional second arg kept for call-site compatibility
        // (sys-open path [flags]) but is never applied to ::open.
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC;
        int fd = ev.host_path_policy_active()
                     ? ev.open_tenant_host_fd(resolved, O_RDONLY | O_CLOEXEC, 0, "sys-open")
                     : ::open(resolved.c_str(), flags);
        if (fd >= 0)
            stamp_minted_fd_tenant(ev, fd);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->sys_open_calls.fetch_add(1, std::memory_order_relaxed);
        return make_int(fd);
    });

    ev.defer_std_host_prim("sys-read", [&ev, deny_sys](std::span<const EvalValue> a) -> EvalValue {
        // (sys-read fd n) → string | ""
        if (auto d = deny_sys(aura::compiler::security::kCapSysRead,
                              "capability denied: sys-read required");
            !is_void(d))
            return d;
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_string(ev.push_string_heap(""));
        int fd = static_cast<int>(as_int(a[0]));
        auto n = as_int(a[1]);
        if (n < 0 || n > 1 << 20)
            return make_string(ev.push_string_heap(""));
        // Issue #4561: same owner map as tcp-recv. Do not read a foreign
        // or unminted fd. The sys-write /proc/self/fd path check stays.
        if (!minted_fd_tenant_allows(ev, fd, "sys-read"))
            return make_string(ev.push_string_heap(""));
        std::string buf(static_cast<std::size_t>(n), '\0');
        auto got = ::read(fd, buf.data(), static_cast<std::size_t>(n));
        if (got < 0)
            buf.clear();
        else
            buf.resize(static_cast<std::size_t>(got));
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->sys_read_calls.fetch_add(1, std::memory_order_relaxed);
        return make_string(ev.push_string_heap(buf));
    });

    ev.defer_std_host_prim("sys-write", [&ev, deny_sys](std::span<const EvalValue> a) -> EvalValue {
        // (sys-write fd data) → bytes-written | -1
        if (auto d = deny_sys(aura::compiler::security::kCapSysWrite,
                              "capability denied: sys-write required");
            !is_void(d))
            return d;
        if (a.size() < 2 || !is_int(a[0]) || !is_string(a[1]))
            return make_int(-1);
        int fd = static_cast<int>(as_int(a[0]));
        auto sidx = as_string_idx(a[1]);
        if (sidx >= ev.string_heap_.size())
            return make_int(-1);
        // Issue #4559: Write pays for the fd write, not for reading another
        // principal's stamped string into the kernel buffer.
        if (!ev.inrange_string_tenant_allows(sidx, aura::compiler::security::kEffectWrite,
                                             "sys-write"))
            return make_int(-1);
        // Issue #4644: the fd owner map gates the write itself; the #3802
        // path check below only sees paths under a tenant base.
        if (!minted_fd_tenant_allows(ev, fd, "sys-write"))
            return make_int(-1);
        // Issue #3802: under Restricted+MT / Strict, if /proc/self/fd/N
        // resolves to a path under the tenant FS base, refuse cross-tenant
        // targets (sys-open remaps path args; this closes inherited fds into
        // another tenant prefix). Non-FS fds (pipe:/socket:/anon) and paths
        // outside the tenant base (tty, legacy) go through the owner map.
        {
            using aura::compiler::security::path_is_under_root;
            using aura::compiler::security::tenant_host_path_policy_active;
            using aura::compiler::security::tenant_host_roots_base;
            using aura::core::provenance::hard_capture_tenant_active;
            using aura::core::provenance::multi_tenant_env_active;
            const auto mode = ev.effect_sandbox_mode();
            const bool mt = hard_capture_tenant_active() || multi_tenant_env_active();
            if (tenant_host_path_policy_active(mode, mt)) {
                std::array<char, 64> link{};
                std::snprintf(link.data(), link.size(), "/proc/self/fd/%d", fd);
                std::array<char, 4096> buf{};
                const auto nlink = ::readlink(link.data(), buf.data(), buf.size() - 1);
                if (nlink > 0 && buf[0] == '/') {
                    buf[static_cast<std::size_t>(nlink)] = '\0';
                    const std::string_view target(buf.data(), static_cast<std::size_t>(nlink));
                    const auto base = tenant_host_roots_base();
                    if (path_is_under_root(target, base)) {
                        std::string resolved;
                        if (!ev.check_tenant_host_path(target, resolved, "sys-write"))
                            return make_int(-1);
                    }
                }
            }
        }
        const auto& s = ev.string_at(sidx);
        auto n = ::write(fd, s.data(), s.size());
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->sys_write_calls.fetch_add(1, std::memory_order_relaxed);
        return make_int(n >= 0 ? static_cast<std::int64_t>(n) : -1);
    });

    // Issue #4922: sys-open had no release path, so every minted fd leaked
    // an OS fd and a process-wide owner row until exit (fs-only tenants had
    // no way to free either). sys-close is the release half of the same
    // capability - no new cap name, effect bit, or query key - and the same
    // shape as tcp-close: the fd owner map must still name this tenant,
    // then ::close drops the OS fd and the row so the number can be
    // re-minted (and re-stamped for its new owner).
    ev.defer_std_host_prim("sys-close", [&ev, deny_sys](std::span<const EvalValue> a) -> EvalValue {
        if (auto d = deny_sys(aura::compiler::security::kCapSysOpen,
                              "capability denied: sys-open required");
            !is_void(d))
            return d;
        if (a.empty() || !is_int(a[0]))
            return make_void();
        const int fd = static_cast<int>(as_int(a[0]));
        if (!minted_fd_tenant_allows(ev, fd, "sys-close"))
            return make_void();
        ::close(fd);
        erase_minted_fd_tenant(ev, fd);
        return make_void();
    });
}

// Issue #4922: ~Evaluator hands back the fds it minted. No-op unless the fd
// owner face stamped them (Soft / Off and single-tenant Restricted never
// touch the table), and an fd whose row now names another Evaluator (the
// number was closed and re-minted) keeps its new owner.
void release_minted_fds(const Evaluator& ev) noexcept {
    auto& table = fd_tenant_table();
    for (;;) {
        int fd = -1;
        bool owned_here = false;
        {
            std::scoped_lock<std::mutex> lock(table.mu);
            const auto it = table.owned.find(&ev);
            if (it == table.owned.end())
                return;
            if (it->second.empty()) {
                table.owned.erase(it);
                return;
            }
            fd = *it->second.begin();
            it->second.erase(it->second.begin());
            const auto owner = table.owner.find(fd);
            if (owner != table.owner.end() && owner->second.ev == &ev) {
                table.owner.erase(owner);
                owned_here = true;
            }
        }
        if (owned_here)
            ::close(fd);
    }
}

} // namespace aura::compiler::primitives_detail
