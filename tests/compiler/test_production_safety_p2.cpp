// Issue #1097/#1104/#1122 (#1978 renamed): issue# moved from filename to header.
// test_production_safety_1097_1122.cpp — Issues #1097–#1122 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "core/sandbox.hh"
#include "reflect/cache_format.h"

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;
using aura::compiler::types::make_int;
using aura::compiler::types::make_string;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(aura::test::aura_href_expr(q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::string slurp_repo(const char* rel) {
    std::ifstream in(std::string(AURA_SOURCE_DIR) + "/" + rel);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string find_aura_bin() {
    std::array<char, 4096> buf{};
    const ssize_t got = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (got > 0) {
        const std::string self(buf.data(), static_cast<std::size_t>(got));
        const auto slash = self.rfind('/');
        if (slash != std::string::npos) {
            const std::string cand = self.substr(0, slash + 1) + "aura";
            if (::access(cand.c_str(), X_OK) == 0)
                return cand;
        }
    }
    if (const char* env = std::getenv("AURA_BIN"); env != nullptr && ::access(env, X_OK) == 0)
        return env;
    for (const char* cand : {"./aura", "./build/aura", "../build/aura"}) {
        if (::access(cand, X_OK) == 0)
            return cand;
    }
    return {};
}

// Spawn `aura --emit-binary <out>` with the source on stdin. The output
// path is one argv element (no shell). Stderr is captured. Deadline is
// 90 seconds so a cold LLVM emit can finish.
int emit_to_path(const std::string& bin, const std::string& out_path, std::string_view source,
                 std::string& err_out) {
    const char* prior_runtime = std::getenv("AURA_RUNTIME_DIR");
    const std::string saved_runtime = prior_runtime != nullptr ? prior_runtime : "";
    const bool had_runtime = prior_runtime != nullptr;
    const std::string runtime_dir = std::string(AURA_SOURCE_DIR) + "/lib";
    ::setenv("AURA_RUNTIME_DIR", runtime_dir.c_str(), 1);
    std::array<int, 2> in_pipe{-1, -1};
    std::array<int, 2> err_pipe{-1, -1};
    if (::pipe2(in_pipe.data(), O_CLOEXEC) != 0)
        return -1;
    if (::pipe2(err_pipe.data(), O_CLOEXEC) != 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        return -1;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        if (::dup2(in_pipe[0], STDIN_FILENO) < 0 || ::dup2(err_pipe[1], STDERR_FILENO) < 0)
            ::_exit(127);
        const int devnull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            if (devnull != STDOUT_FILENO)
                ::close(devnull);
        }
        if (in_pipe[0] != STDIN_FILENO)
            ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(err_pipe[0]);
        if (err_pipe[1] != STDERR_FILENO)
            ::close(err_pipe[1]);
        std::vector<char> bin_buf(bin.begin(), bin.end());
        bin_buf.push_back('\0');
        std::vector<char> out_buf(out_path.begin(), out_path.end());
        out_buf.push_back('\0');
        char flag[] = "--emit-binary";
        std::vector<char*> argv{bin_buf.data(), flag, out_buf.data(), nullptr};
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(in_pipe[0]);
    ::close(err_pipe[1]);
    std::string_view pending(source);
    bool input_open = true;
    bool err_open = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (err_open && std::chrono::steady_clock::now() < deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int timeout_ms =
            left.count() > 0 ? static_cast<int>(std::min<std::int64_t>(left.count(), 5000)) : 1;
        std::array<pollfd, 2> slots{};
        nfds_t slot_count = 0;
        int input_slot = -1;
        if (input_open) {
            slots[slot_count].fd = in_pipe[1];
            slots[slot_count].events = POLLOUT;
            input_slot = static_cast<int>(slot_count);
            ++slot_count;
        }
        slots[slot_count].fd = err_pipe[0];
        slots[slot_count].events = POLLIN;
        const int err_slot = static_cast<int>(slot_count);
        ++slot_count;
        const int polled = ::poll(slots.data(), slot_count, timeout_ms);
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (polled == 0)
            break;
        const short err_events = slots[static_cast<std::size_t>(err_slot)].revents;
        if ((err_events & (POLLIN | POLLHUP | POLLERR)) != 0) {
            std::array<char, 1024> buf{};
            const ssize_t got = ::read(err_pipe[0], buf.data(), buf.size());
            if (got > 0) {
                if (err_out.size() < 8192)
                    err_out.append(buf.data(), static_cast<std::size_t>(got));
            } else if (got == 0 || errno != EINTR) {
                err_open = false;
            }
        }
        if (input_open && input_slot >= 0) {
            const short input_events = slots[static_cast<std::size_t>(input_slot)].revents;
            if ((input_events & (POLLOUT | POLLERR | POLLHUP)) != 0) {
                if (pending.empty() || (input_events & (POLLERR | POLLHUP)) != 0) {
                    ::close(in_pipe[1]);
                    in_pipe[1] = -1;
                    input_open = false;
                } else {
                    const ssize_t wrote = ::write(in_pipe[1], pending.data(), pending.size());
                    if (wrote > 0)
                        pending.remove_prefix(static_cast<std::size_t>(wrote));
                    else if (wrote < 0 && errno != EINTR) {
                        ::close(in_pipe[1]);
                        in_pipe[1] = -1;
                        input_open = false;
                    }
                    if (pending.empty() && input_open) {
                        ::close(in_pipe[1]);
                        in_pipe[1] = -1;
                        input_open = false;
                    }
                }
            }
        }
    }
    if (input_open && in_pipe[1] >= 0)
        ::close(in_pipe[1]);
    ::close(err_pipe[0]);
    int status = 0;
    pid_t reaped = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        reaped = ::waitpid(pid, &status, WNOHANG);
        if (reaped != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (reaped <= 0) {
        if (reaped == 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
        }
        if (had_runtime)
            ::setenv("AURA_RUNTIME_DIR", saved_runtime.c_str(), 1);
        else
            ::unsetenv("AURA_RUNTIME_DIR");
        return -1;
    }
    if (had_runtime)
        ::setenv("AURA_RUNTIME_DIR", saved_runtime.c_str(), 1);
    else
        ::unsetenv("AURA_RUNTIME_DIR");
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

void check_5214_source() {
    std::println("\n--- #5214: shell and bearer argv are gone from the sources ---");
    const std::string bridge = slurp_repo("src/compiler/aura_jit_bridge.cpp");
    const std::string io = slurp_repo("src/compiler/evaluator_primitives_io.cpp");
    const std::string file = slurp_repo("src/compiler/evaluator_primitives_file.cpp");
    CHECK(!bridge.empty() && bridge.find("::system(") == std::string::npos,
          "5214: aura_jit_bridge has no ::system(");
    CHECK(!bridge.empty() && bridge.find("::popen(") == std::string::npos,
          "5214: aura_jit_bridge has no ::popen(");
    const auto post = io.find("defer_std_host_prim(\"http-post\"");
    const auto tcp = io.find("\"tcp-connect\"", post == std::string::npos ? 0 : post);
    const std::string body =
        (post != std::string::npos && tcp != std::string::npos) ? io.substr(post, tcp - post) : "";
    CHECK(body.find("\"--proto\"") != std::string::npos &&
              body.find("\"--proto-redir\"") != std::string::npos,
          "5214: http-post keeps --proto and --proto-redir");
    CHECK(body.find("\"--url\"") != std::string::npos &&
              body.find("\"@/dev/fd/\"") != std::string::npos,
          "5214: bearer header goes through @/dev/fd and the URL through --url");
    CHECK(body.find("auth_hdr.c_str()") == std::string::npos,
          "5214: bearer is not an argv element");
    CHECK(body.find("!curl_loaded") != std::string::npos,
          "5214: CLI fallback runs only when libcurl did not load");
    CHECK(io.find("::pipe2(") != std::string::npos, "5214: jailed git capture uses pipe2");
    CHECK(file.find("::pipe2(") != std::string::npos, "5214: command-output capture uses pipe2");
}

void check_5214_http_post_once() {
    std::println("\n--- #5214: empty libcurl body is a single POST ---");
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0, "5214: localhost listener");
    if (listener < 0)
        return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 2) != 0) {
        ::close(listener);
        CHECK(false, "5214: bind localhost");
        return;
    }
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &bound_len);
    const int port = ntohs(bound.sin_port);
    std::atomic<int> hits{0};
    std::string request;
    std::thread server([&] {
        for (int turn = 0; turn < 2; ++turn) {
            pollfd ready{};
            ready.fd = listener;
            ready.events = POLLIN;
            const int waited = ::poll(&ready, 1, turn == 0 ? 15000 : 400);
            if (waited <= 0)
                break;
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0)
                break;
            hits.fetch_add(1);
            std::array<char, 2048> buf{};
            const ssize_t got = ::recv(client, buf.data(), buf.size(), 0);
            if (got > 0 && request.empty())
                request.assign(buf.data(), static_cast<std::size_t>(got));
            const char response[] =
                "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            (void)::send(client, response, sizeof(response) - 1, 0);
            ::close(client);
        }
        ::close(listener);
    });
    const auto saved = aura::core::sandbox::snapshot_sandbox_authority_stats().current_mode;
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        (void)ev.ensure_std_host_prims("std/net");
        auto http_post = ev.primitives().lookup("http-post");
        CHECK(http_post.has_value(), "5214: http-post is installed");
        if (http_post) {
            auto& heap = ev.string_heap_mut();
            const auto url_idx =
                heap.mint(std::string("http://127.0.0.1:") + std::to_string(port) + "/once");
            const auto body_idx = heap.mint(std::string("{\"n\":1}"));
            const auto auth_idx = heap.mint(std::string("sekret-token-5214"));
            const auto value =
                (*http_post)({make_string(url_idx), make_string(body_idx), make_string(auth_idx)});
            CHECK(is_string(value) && !is_error(value),
                  "5214: empty body is an empty string, still one POST");
            if (is_string(value))
                CHECK(ev.string_at(as_string_idx(value)).empty(), "5214: empty body stays empty");
        }
    }
    aura::core::sandbox::set_mode(static_cast<aura::core::sandbox::SandboxMode>(saved));
    server.join();
    CHECK(hits.load() == 1, "5214: libcurl empty body posted once");
    CHECK(request.find("aura/1.0") != std::string::npos,
          "5214: the single POST is the in-process libcurl path");
    CHECK(request.find("sekret-token-5214") != std::string::npos,
          "5214: bearer still reaches the wire");
}

