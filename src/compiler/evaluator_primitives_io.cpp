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
#include "git_ctx.h"

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
            // Fallback: popen with single-quote escaping (safe for libgit2 path)
            std::string cmd = "git add";
            for (const auto& v : a) {
                if (!is_string(v))
                    return make_int(-1);
                auto si = as_string_idx(v);
                if (si >= ev.string_heap_.size())
                    return make_int(-1);
                cmd += " \'";
                cmd += ev.string_heap_[si];
                cmd += "\'";
            }
            cmd += " 2>/dev/null";
            rc = ::system(cmd.c_str());
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

    // ── HTTP primitives (via curl CLI) ─────────────────────
    add("http-get", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !types::is_string(a[0]))
            return make_void();
        auto url = ev.string_heap_[types::as_string_idx(a[0])];
        std::string cmd = "curl -s -f \"" + url + "\" 2>/dev/null";
        std::array<char, 4096> buf;
        std::string result;
        auto fp = ::popen(cmd.c_str(), "r");
        if (!fp)
            return make_void();
        while (::fgets(buf.data(), static_cast<int>(buf.size()), fp))
            result += buf.data();
        auto rc = ::pclose(fp);
        if (rc != 0 && result.empty())
            return make_void();
        auto sidx = ev.string_heap_.size();
        ev.string_heap_.push_back(std::move(result));
        return types::make_string(sidx);
    });

    add("http-post", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !types::is_string(a[0]) || !types::is_string(a[1]))
            return make_void();

        auto& curl_url = ev.string_heap_[types::as_string_idx(a[0])];
        auto& curl_body = ev.string_heap_[types::as_string_idx(a[1])];
        std::string auth;
        if (a.size() >= 3 && types::is_string(a[2]))
            auth = ev.string_heap_[types::as_string_idx(a[2])];

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
}

} // namespace aura::compiler::primitives_detail
