// @category: integration
// @reason: Issue #677 deployment health endpoints + install layout

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <print>
#include <string>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace fs = std::filesystem;

namespace aura_issue_677_detail {
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++g_passed;                                                                            \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++g_failed;                                                                            \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

static std::string find_aura_binary() {
    // Check common locations in priority order.
    for (const char* path : {
             "./aura",           // cwd = build/
             "./build/aura",     // cwd = repo root, build is direct child
             "../build/aura",    // cwd = tests/
             "../aura",          // cwd = build/, parent has aura
             "../../build/aura", // deeper nesting
         }) {
        if (fs::is_regular_file(path))
            return fs::absolute(path).string();
    }
    return "aura";
}

static std::string http_get(const std::string& host, int port, const std::string& path) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return {};
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    const std::string req =
        "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    (void)::send(fd, req.data(), req.size(), 0);
    std::string resp;
    char buf[4096];
    ssize_t n;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    return resp;
}

} // namespace aura_issue_677_detail

int aura_issue_677_run() {
    using namespace aura_issue_677_detail;
    std::println("=== Issue #677: deployment health + stats ===");

    const auto aura = find_aura_binary();
    const int port = 18087;

    fs::path repo_root = fs::current_path();
    for (int i = 0; i < 4 && !fs::is_regular_file(repo_root / "lib/runtime.c"); ++i)
        repo_root = repo_root.parent_path();

    pid_t pid = ::fork();
    if (pid == 0) {
        ::setenv("AURA_RUNTIME_DIR", repo_root.string().c_str(), 1);
        execl(aura.c_str(), aura.c_str(), "--health-server", std::to_string(port).c_str(), nullptr);
        _exit(127);
    }
    if (pid < 0) {
        CHECK(false, "fork health server");
        return 1;
    }

    bool ok = false;
    for (int i = 0; i < 30; ++i) {
        const auto h = http_get("127.0.0.1", port, "/healthz");
        if (h.find("200") != std::string::npos) {
            ok = true;
            break;
        }
        ::usleep(100000);
    }
    CHECK(ok, "/healthz returns 200");

    const auto ready = http_get("127.0.0.1", port, "/readyz");
    CHECK(ready.find("200") != std::string::npos, "/readyz returns 200");

    const auto metrics = http_get("127.0.0.1", port, "/metrics");
    CHECK(metrics.find("aura_up 1") != std::string::npos, "/metrics exposes aura_up");

    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);

    aura::compiler::CompilerService cs;
    auto r = cs.eval("(stats:count)");
    const auto n = r && aura::compiler::types::is_int(*r) ? aura::compiler::types::as_int(*r) : 0;
    CHECK(n >= 55, std::format("stats:count >= 55 (got {})", n));

    auto dep = cs.eval("(engine:metrics \"query:deployment-stats\")");
    CHECK(aura::compiler::types::is_hash(*dep), "query:deployment-stats returns hash");

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_677_run();
}
#endif