void check_5214_emit_metacharacters() {
    std::println("\n--- #5214: emit-binary output path is not a shell command ---");
    const std::string bin = find_aura_bin();
    CHECK(!bin.empty(), "5214: aura binary found");
    if (bin.empty())
        return;
    std::array<char, 64> tmpl{};
    const char pattern[] = "/tmp/aura5214-XXXXXX";
    std::memcpy(tmpl.data(), pattern, sizeof(pattern));
    if (::mkdtemp(tmpl.data()) == nullptr) {
        CHECK(false, "5214: mkdtemp");
        return;
    }
    const std::string dir = tmpl.data();
    // The filename itself holds the metacharacters. A slash would be a
    // directory separator, so the marker names are relative: a shell
    // rewrite would create them in the process cwd.
    const std::string semi = "aura5214-semi-" + std::to_string(::getpid());
    const std::string subst = "aura5214-subst-" + std::to_string(::getpid());
    ::unlink(semi.c_str());
    ::unlink(subst.c_str());
    const std::string out = dir + "/b in;touch " + semi + " $(touch " + subst + ")";
    std::string err;
    const int code = emit_to_path(bin, out, "(display (+ 1 2))\n", err);
    const bool semi_ran = ::access(semi.c_str(), F_OK) == 0;
    const bool subst_ran = ::access(subst.c_str(), F_OK) == 0;
    const bool wrote = ::access(out.c_str(), F_OK) == 0;
    if (!wrote || semi_ran || subst_ran || code != 0)
        std::println(stderr, "5214 emit rc {} err [{}]", code, err);
    CHECK(!semi_ran, "5214: semicolon in the output name did not run");
    CHECK(!subst_ran, "5214: command substitution in the output name did not run");
    CHECK(wrote && code == 0, "5214: literal output path was written");
    ::unlink(out.c_str());
    ::unlink(semi.c_str());
    ::unlink(subst.c_str());
    ::rmdir(dir.c_str());
}

