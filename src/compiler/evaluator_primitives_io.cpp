// evaluator_primitives_io.cpp — P0 step 17: git:* and network primitives
// aura.compiler.evaluator module partition; registered via evaluator_primitives_registry.cpp.

module;

#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netdb.h>
#include <poll.h>
#include <fcntl.h>
#include <dlfcn.h>
#include "runtime_shared.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "primitives_meta.h"
#include "security_capabilities.h"
#include "core/arena_auto_policy_stats.h"
#include "core/gap_buffer.hh"
#include "git_ctx.h"
#include "renderer/batch_terminal.hh"
#include "renderer/render_ffi.hh"
#include "terminal_buffer_registry.hh"
#include "hash_meta.h"

#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define AURA_HAVE_CURL 1
#else
typedef void CURL;
struct curl_slist {};
using CURLcode = int;
using CURLoption = int;
constexpr CURLoption CURLOPT_URL = 10002;
constexpr CURLoption CURLOPT_POST = 47;
constexpr CURLoption CURLOPT_POSTFIELDS = 10015;
constexpr CURLoption CURLOPT_POSTFIELDSIZE = 60;
constexpr CURLoption CURLOPT_HTTPHEADER = 10023;
constexpr CURLoption CURLOPT_WRITEFUNCTION = 20011;
constexpr CURLoption CURLOPT_WRITEDATA = 10001;
constexpr CURLoption CURLOPT_TIMEOUT = 13;
constexpr CURLoption CURLOPT_CONNECTTIMEOUT = 78;
constexpr CURLoption CURLOPT_SSL_VERIFYPEER = 64;
constexpr CURLoption CURLOPT_SSL_VERIFYHOST = 81;
constexpr CURLoption CURLOPT_USERAGENT = 10018;
constexpr CURLoption CURLOPT_FOLLOWLOCATION = 52;
constexpr CURLcode CURLE_OK = 0;
#endif
#include "messaging_bridge.h"

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

