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
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
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
            CHECK(!is_string(value), "5214: empty body stays void");
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