struct EnvGuard {
    std::string key;
    bool had = false;
    std::string old;
    explicit EnvGuard(const char* k)
        : key(k) {
        if (const char* v = std::getenv(k)) {
            had = true;
            old = v;
        }
    }
    void set(const char* v) const { ::setenv(key.c_str(), v, 1); }
    void clear() const { ::unsetenv(key.c_str()); }
    ~EnvGuard() {
        if (had)
            ::setenv(key.c_str(), old.c_str(), 1);
        else
            ::unsetenv(key.c_str());
    }
};

struct SandboxOff {
    int saved;
    SandboxOff()
        : saved(static_cast<int>(
              aura::core::sandbox::snapshot_sandbox_authority_stats().current_mode)) {
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    }
    ~SandboxOff() {
        aura::core::sandbox::set_mode(static_cast<aura::core::sandbox::SandboxMode>(saved));
    }
};

int listen_loopback(int& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(bound.sin_port);
    return fd;
}

std::size_t http_content_length(std::string_view head) {
    auto pos = head.find("Content-Length:");
    if (pos == std::string_view::npos)
        pos = head.find("content-length:");
    if (pos == std::string_view::npos)
        return 0;
    pos = head.find(':', pos);
    if (pos == std::string_view::npos)
        return 0;
    std::size_t n = 0;
    for (std::size_t i = pos + 1; i < head.size(); ++i) {
        const char c = head[i];
        if (c == ' ' || c == '\t')
            continue;
        if (c < '0' || c > '9')
            break;
        n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    return n;
}

std::string read_http_request(int fd) {
    std::string req;
    while (req.size() < (1u << 20)) {
        std::array<char, 4096> buf{};
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        req.append(buf.data(), static_cast<std::size_t>(n));
        const auto hdr = req.find("\r\n\r\n");
        if (hdr == std::string::npos)
            continue;
        const auto need = hdr + 4 + http_content_length(std::string_view(req).substr(0, hdr));
        if (req.size() >= need)
            break;
    }
    return req;
}

struct HttpCapture {
    std::atomic<int> hits{0};
    std::mutex mu;
    std::string request;
    std::string body;
    int delay_ms = 0;
};

void run_http_server(int listener, const std::shared_ptr<HttpCapture>& cap) {
    for (int turn = 0; turn < 2; ++turn) {
        pollfd ready{};
        ready.fd = listener;
        ready.events = POLLIN;
        if (::poll(&ready, 1, turn == 0 ? 15000 : 400) <= 0)
            break;
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0)
            break;
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        cap->hits.fetch_add(1);
        const std::string req = read_http_request(client);
        {
            std::scoped_lock lock(cap->mu);
            if (cap->request.empty())
                cap->request = req;
        }
        if (cap->delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(cap->delay_ms));
        const std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(cap->body.size()) + "\r\nConnection: close\r\n\r\n" + cap->body;
        (void)::send(client, resp.data(), resp.size(), MSG_NOSIGNAL);
        ::close(client);
    }
    ::close(listener);
}