// ── Runtime-loaded libcurl via dlopen (avoids ld.bfd symbol issues) ──
namespace {
    struct CurlAPI {
        void* handle = nullptr;
        CURL* (*easy_init)() = nullptr;
        CURLcode (*easy_setopt)(CURL*, CURLoption, ...) = nullptr;
        CURLcode (*easy_perform)(CURL*) = nullptr;
        void (*easy_cleanup)(CURL*) = nullptr;
        struct curl_slist* (*slist_append)(struct curl_slist*, const char*) = nullptr;
        void (*slist_free_all)(struct curl_slist*) = nullptr;

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
                auto* ei = (CURL * (*)())::dlsym(h, "curl_easy_init");
                auto* es = (CURLcode (*)(CURL*, CURLoption, ...))::dlsym(h, "curl_easy_setopt");
                auto* ep = (CURLcode (*)(CURL*))::dlsym(h, "curl_easy_perform");
                auto* ec = (void (*)(CURL*))::dlsym(h, "curl_easy_cleanup");
                auto* sa = (struct curl_slist *
                            (*)(struct curl_slist*, const char*))::dlsym(h, "curl_slist_append");
                auto* sf = (void (*)(struct curl_slist*))::dlsym(h, "curl_slist_free_all");
                if (ei && es && ep && ec && sa && sf) {
                    handle = h;
                    easy_init = ei;
                    easy_setopt = es;
                    easy_perform = ep;
                    easy_cleanup = ec;
                    slist_append = sa;
                    slist_free_all = sf;
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
    auto& curl_writer_fn() {
        static auto writer = [](char* ptr, size_t size, size_t nmemb, void* ud) -> size_t {
            static_cast<std::string*>(ud)->append(ptr, size * nmemb);
            return size * nmemb;
        };
        return writer;
    }
} // namespace


void register_git_primitives(PrimRegistrar add, Evaluator& ev) {

    // ── Git integration (Issue #96) ─────────────────────────────
    // Backed by libgit2 (in-process) when available, with popen fallback
    // for systems without libgit2. Much faster than fork+exec per call,
    // and avoids shell escape issues for commit messages and paths.

    // (git-status) → short status string (like "git status --short")
    add("git-status", [&ev](const auto&) -> EvalValue {
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
            auto* fp = ::popen("git status --short 2>/dev/null", "r");
            if (!fp)
                return make_void();
            while (::fgets(buf.data(), buf.size(), fp) != nullptr)
                result += buf.data();
            ::pclose(fp);
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    // (git-diff ["staged"]) → unified diff
    add("git-diff", [&ev](std::span<const EvalValue> a) -> EvalValue {
        bool staged = false;
        if (a.size() >= 1 && is_string(a[0])) {
            auto mi = as_string_idx(a[0]);
            if (mi < ev.string_heap_.size() && ev.string_heap_[mi] == "staged") {
                staged = true;
            }
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
            std::string cmd = staged ? "git diff --staged 2>/dev/null" : "git diff 2>/dev/null";
            std::array<char, 4096> buf;
            auto* fp = ::popen(cmd.c_str(), "r");
            if (!fp)
                return make_void();
            while (::fgets(buf.data(), buf.size(), fp) != nullptr)
                result += buf.data();
            ::pclose(fp);
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    // (git-log n) → last n commits, one-line format (n=1..1000, default 10)
    add("git-log", [&ev](std::span<const EvalValue> a) -> EvalValue {
        int n = 10;
        if (a.size() >= 1 && is_int(a[0]))
            n = static_cast<int>(as_int(a[0]));
        if (n < 1)
            n = 1;
        if (n > 1000)
            n = 1000;
        std::string result;
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
            auto* fp = ::popen(cmd.c_str(), "r");
            if (!fp)
                return make_void();
            while (::fgets(buf.data(), buf.size(), fp) != nullptr)
                result += buf.data();
            ::pclose(fp);
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    // (git-commit "message") → exit code (0 = ok, no shell escape needed)
    add("git-commit", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto mi = as_string_idx(a[0]);
        if (mi >= ev.string_heap_.size())
            return make_int(-1);
        const std::string& msg = ev.string_heap_[mi];
        int rc = -1;
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
    add("git-branch-current", [&ev](const auto&) -> EvalValue {
        std::string result;
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            result = ctx.branch_current();
        }
#endif
        if (result.empty()) {
            std::array<char, 256> buf;
            auto* fp = ::popen("git rev-parse --abbrev-ref HEAD 2>/dev/null", "r");
            if (!fp)
                return make_void();
            while (::fgets(buf.data(), buf.size(), fp) != nullptr)
                result += buf.data();
            ::pclose(fp);
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });

    // (git-stage "file1" "file2" ...) → exit code
    add("git-stage", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty())
            return make_int(-1);
        int rc = -1;
#ifdef AURA_HAVE_LIBGIT2
        std::vector<std::string> paths;
        for (const auto& v : a) {
            if (!is_string(v))
                return make_int(-1);
            auto si = as_string_idx(v);
            if (si >= ev.string_heap_.size())
                return make_int(-1);
            paths.push_back(ev.string_heap_[si]);
        }
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            rc = ctx.stage(paths);
        } else
#endif
        {
            // Issue #1161: fork+execvp — no shell (matches git-commit #473).
            std::vector<std::string> path_bufs;
            path_bufs.reserve(a.size());
            for (const auto& v : a) {
                if (!is_string(v))
                    return make_int(-1);
                auto si = as_string_idx(v);
                if (si >= ev.string_heap_.size())
                    return make_int(-1);
                path_bufs.push_back(ev.string_heap_[si]);
            }
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

    // (git-rev-parse) → current HEAD sha (short, 7 chars)
    add("git-rev-parse", [&ev](const auto&) -> EvalValue {
        std::string result;
#ifdef AURA_HAVE_LIBGIT2
        thread_local GitContext ctx;
        if (ctx.is_open()) {
            result = ctx.rev_parse_short();
        }
#endif
        if (result.empty()) {
            std::array<char, 64> buf;
            auto* fp = ::popen("git rev-parse --short HEAD 2>/dev/null", "r");
            if (!fp)
                return make_void();
            while (::fgets(buf.data(), buf.size(), fp) != nullptr)
                result += buf.data();
            ::pclose(fp);
            if (!result.empty() && result.back() == '\n')
                result.pop_back();
        }
        auto sid = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return make_string(sid);
    });
}

void register_network_primitives(PrimRegistrar add, Evaluator& ev) {

    // ── Environment + HTTP primitives ────────────────────────
    add("getenv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto name = ev.string_heap_[types::as_string_idx(a[0])];
        auto* val = ::getenv(name.c_str());
        if (!val)
            return make_void();
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::string(val));
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
    add("http-get", [&ev, shell_single_quote](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        const auto uidx = types::as_string_idx(a[0]);
        if (uidx >= ev.string_heap_.size())
            return make_void();
        const auto& url = ev.string_heap_[uidx];

        std::string result;
        // Prefer libcurl (no shell) when available.
        if (get_curl().load()) {
            CURL* curl = get_curl().easy_init();
            if (curl) {
                get_curl().easy_setopt(curl, CURLOPT_URL, url.c_str());
                get_curl().easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_writer_fn);
                get_curl().easy_setopt(curl, CURLOPT_WRITEDATA, &result);
                get_curl().easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                get_curl().easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
                get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
                get_curl().easy_setopt(curl, CURLOPT_USERAGENT, "aura/1.0");
                get_curl().easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                CURLcode res = get_curl().easy_perform(curl);
                get_curl().easy_cleanup(curl);
                if (res != CURLE_OK && result.empty())
                    return make_void();
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::move(result));
                return types::make_string(sidx);
            }
        }

        // Issue #1160: no shell fallback. Without libcurl, refuse rather
        // than interpolate the URL into popen/curl (command injection).
        (void)shell_single_quote;
        return make_void();
    });

    add("http-post", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();
        // Issue #1077: bounds-check before string_heap_ index.
        const auto uidx = types::as_string_idx(a[0]);
        const auto bidx = types::as_string_idx(a[1]);
        if (uidx >= ev.string_heap_.size() || bidx >= ev.string_heap_.size())
            return make_void();

        auto& curl_url = ev.string_heap_[uidx];
        auto& curl_body = ev.string_heap_[bidx];
        std::string auth;
        if (a.size() >= 3 && types::is_string(a[2])) {
            const auto aidx = types::as_string_idx(a[2]);
            if (aidx < ev.string_heap_.size())
                auth = ev.string_heap_[aidx];
        }

        // Async HTTP via thread (fiber-friendly, serve mode only)
        if (aura::messaging::g_http_post_async) {
            auto result = aura::messaging::g_http_post_async(curl_url, curl_body, auth);
            if (!result.empty()) {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::move(result));
                return types::make_string(sidx);
            }
        }

        // Try native libcurl (synchronous, default path)
        std::string result;
        if (get_curl().load()) {
            auto& curl_url = ev.string_heap_[types::as_string_idx(a[0])];
            auto& curl_body = ev.string_heap_[types::as_string_idx(a[1])];

            CURL* curl = get_curl().easy_init();
            if (curl) {
                struct curl_slist* headers = nullptr;
                headers = get_curl().slist_append(headers, "Content-Type: application/json");
                if (a.size() >= 3 && types::is_string(a[2])) {
                    auto& auth = ev.string_heap_[types::as_string_idx(a[2])];
                    headers = get_curl().slist_append(
                        headers, (std::string("Authorization: Bearer ") + auth).c_str());
                }

                std::string response;
                get_curl().easy_setopt(curl, CURLOPT_URL, curl_url.c_str());
                get_curl().easy_setopt(curl, CURLOPT_POST, 1L);
                get_curl().easy_setopt(curl, CURLOPT_POSTFIELDS, curl_body.c_str());
                get_curl().easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                                       static_cast<long>(curl_body.size()));
                get_curl().easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                get_curl().easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_writer_fn);
                get_curl().easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                get_curl().easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                get_curl().easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
                get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
                get_curl().easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
                get_curl().easy_setopt(curl, CURLOPT_USERAGENT, "aura/1.0");

                CURLcode res = get_curl().easy_perform(curl);
                get_curl().slist_free_all(headers);
                get_curl().easy_cleanup(curl);

                if (res == CURLE_OK && !response.empty())
                    result = std::move(response);
            }
        }

        if (result.empty()) {
            // ── fallback: pipe+fork+exec curl CLI ──
            auto& url = ev.string_heap_[types::as_string_idx(a[0])];
            auto& body = ev.string_heap_[types::as_string_idx(a[1])];
            std::string auth_hdr;
            if (a.size() >= 3 && types::is_string(a[2]))
                auth_hdr = std::string("Authorization: Bearer ") +
                           ev.string_heap_[types::as_string_idx(a[2])];
            int in[2], out[2];
            if (::pipe(in) < 0 || ::pipe(out) < 0)
                return make_void();
            pid_t pid = ::fork();
            if (pid < 0) {
                ::close(in[0]);
                ::close(in[1]);
                ::close(out[0]);
                ::close(out[1]);
                return make_void();
            }
            if (pid == 0) {
                ::close(in[1]);
                ::close(out[0]);
                ::dup2(in[0], STDIN_FILENO);
                ::dup2(out[1], STDOUT_FILENO);
                ::close(in[0]);
                ::close(out[1]);
                const char* argv[16]{};
                int i = 0;
                argv[i++] = "curl";
                argv[i++] = "-s";
                argv[i++] = "-X";
                argv[i++] = "POST";
                argv[i++] = "--data-binary";
                argv[i++] = "@-";
                argv[i++] = "-H";
                argv[i++] = "Content-Type: application/json";
                if (!auth_hdr.empty()) {
                    argv[i++] = "-H";
                    argv[i++] = auth_hdr.c_str();
                }
                argv[i++] = "--max-time";
                argv[i++] = "30";
                argv[i++] = "--connect-timeout";
                argv[i++] = "10";
                argv[i++] = url.c_str();
                argv[i] = nullptr;
                ::execvp("curl", const_cast<char* const*>(argv));
                ::_exit(1);
            }
            ::close(in[0]);
            ::close(out[1]);
            ::write(in[1], body.data(), body.size());
            ::close(in[1]);
            std::array<char, 4096> fbuf;
            ssize_t nr;
            while ((nr = ::read(out[0], fbuf.data(), fbuf.size())) > 0)
                result.append(fbuf.data(), static_cast<std::size_t>(nr));
            ::close(out[0]);
            int cstat;
            ::waitpid(pid, &cstat, 0);
        }

        if (result.empty())
            return make_void();
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return types::make_string(sidx);
    });

    // ── TCP socket primitives ────────────────────────────────
    add("tcp-connect", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_int(a[1]))
            return make_void();
        auto host = ev.string_heap_[types::as_string_idx(a[0])];
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
        return types::make_int(static_cast<std::int64_t>(fd));
    });

    add("tcp-send", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_int(a[0]) || !types::is_string(a[1]))
            return make_int(-1);
        auto fd = static_cast<int>(types::as_int(a[0]));
        auto sidx = types::as_string_idx(a[1]);
        if (sidx >= ev.string_heap_.size())
            return types::make_int(0);
        auto& data = ev.string_heap_[sidx];
        auto sent = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        return types::make_int(static_cast<std::int64_t>(sent));
    });