template <class Fn> void with_http(std::string body, int delay_ms, Fn&& fn) {
    auto cap = std::make_shared<HttpCapture>();
    cap->body = std::move(body);
    cap->delay_ms = delay_ms;
    int port = 0;
    const int listener = listen_loopback(port);
    CHECK(listener >= 0, "5267: localhost listener");
    if (listener < 0)
        return;
    std::thread server(run_http_server, listener, cap);
    fn(port, *cap);
    server.join();
}

template <class Ev> std::string heap_string(Ev& ev, const aura::compiler::types::EvalValue& v) {
    if (!is_string(v))
        return {};
    return std::string(ev.string_at(as_string_idx(v)));
}

std::string eval_string(CompilerService& cs, std::string_view expr, const char* what) {
    auto r = cs.eval(expr);
    if (!r || !is_string(*r)) {
        if (!r)
            std::println(stderr, "{}: {}", what, r.error().message);
        CHECK(false, what);
        return {};
    }
    return heap_string(cs.evaluator(), *r);
}

void check_http_body_and_timeout() {
    std::println("\n--- #5267/#5268: http body, timeout, llm ---");
    const std::string io = slurp_repo("src/compiler/evaluator_primitives_io.cpp");
    CHECK(io.find("curl_write_callback()") != std::string::npos,
          "5267: WRITEFUNCTION is the callback pointer");
    CHECK(io.find("CURLOPT_WRITEFUNCTION, curl_writer_fn") == std::string::npos,
          "5267: the accessor is not passed as the writer");
    CHECK(io.find("CURLOPT_TIMEOUT, 30L") == std::string::npos, "5268: 30s timeout is gone");
    CHECK(io.find("AURA_HTTP_TIMEOUT") != std::string::npos, "5268: AURA_HTTP_TIMEOUT");
    CHECK(io.find("kHttpTimeoutDefaultSec = 120") != std::string::npos, "5268: default 120s");
    const std::string llm = slurp_repo("lib/std/llm.aura");
    CHECK(llm.find("LLM_TIMEOUT") != std::string::npos, "5268: LLM_TIMEOUT");
    CHECK(llm.find("truncated/finish=") != std::string::npos, "5268: truncated finish");

    EnvGuard runtime("AURA_RUNTIME_DIR");
    runtime.set((std::string(AURA_SOURCE_DIR) + "/lib").c_str());
    EnvGuard http_to("AURA_HTTP_TIMEOUT");
    http_to.clear();
    EnvGuard llm_to("LLM_TIMEOUT");
    llm_to.clear();
    EnvGuard llm_key("LLM_API_KEY");
    llm_key.clear();
    EnvGuard llm_base("LLM_BASE_URL");
    llm_base.clear();
    EnvGuard llm_model("LLM_MODEL");
    llm_model.clear();

    const std::string json =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"(seq ok)\"}}]}";
    SandboxOff sandbox;
    {
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(0);
        (void)ev.ensure_std_host_prims("std/net");
        auto http_post = ev.primitives().lookup("http-post");
        auto http_get = ev.primitives().lookup("http-get");
        CHECK(http_post.has_value() && http_get.has_value(),
              "5267: http-get and http-post installed");
        if (http_post && http_get) {
            with_http(json, 0, [&](int port, HttpCapture& cap) {
                auto& heap = ev.string_heap_mut();
                const auto url = heap.mint(std::string("http://127.0.0.1:") + std::to_string(port) +
                                           "/chat/completions");
                const auto body = heap.mint(std::string("{\"model\":\"x\",\"messages\":[]}"));
                const auto auth = heap.mint(std::string("TESTKEY_NOT_REAL"));
                const auto posted =
                    (*http_post)({make_string(url), make_string(body), make_string(auth)});
                CHECK(is_string(posted) && !is_error(posted), "5267: http-post returns the body");
                CHECK(heap_string(ev, posted) == json, "5267: http-post body is the JSON");
                const auto got = (*http_get)({make_string(url)});
                CHECK(is_string(got) && heap_string(ev, got) == json,
                      "5267: http-get returns the body");
                CHECK(cap.hits.load() == 2, "5267: one GET and one POST");
                CHECK(cap.request.find("aura/1.0") != std::string::npos,
                      "5267: libcurl user agent");
                CHECK(cap.request.find("TESTKEY_NOT_REAL") != std::string::npos, "5267: bearer");
            });

            with_http(json, 2000, [&](int port, HttpCapture& cap) {
                auto& heap = ev.string_heap_mut();
                const auto url =
                    heap.mint(std::string("http://127.0.0.1:") + std::to_string(port) + "/slow");
                const auto body = heap.mint(std::string("{}"));
                const auto auth = heap.mint(std::string("k"));
                const auto slow = (*http_post)(
                    {make_string(url), make_string(body), make_string(auth), make_int(1)});
                CHECK(is_error(slow) && !is_string(slow), "5268: 1s timeout is an error");
                const auto msg = ev.soft_error_message(slow);
                CHECK(msg.find("timeout") != std::string::npos, "5268: timeout text: " + msg);
                CHECK(cap.hits.load() == 1, "5268: timed-out POST is not retried via CLI");
            });

            with_http(json, 2000, [&](int port, HttpCapture& cap) {
                auto& heap = ev.string_heap_mut();
                const auto url =
                    heap.mint(std::string("http://127.0.0.1:") + std::to_string(port) + "/ok");
                const auto body = heap.mint(std::string("{}"));
                const auto auth = heap.mint(std::string("k"));
                const auto slow = (*http_post)(
                    {make_string(url), make_string(body), make_string(auth), make_int(5)});
                CHECK(is_string(slow) && heap_string(ev, slow) == json,
                      "5268: 5s timeout accepts a 2s response");
                CHECK(cap.hits.load() == 1, "5268: widened timeout is a single POST");
            });

            http_to.set("1");
            with_http(json, 2000, [&](int port, HttpCapture& cap) {
                auto& heap = ev.string_heap_mut();
                const auto url =
                    heap.mint(std::string("http://127.0.0.1:") + std::to_string(port) + "/env");
                const auto body = heap.mint(std::string("{}"));
                const auto denied = (*http_post)({make_string(url), make_string(body)});
                CHECK(is_error(denied), "5268: AURA_HTTP_TIMEOUT=1 times out");
                CHECK(ev.soft_error_message(denied).find("timeout") != std::string::npos,
                      "5268: env timeout text");
                CHECK(cap.hits.load() == 1, "5268: env timeout is a single POST");
            });
            with_http(json, 2000, [&](int port, HttpCapture&) {
                auto& heap = ev.string_heap_mut();
                const auto url =
                    heap.mint(std::string("http://127.0.0.1:") + std::to_string(port) + "/over");
                const auto body = heap.mint(std::string("{}"));
                const auto overridden =
                    (*http_post)({make_string(url), make_string(body), make_int(5)});
                CHECK(is_string(overridden) && heap_string(ev, overridden) == json,
                      "5268: explicit timeout overrides AURA_HTTP_TIMEOUT");
            });
            http_to.clear();

            int closed_port = 0;
            const int closed = listen_loopback(closed_port);
            CHECK(closed >= 0, "5267: refused-port listener");
            if (closed >= 0)
                ::close(closed);
            if (closed_port > 0) {
                auto& heap = ev.string_heap_mut();
                const auto url = heap.mint(std::string("http://127.0.0.1:") +
                                           std::to_string(closed_port) + "/nope");
                const auto body = heap.mint(std::string("{}"));
                const auto refused = (*http_post)({make_string(url), make_string(body)});
                CHECK(is_error(refused) && !is_void(refused), "5267: connect failure is an error");
                CHECK(ev.soft_error_message(refused).find("http-post:") == 0,
                      "5267: connect failure names http-post");
                const auto get_refused = (*http_get)({make_string(url)});
                CHECK(is_error(get_refused), "5267: http-get connect failure is an error");
            }

            {
                auto& heap = ev.string_heap_mut();
                const auto url = heap.mint(std::string("http://127.0.0.1:9/bad-timeout"));
                const auto body = heap.mint(std::string("{}"));
                const auto bad = (*http_post)({make_string(url), make_string(body), make_int(0)});
                CHECK(is_error(bad), "5268: timeout 0 is an error");
                CHECK(ev.soft_error_message(bad).find("bad timeout") != std::string::npos,
                      "5268: bad timeout text");
            }
        }
    }

    {
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        auto loaded = cs.eval("(require \"std/llm\" all:)");
        CHECK(static_cast<bool>(loaded), "5268: require std/llm");
        const auto truncated = eval_string(
            cs,
            "(hash-ref (llm:parse-response "
            "\"{\\\"choices\\\":[{\\\"finish_reason\\\":\\\"length\\\",\\\"message\\\":{"
            "\\\"content\\\":\\\"\\\",\\\"reasoning_content\\\":\\\"thinking\\\"}}]}\") "
            "\"error\")",
            "5268: truncated parse");
        CHECK(truncated == "truncated/finish=length", "5268: finish=length: " + truncated);
        const auto content = eval_string(
            cs,
            "(hash-ref (llm:parse-response "
            "\"{\\\"choices\\\":[{\\\"message\\\":{\\\"content\\\":\\\"(seq ok)\\\"}}]}\") "
            "\"content\")",
            "5267: parse content");
        CHECK(content == "(seq ok)", "5267: parse extracts content");
        const auto missing = eval_string(cs,
                                         "(hash-ref (llm:parse-response "
                                         "\"{\\\"choices\\\":[{\\\"message\\\":{}}]}\") \"error\")",
                                         "5268: no-content");
        CHECK(missing == "no-content", "5268: missing content stays no-content");
        const auto compat = eval_string(cs, "(hash-ref (llm:chat \"sys\" \"user\") \"error\")",
                                        "5268: two-arg chat");
        CHECK(compat == "empty", "5268: two-arg llm:chat without a key is empty");
    }

    llm_key.set("TESTKEY_NOT_REAL");
    llm_model.set("unit-model");
    with_http(json, 0, [&](int port, HttpCapture& cap) {
        llm_base.set((std::string("http://127.0.0.1:") + std::to_string(port) + "/v1").c_str());
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        CHECK(static_cast<bool>(cs.eval("(require \"std/llm\" all:)")), "5268: llm for chat");
        const auto content =
            eval_string(cs,
                        "(hash-ref (llm:chat \"sys\" \"user\" (hash \"max_tokens\" 32 "
                        "\"reasoning_effort\" \"minimal\")) \"content\")",
                        "5268: llm:chat extra fields");
        CHECK(content == "(seq ok)", "5268: llm:chat returns content");
        CHECK(cap.request.find("\"max_tokens\":32") != std::string::npos,
              "5268: max_tokens merged");
        CHECK(cap.request.find("\"reasoning_effort\":\"minimal\"") != std::string::npos,
              "5268: reasoning_effort merged");
        CHECK(cap.request.find("\"model\":\"unit-model\"") != std::string::npos, "5268: model");
        CHECK(cap.request.find("TESTKEY_NOT_REAL") != std::string::npos, "5268: chat bearer");
    });
    with_http(json, 0, [&](int port, HttpCapture& cap) {
        llm_base.set((std::string("http://127.0.0.1:") + std::to_string(port) + "/v1").c_str());
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        CHECK(static_cast<bool>(cs.eval("(require \"std/llm\" all:)")), "5268: llm alist");
        const auto content =
            eval_string(cs,
                        "(hash-ref (llm:chat \"sys\" \"user\" (list (cons \"max_tokens\" 7) "
                        "(cons \"reasoning_effort\" \"low\")) 5) \"content\")",
                        "5268: llm:chat alist");
        CHECK(content == "(seq ok)", "5268: alist chat returns content");
        CHECK(cap.request.find("\"max_tokens\":7") != std::string::npos, "5268: alist max_tokens");
        CHECK(cap.request.find("\"reasoning_effort\":\"low\"") != std::string::npos,
              "5268: alist reasoning_effort");
    });
    llm_to.set("1");
    with_http(json, 2000, [&](int port, HttpCapture& cap) {
        llm_base.set((std::string("http://127.0.0.1:") + std::to_string(port) + "/v1").c_str());
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        CHECK(static_cast<bool>(cs.eval("(require \"std/llm\" all:)")),
              "5268: llm timeout require");
        const auto err = eval_string(cs, "(hash-ref (llm:chat \"sys\" \"user\") \"error\")",
                                     "5268: LLM_TIMEOUT");
        CHECK(err.find("timeout") != std::string::npos, "5268: LLM_TIMEOUT error: " + err);
        CHECK(cap.hits.load() == 1, "5268: LLM_TIMEOUT does not fall back to a second POST");
    });
    llm_to.clear();
}

// libcurl dlopen is blocked. http-get has no CLI fallback and must say so.
// http-post may still use the jailed curl CLI (#5214) and then the body
// comes back; if that binary is missing the error has to name the failure.
void check_libcurl_unavailable() {
    std::println("\n--- #5267: libcurl unavailable ---");
    const std::string bin = find_aura_bin();
    CHECK(!bin.empty(), "5267: aura binary for the libcurl-unavailable case");
    if (bin.empty())
        return;
    const std::string dir = "/tmp/aura5267-" + std::to_string(::getpid());
    if (::mkdir(dir.c_str(), 0700) != 0) {
        CHECK(false, "5267: mkdir interposer dir");
        return;
    }
    const std::string src = dir + "/nocurl.c";
    const std::string so = dir + "/nocurl.so";
    {
        const int fd = ::open(src.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
        const char text[] =
            "#define _GNU_SOURCE\n"
            "#include <dlfcn.h>\n"
            "#include <string.h>\n"
            "void *dlopen(const char *filename, int flags) {\n"
            "  static void *(*real_dlopen)(const char *, int) = 0;\n"
            "  if (!real_dlopen)\n"
            "    real_dlopen = (void *(*)(const char *, int))dlsym(RTLD_NEXT, \"dlopen\");\n"
            "  if (filename && strstr(filename, \"libcurl\")) return 0;\n"
            "  return real_dlopen(filename, flags);\n"
            "}\n";
        CHECK(fd >= 0, "5267: write interposer source");
        if (fd >= 0) {
            (void)::write(fd, text, sizeof(text) - 1);
            ::close(fd);
        }
    }
    const pid_t cc = ::fork();
    if (cc == 0) {
        ::execlp("g++", "g++", "-shared", "-fPIC", "-o", so.c_str(), src.c_str(), "-ldl", nullptr);
        ::_exit(127);
    }
    int cc_status = 1;
    ::waitpid(cc, &cc_status, 0);
    if (cc < 0 || !WIFEXITED(cc_status) || WEXITSTATUS(cc_status) != 0) {
        CHECK(false, "5267: compile dlopen interposer");
        return;
    }
    with_http("FROM_CLI", 0, [&](int port, HttpCapture& cap) {
        std::array<int, 2> out_pipe{-1, -1};
        if (::pipe2(out_pipe.data(), O_CLOEXEC) != 0) {
            CHECK(false, "5267: pipe for aura");
            return;
        }
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::dup2(out_pipe[1], STDOUT_FILENO);
            ::dup2(out_pipe[1], STDERR_FILENO);
            ::setenv("LD_PRELOAD", so.c_str(), 1);
            ::setenv("AURA_SANDBOX", "off", 1);
            ::setenv("AURA_RUNTIME_DIR", (std::string(AURA_SOURCE_DIR) + "/lib").c_str(), 1);
            const std::string expr =
                "(begin (require \"std/net\") (display (try (http-get \"http://127.0.0.1:" +
                std::to_string(port) +
                "/g\") (catch (e) e))) (newline) (display (try (http-post "
                "\"http://127.0.0.1:" +
                std::to_string(port) + "/p\" \"{\\\"a\\\":1}\") (catch (e) e))) (newline))";
            ::execl(bin.c_str(), bin.c_str(), "-e", expr.c_str(), nullptr);
            ::_exit(127);
        }
        ::close(out_pipe[1]);
        std::string out;
        std::array<char, 1024> buf{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd ready{};
            ready.fd = out_pipe[0];
            ready.events = POLLIN;
            if (::poll(&ready, 1, 100) > 0) {
                const ssize_t n = ::read(out_pipe[0], buf.data(), buf.size());
                if (n > 0)
                    out.append(buf.data(), static_cast<std::size_t>(n));
                else if (n == 0)
                    break;
            }
            int st = 0;
            if (::waitpid(pid, &st, WNOHANG) == pid && (ready.revents & POLLIN) == 0) {
                const ssize_t n = ::read(out_pipe[0], buf.data(), buf.size());
                if (n > 0)
                    out.append(buf.data(), static_cast<std::size_t>(n));
                break;
            }
        }
        ::kill(pid, SIGKILL);
        int st = 0;
        ::waitpid(pid, &st, 0);
        ::close(out_pipe[0]);
        if (out.find("libcurl unavailable") == std::string::npos)
            std::println(stderr, "5267 libcurl-unavailable output:\n{}", out);
        CHECK(out.find("libcurl unavailable") != std::string::npos,
              "5267: http-get reports libcurl unavailable");
        const bool cli_body = out.find("FROM_CLI") != std::string::npos;
        const bool cli_err = out.find("http-post:") != std::string::npos;
        CHECK(cli_body || cli_err, "5267: http-post without libcurl is a body or a named error");
        if (cli_body) {
            CHECK(cap.request.find("aura/1.0") == std::string::npos,
                  "5267: CLI fallback is not the in-process user agent");
            CHECK(cap.hits.load() == 1, "5267: CLI fallback posts once");
        }
    });
    ::unlink(so.c_str());
    ::unlink(src.c_str());
    ::rmdir(dir.c_str());
}

} // namespace