    add("tcp-recv", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        auto fd = static_cast<int>(types::as_int(a[0]));
        auto maxlen = static_cast<std::size_t>(
            a.size() >= 2 && types::is_int(a[1]) ? types::as_int(a[1]) : 4096);
        if (maxlen > 65536)
            maxlen = 65536;
        std::string buf(maxlen, '\0');
        auto n = ::recv(fd, buf.data(), maxlen, 0);
        if (n <= 0)
            return make_void();
        buf.resize(static_cast<std::size_t>(n));
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(buf));
        return types::make_string(sidx);
    });

    add("tcp-close", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_int(a[0]))
            return make_void();
        ::close(static_cast<int>(types::as_int(a[0])));
        return make_void();
    });

    // ── Issues #1313/#1314/#1316/#1317/#1349/#1350/#1352: terminal buffer ──
    // #1350: TermCell = Unicode + RGB/palette. #1352: lifecycle + per-buffer shared_mutex.
    // #1317: REGISTERED with RENDER_PRIMITIVE_META (perf_tier=hot, category=rendering).
    using TermCell = aura::renderer::TermCell;
    using term_registry::s_term_bufs;
    using term_registry::s_term_free_ids;
    using term_registry::s_term_registry_mtx;
    using term_registry::TermBuf;

    // Use meta-aware registration for render primitives (#1317).
    auto add_render = [&ev](const char* name, PrimFn fn, PrimMeta meta) {
        ev.primitives().add(name, std::move(fn), std::move(meta));
    };

    add_render(
        "make-terminal-buffer",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            // (make-terminal-buffer width height) → buffer-id
            if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
                return make_int(-1);
            auto w = as_int(a[0]);
            auto h = as_int(a[1]);
            if (w <= 0 || h <= 0 || w > 512 || h > 256)
                return make_int(-1);
            auto buf = std::make_unique<TermBuf>();
            buf->w = static_cast<std::int32_t>(w);
            buf->h = static_cast<std::int32_t>(h);
            buf->cells.assign(static_cast<std::size_t>(w * h), TermCell::space_palette());
            std::int64_t id = -1;
            {
                // Exclusive registry: mutate vector / freelist.
                std::unique_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (!s_term_free_ids.empty()) {
                    id = static_cast<std::int64_t>(s_term_free_ids.back());
                    s_term_free_ids.pop_back();
                    s_term_bufs[static_cast<std::size_t>(id)] = std::move(buf);
                } else {
                    id = static_cast<std::int64_t>(s_term_bufs.size());
                    s_term_bufs.push_back(std::move(buf));
                }
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->terminal_buffer_creates.fetch_add(1, std::memory_order_relaxed);
                m->terminal_buffer_live.fetch_add(1, std::memory_order_relaxed);
            }
            return make_int(id);
        },
        RENDER_PRIMITIVE_META(2, "Create terminal screen buffer (id).", "(int int) -> int"));

    // Issue #1352: (delete-terminal-buffer id) → #t/#f
    add_render(
        "delete-terminal-buffer",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            if (a.empty() || !is_int(a[0]))
                return make_bool(false);
            auto id = as_int(a[0]);
            {
                // Exclusive registry: no concurrent holder of shared reg can run.
                std::unique_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                    !s_term_bufs[static_cast<std::size_t>(id)])
                    return make_bool(false);
                s_term_bufs[static_cast<std::size_t>(id)]
                    .reset(); // tombstone; freelist via compact
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->terminal_buffer_deletes.fetch_add(1, std::memory_order_relaxed);
                const auto live = m->terminal_buffer_live.load(std::memory_order_relaxed);
                if (live > 0)
                    m->terminal_buffer_live.fetch_sub(1, std::memory_order_relaxed);
            }
            return make_bool(true);
        },
        RENDER_PRIMITIVE_META(1,
                              "Free terminal buffer by id (#1352). Slot reclaimed after compact.",
                              "(int) -> bool"));

    // Issue #1352: (compact-terminal-buffers) → reclaimed-slot-count
    // Rebuilds freelist from tombstones. Live IDs stay stable (no erase-remove renumber).
    add_render(
        "compact-terminal-buffers",
        [&ev](std::span<const EvalValue>) -> EvalValue {
            std::int64_t reclaimed = 0;
            {
                std::unique_lock<std::shared_mutex> reg(s_term_registry_mtx);
                // Drop trailing tombstones first (cannot be live IDs).
                std::int64_t trailing = 0;
                while (!s_term_bufs.empty() && !s_term_bufs.back()) {
                    s_term_bufs.pop_back();
                    ++trailing;
                }
                s_term_free_ids.clear();
                for (std::size_t i = 0; i < s_term_bufs.size(); ++i) {
                    if (!s_term_bufs[i])
                        s_term_free_ids.push_back(i);
                }
                reclaimed = trailing + static_cast<std::int64_t>(s_term_free_ids.size());
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->terminal_buffer_compacts.fetch_add(1, std::memory_order_relaxed);
            return make_int(reclaimed);
        },
        RENDER_PRIMITIVE_META(
            0, "Rebuild freelist from tombstones; shrink trailing empties (#1352).", "() -> int"));
    // (terminal-buffer-count) → live buffers (test/ops hook)
    add_render(
        "terminal-buffer-count",
        [](std::span<const EvalValue>) -> EvalValue {
            std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
            std::int64_t n = 0;
            for (const auto& p : s_term_bufs)
                if (p)
                    ++n;
            return make_int(n);
        },
        RENDER_PRIMITIVE_META(0, "Count live terminal buffers (#1352).", "() -> int"));

    add_render(
        "terminal-set-cell",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            // (terminal-set-cell buf-id x y ch [fg [bg]]) → #t/#f
            // Backward-compat 256-color palette path (#1313 / #1350).
            if (a.size() < 4 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
                return make_bool(false);
            auto id = as_int(a[0]);
            auto x = as_int(a[1]);
            auto y = as_int(a[2]);
            auto ch = static_cast<std::uint32_t>(as_int(a[3]));
            if (ch > 0x10FFFFu)
                ch = static_cast<std::uint32_t>(' ');
            auto fg = a.size() >= 5 && is_int(a[4]) ? static_cast<std::uint8_t>(as_int(a[4]) & 0xFF)
                                                    : static_cast<std::uint8_t>(7);
            auto bg = a.size() >= 6 && is_int(a[5]) ? static_cast<std::uint8_t>(as_int(a[5]) & 0xFF)
                                                    : static_cast<std::uint8_t>(0);
            // Shared registry + exclusive buffer: parallel set-cell across buffers.
            std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
            if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                !s_term_bufs[static_cast<std::size_t>(id)])
                return make_bool(false);
            auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
            std::unique_lock<std::shared_mutex> buf(b.rwlock);
            if (x < 0 || y < 0 || x >= b.w || y >= b.h)
                return make_bool(false);
            TermCell cell;
            cell.ch = ch;
            cell.fg_r = fg;
            cell.bg_r = bg;
            cell.mode = 0; // palette
            b.cells[static_cast<std::size_t>(y * b.w + x)] = cell;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                m->terminal_set_cell_total.fetch_add(1, std::memory_order_relaxed);
            return make_bool(true);
        },
        RENDER_PRIMITIVE_META(4, "Set cell ch/fg/bg at (x,y) [256-color palette].",
                              "(int int int int [int [int]]) -> bool"));

    // Issue #1350: (terminal-set-cell-rgb buf x y ch fr fg fb [br bg bb]) → bool
    add_render(
        "terminal-set-cell-rgb",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 7 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]) ||
                !is_int(a[4]) || !is_int(a[5]) || !is_int(a[6]))
                return make_bool(false);
            auto id = as_int(a[0]);
            auto x = as_int(a[1]);
            auto y = as_int(a[2]);
            auto ch = static_cast<std::uint32_t>(as_int(a[3]));
            if (ch > 0x10FFFFu)
                ch = static_cast<std::uint32_t>(' ');
            TermCell cell;
            cell.ch = ch;
            cell.fg_r = static_cast<std::uint8_t>(as_int(a[4]) & 0xFF);
            cell.fg_g = static_cast<std::uint8_t>(as_int(a[5]) & 0xFF);
            cell.fg_b = static_cast<std::uint8_t>(as_int(a[6]) & 0xFF);
            cell.bg_r =
                a.size() >= 8 && is_int(a[7]) ? static_cast<std::uint8_t>(as_int(a[7]) & 0xFF) : 0;
            cell.bg_g =
                a.size() >= 9 && is_int(a[8]) ? static_cast<std::uint8_t>(as_int(a[8]) & 0xFF) : 0;
            cell.bg_b =
                a.size() >= 10 && is_int(a[9]) ? static_cast<std::uint8_t>(as_int(a[9]) & 0xFF) : 0;
            cell.mode = 1; // rgb
            std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
            if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                !s_term_bufs[static_cast<std::size_t>(id)])
                return make_bool(false);
            auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
            std::unique_lock<std::shared_mutex> buf(b.rwlock);
            if (x < 0 || y < 0 || x >= b.w || y >= b.h)
                return make_bool(false);
            b.cells[static_cast<std::size_t>(y * b.w + x)] = cell;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->terminal_set_cell_total.fetch_add(1, std::memory_order_relaxed);
                m->terminal_set_cell_rgb_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_bool(true);
        },
        RENDER_PRIMITIVE_META(7, "Set cell with 24-bit RGB fg/bg (#1350).",
                              "(int int int int int int int [int int int]) -> bool"));

    // Issue #1350: (terminal-set-cell-unicode buf x y ch-string [fr fg fb [br bg bb]])
    add_render(
        "terminal-set-cell-unicode",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            if (a.size() < 4 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_string(a[3]))
                return make_bool(false);
            auto id = as_int(a[0]);
            auto x = as_int(a[1]);
            auto y = as_int(a[2]);
            auto sidx = as_string_idx(a[3]);
            if (sidx >= ev.string_heap_.size())
                return make_bool(false);
            const auto& s = ev.string_heap_[sidx];
            const auto cp = aura::renderer::utf8_first_codepoint(s.data(), s.size());
            TermCell cell;
            cell.ch = cp;
            if (a.size() >= 7 && is_int(a[4]) && is_int(a[5]) && is_int(a[6])) {
                cell.fg_r = static_cast<std::uint8_t>(as_int(a[4]) & 0xFF);
                cell.fg_g = static_cast<std::uint8_t>(as_int(a[5]) & 0xFF);
                cell.fg_b = static_cast<std::uint8_t>(as_int(a[6]) & 0xFF);
                cell.bg_r = a.size() >= 8 && is_int(a[7])
                                ? static_cast<std::uint8_t>(as_int(a[7]) & 0xFF)
                                : 0;
                cell.bg_g = a.size() >= 9 && is_int(a[8])
                                ? static_cast<std::uint8_t>(as_int(a[8]) & 0xFF)
                                : 0;
                cell.bg_b = a.size() >= 10 && is_int(a[9])
                                ? static_cast<std::uint8_t>(as_int(a[9]) & 0xFF)
                                : 0;
                cell.mode = 1;
            } else {
                cell.fg_r = 7;
                cell.bg_r = 0;
                cell.mode = 0;
            }
            std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
            if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                !s_term_bufs[static_cast<std::size_t>(id)])
                return make_bool(false);
            auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
            std::unique_lock<std::shared_mutex> buf(b.rwlock);
            if (x < 0 || y < 0 || x >= b.w || y >= b.h)
                return make_bool(false);
            b.cells[static_cast<std::size_t>(y * b.w + x)] = cell;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->terminal_set_cell_total.fetch_add(1, std::memory_order_relaxed);
                m->terminal_set_cell_unicode_total.fetch_add(1, std::memory_order_relaxed);
            }
            return make_bool(true);
        },
        RENDER_PRIMITIVE_META(4, "Set cell from UTF-8 string + optional RGB (#1350).",
                              "(int int int string [int int int [int int int]]) -> bool"));

    add_render(
        "terminal-diff-update",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            // (terminal-diff-update old-id new-id) → changed-cell-count
            if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
                return make_int(-1);
            auto oid = as_int(a[0]);
            auto nid = as_int(a[1]);
            std::int64_t changed = 0;
            {
                std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (oid < 0 || nid < 0 || static_cast<std::size_t>(oid) >= s_term_bufs.size() ||
                    static_cast<std::size_t>(nid) >= s_term_bufs.size() ||
                    !s_term_bufs[static_cast<std::size_t>(oid)] ||
                    !s_term_bufs[static_cast<std::size_t>(nid)])
                    return make_int(-1);
                auto& o = *s_term_bufs[static_cast<std::size_t>(oid)];
                auto& n = *s_term_bufs[static_cast<std::size_t>(nid)];
                // Lock order by id to avoid deadlock on same-buffer pairs.
                std::shared_lock<std::shared_mutex> lo1(oid <= nid ? o.rwlock : n.rwlock);
                std::shared_lock<std::shared_mutex> lo2(oid <= nid ? n.rwlock : o.rwlock,
                                                        std::defer_lock);
                if (oid != nid)
                    lo2.lock();
                if (o.w != n.w || o.h != n.h)
                    changed = static_cast<std::int64_t>(n.cells.size()); // full refresh
                else {
                    for (std::size_t i = 0; i < o.cells.size(); ++i) {
                        if (o.cells[i] != n.cells[i])
                            ++changed;
                    }
                }
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->terminal_diff_updates.fetch_add(1, std::memory_order_relaxed);
                m->terminal_diff_cells_total.fetch_add(
                    static_cast<std::uint64_t>(changed > 0 ? changed : 0),
                    std::memory_order_relaxed);
            }
            return make_int(changed);
        },
        RENDER_PRIMITIVE_META(2, "Count changed cells between two buffers.", "(int int) -> int"));

    // Shared frame builder for present-batch and frame-ansi (#1349/#1350).
    auto build_present_frame = [](const TermBuf& b, std::string& out) -> std::uint64_t {
        return aura::renderer::build_terminal_frame_ansi(out, b.w, b.h, b.cells.data());
    };

    add_render(
        "terminal-present-batch",
        [&ev, build_present_frame](std::span<const EvalValue> a) -> EvalValue {
            // Issue #1314/#1316/#1349: (terminal-present-batch buf-id [fd]) → bytes-written
            if (a.empty() || !is_int(a[0]))
                return make_int(-1);
            auto id = as_int(a[0]);
            int fd = 1; // stdout
            if (a.size() >= 2 && is_int(a[1]))
                fd = static_cast<int>(as_int(a[1]));
            ev.enter_render_hotpath();
            std::string out;
            std::uint64_t sgr_emits = 0;
            std::int32_t rows = 0;
            {
                std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                    !s_term_bufs[static_cast<std::size_t>(id)]) {
                    ev.exit_render_hotpath();
                    return make_int(-1);
                }
                auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
                std::shared_lock<std::shared_mutex> buf(b.rwlock);
                rows = b.h;
                sgr_emits = build_present_frame(b, out);
            }
            auto n = ::write(fd, out.data(), out.size());
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->terminal_present_batch_total.fetch_add(1, std::memory_order_relaxed);
                m->terminal_present_bytes_total.fetch_add(n > 0 ? static_cast<std::uint64_t>(n) : 0,
                                                          std::memory_order_relaxed);
                m->terminal_present_sgr_emits_total.fetch_add(sgr_emits, std::memory_order_relaxed);
                m->terminal_present_csi_h_rows_total.fetch_add(
                    static_cast<std::uint64_t>(rows > 0 ? rows : 0), std::memory_order_relaxed);
                m->terminal_present_sync_frames_total.fetch_add(1, std::memory_order_relaxed);
                m->render_hotpath_samples.fetch_add(1, std::memory_order_relaxed);
                m->render_jit_aot_prefer_hits.fetch_add(1, std::memory_order_relaxed);
            }
            ev.exit_render_hotpath();
            return make_int(n > 0 ? static_cast<std::int64_t>(n) : 0);
        },
        RENDER_PRIMITIVE_META(1, "Present terminal buffer as ANSI SGR/CSI frame (#1349).",
                              "(int [int]) -> int"));

    add_render(
        "terminal-present",
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            auto r = ev.primitives().lookup("terminal-present-batch");
            if (!r)
                return make_int(-1);
            return (*r)(a);
        },
        RENDER_PRIMITIVE_META(1, "Alias of terminal-present-batch.", "(int [int]) -> int"));

    // Issue #1349: (terminal-frame-ansi buf-id) → string
    add_render(
        "terminal-frame-ansi",
        [&ev, build_present_frame](std::span<const EvalValue> a) -> EvalValue {
            auto push_str = [&ev](std::string s) -> EvalValue {
                auto sidx = ev.string_heap_.size();
                ev.string_heap_.push_back(std::move(s));
                return make_string(sidx);
            };
            if (a.empty() || !is_int(a[0]))
                return push_str("");
            auto id = as_int(a[0]);
            std::string out;
            {
                std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                    !s_term_bufs[static_cast<std::size_t>(id)])
                    return push_str("");
                auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
                std::shared_lock<std::shared_mutex> buf(b.rwlock);
                (void)build_present_frame(b, out);
            }
            return push_str(std::move(out));
        },
        RENDER_PRIMITIVE_META(1, "Return ANSI frame string for buffer (no write) (#1349).",
                              "(int) -> string"));

    // ── Issue #1316: render JIT stability probe + stats ──
    add_render(
        "render-jit-deopt-probe",
        [&ev](std::span<const EvalValue>) -> EvalValue {
            // Agent/test hook: simulate deopt under render hot path (throttled).
            ev.enter_render_hotpath();
            ev.bump_render_jit_deopt_throttled();
            ev.exit_render_hotpath();
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
                return make_int(static_cast<std::int64_t>(
                    m->render_jit_deopt_applied.load(std::memory_order_relaxed)));
            return make_int(0);
        },
        RENDER_PRIMITIVE_META(0, "Probe throttled render JIT deopt (returns applied count).",
                              "() -> int"));

    ObservabilityPrims::register_stats_impl(
        "query:render-jit-stability-stats", [&ev](std::span<const EvalValue>) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            if (m)
                m->render_obs_query_hits.fetch_add(1, std::memory_order_relaxed);
            auto load = [](const std::atomic<std::uint64_t>& a) {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            // Return compact string for Phase 1 (hash via production-sweep for structured).
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(
                std::format("deopt_applied={} throttled={} aot_prefer={} window_ms={} schema=1316",
                            m ? load(m->render_jit_deopt_applied) : 0,
                            m ? load(m->render_jit_deopt_throttled) : 0,
                            m ? load(m->render_jit_aot_prefer_hits) : 0,
                            m ? load(m->render_deopt_throttle_window_ms) : 500));
            return types::make_string(sidx);
        });

    // ── Issue #1354: query:render-ffi-available ──
    // Agent discovery: registered bindings + hot-path dispatch stats.
    ObservabilityPrims::register_stats_impl(
        "query:render-ffi-available", [&ev](std::span<const EvalValue>) -> EvalValue {
            auto& reg = aura::renderer::ffi::render_ffi_registry();
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            if (m) {
                m->render_obs_query_hits.fetch_add(1, std::memory_order_relaxed);
                m->render_ffi_registered.store(reg.registered.load(std::memory_order_relaxed),
                                               std::memory_order_relaxed);
                m->render_ffi_hot_path_dispatches.store(
                    reg.hot_path_dispatches.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m->render_ffi_hotpath_enter_total.store(
                    reg.ffi_hotpath_enter_total.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                m->render_ffi_bind_success.store(reg.bind_success.load(std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
            }
            auto* ht = FlatHashTable::create(32) /* #1141 */;
            if (!ht)
                return make_void();
            auto insert_kv = [&](const char* k_str, std::int64_t v) {
                (void)primitives_detail::flat_hash_insert_cstr_i64(ht, ev.string_heap_, k_str, v,
                                                                   make_string, make_int);
            };
            insert_kv("schema", 1354);
            insert_kv("active", 1);
            insert_kv("phase", static_cast<std::int64_t>(aura::renderer::ffi::kRenderFfiPhase));
            insert_kv("registered",
                      static_cast<std::int64_t>(reg.registered.load(std::memory_order_relaxed)));
            insert_kv("hot-path-dispatches", static_cast<std::int64_t>(reg.hot_path_dispatches.load(
                                                 std::memory_order_relaxed)));
            insert_kv("bind-success",
                      static_cast<std::int64_t>(reg.bind_success.load(std::memory_order_relaxed)));
            insert_kv("bind-attempts",
                      static_cast<std::int64_t>(reg.bind_attempts.load(std::memory_order_relaxed)));
            insert_kv("resolve-hits",
                      static_cast<std::int64_t>(reg.resolve_hits.load(std::memory_order_relaxed)));
            insert_kv("ffi-hotpath-enter",
                      static_cast<std::int64_t>(
                          reg.ffi_hotpath_enter_total.load(std::memory_order_relaxed)));
            // Per-binding call totals (sum of call_count)
            std::int64_t calls = 0;
            for (const auto& snap : reg.snapshot())
                calls += snap.call_count;
            insert_kv("binding-calls", calls);
            auto hidx = g_hash_tables.size();
            g_hash_tables.push_back(ht);
            return make_hash(hidx);
        });

    // ── Issue #1317: terminal-diff-stats ──
    ObservabilityPrims::register_stats_impl(
        "query:terminal-diff-stats", [&ev](std::span<const EvalValue>) -> EvalValue {
            auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
            if (m) {
                m->terminal_diff_stats_queries.fetch_add(1, std::memory_order_relaxed);
                m->render_obs_query_hits.fetch_add(1, std::memory_order_relaxed);
            }
            auto load = [](const std::atomic<std::uint64_t>& a) {
                return static_cast<std::int64_t>(a.load(std::memory_order_relaxed));
            };
            auto sidx = ev.string_heap_.size();
            ev.string_heap_.push_back(std::format("updates={} cells={} schema=1317",
                                                  m ? load(m->terminal_diff_updates) : 0,
                                                  m ? load(m->terminal_diff_cells_total) : 0));
            return types::make_string(sidx);
        });

    // ── Issue #1319: gap-buffer structural mutate exercise ──
    add("gap-buffer-structural-mutate-demo", [&ev](std::span<const EvalValue> a) -> EvalValue {
        // (gap-buffer-structural-mutate-demo [n]) → ops-completed
        // Exercises GapBuffer O(1) insert/erase path and records hits.
        std::int64_t n = 64;
        if (!a.empty() && is_int(a[0]))
            n = as_int(a[0]);
        if (n < 1)
            n = 1;
        if (n > 10000)
            n = 10000;
        aura::ast::GapBuffer<std::uint32_t> gb;
        for (std::int64_t i = 0; i < n; ++i)
            gb.push_back(static_cast<std::uint32_t>(i));
        for (std::int64_t i = 0; i < n / 2; ++i)
            gb.erase(static_cast<std::size_t>(i % std::max<std::int64_t>(1, gb.size())));
        for (std::int64_t i = 0; i < n / 4; ++i)
            gb.insert(0, static_cast<std::uint32_t>(i + 1000));
        const auto hits =
            aura::ast::g_gap_buffer_structural_mutate_hits.load(std::memory_order_relaxed);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->gap_buffer_structural_mutate_hits.store(hits, std::memory_order_relaxed);
            m->gap_buffer_insert_total.store(
                aura::ast::g_gap_buffer_insert_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->gap_buffer_erase_total.store(
                aura::ast::g_gap_buffer_erase_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        }
        return make_int(static_cast<std::int64_t>(gb.size()));
    });

    // ── Issues #1329 Phase 1: minimal sys-* C++ bindings for stdlib migration ──
    // Full Phase 4 rewrites lib/std/fs.aura etc. to use these; Phase 1 lands
    // the syscall surface with capability gates (sandbox denies without cap).
    auto deny_sys = [&ev](const char* cap, const char* msg) -> EvalValue {
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

    add("sys-open", [&ev, deny_sys](std::span<const EvalValue> a) -> EvalValue {
        // (sys-open path [flags]) → fd | -1
        if (auto d = deny_sys(aura::compiler::security::kCapSysOpen,
                              "capability denied: sys-open required");
            !is_void(d))
            return d;
        if (a.empty() || !is_string(a[0]))
            return make_int(-1);
        auto sidx = as_string_idx(a[0]);
        if (sidx >= ev.string_heap_.size())
            return make_int(-1);
        int flags = O_RDONLY;
        if (a.size() >= 2 && is_int(a[1]))
            flags = static_cast<int>(as_int(a[1]));
        int fd = ::open(ev.string_heap_[sidx].c_str(), flags, 0644);
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->sys_open_calls.fetch_add(1, std::memory_order_relaxed);
        return make_int(fd);
    });

    add("sys-read", [&ev, deny_sys](std::span<const EvalValue> a) -> EvalValue {
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

    add("sys-write", [&ev, deny_sys](std::span<const EvalValue> a) -> EvalValue {
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
        const auto& s = ev.string_heap_[sidx];
        auto n = ::write(fd, s.data(), s.size());
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics()))
            m->sys_write_calls.fetch_add(1, std::memory_order_relaxed);
        return make_int(n >= 0 ? static_cast<std::int64_t>(n) : -1);
    });
}

} // namespace aura::compiler::primitives_detail