int run_test_production_safety_p2() {
    CompilerService cs;

    {
        auto r = cs.eval("(engine:metrics \"query:production-safety-1097-1122-stats\")");
        CHECK(r && is_hash(*r), "safety stats is hash");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "schema") == 1097, "schema 1097");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "eval-async-heap-result") == 1,
              "eval async heap");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "const-fold-bool-tag-fixed") == 1,
              "const fold bool tag");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "const-fold-block-clear") == 1,
              "const fold block clear");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "reflect-bounds-checks") == 1,
              "reflect bounds");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "cache-header-validate-ext") == 1,
              "cache header validate");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "open-cache-ir-bounds") == 1,
              "open cache ir bounds");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "schema-unknown-is-object") == 1,
              "schema unknown is object");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "issue-1122") == 1122,
              "issue-1122 field");
    }

    // #1104: wild header fails extended validation
    {
        CacheHeader bad{};
        std::memcpy(bad.magic, "AURACACH", 8);
        bad.version = 5;
        bad.num_nodes = 1;
        bad.node_offset = 64;
        bad.num_strings = 0xFFFFFFFFu;
        CHECK(cache_validate_header(&bad) != 0, "wild num_strings rejected");

        CacheHeader okh{};
        std::memcpy(okh.magic, "AURACACH", 8);
        okh.version = 5;
        okh.num_nodes = 10;
        okh.node_offset = 64;
        okh.num_strings = 5;
        okh.num_functions = 1;
        okh.string_offset = 128;
        CHECK(cache_validate_header(&okh) == 0, "sane header accepted");
        // #5213: a zero file size keeps the absolute-cap path. A real size
        // rejects a node table that cannot fit in the file.
        CHECK(cache_validate_header(&okh, 0) == 0, "size 0 skips spans");
        CHECK(cache_validate_header(&okh, 80) != 0, "node span past small file");
        CacheHeader past = okh;
        past.node_offset = 100000;
        past.num_nodes = 1;
        past.num_strings = 0;
        past.num_functions = 0;
        past.string_offset = 0;
        CHECK(cache_validate_header(&past, 200) != 0, "node_offset past end");
    }

    // Arithmetic / fold regression (bool-tag fix must not break +)
    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto e = cs.eval("(eval \"(+ 1 2)\")");
        CHECK(e && is_int(*e) && as_int(*e) == 3, "eval works");
        // Fold path: 1+2=3 must remain integer 3 (not confusable with old #f tag)
        auto t = cs.eval("(+ 1 2)");
        CHECK(t && is_int(*t) && as_int(*t) == 3, "1+2=3 pure int");
    }

    check_5214_source();
    check_5214_http_post_once();
    check_5214_emit_metacharacters();
    check_http_body_and_timeout();
    check_libcurl_unavailable();

    if (::aura::test::g_failed)
        return 1;
    std::println("production safety #1097–#1122: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_production_safety_p2();
}
#endif
