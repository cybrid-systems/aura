// @category: integration
// @reason: spawns aura binary as subprocess; integration test for AOT
// test_issue_237.cpp — Issue #237: AOT compilation path end-to-end.
//
// Verifies the existing --emit-binary mode (in src/main.cpp:1748)
// produces a real ELF executable that runs correctly.
//
// MVP scope (this test):
//   1. aura --emit-binary on a simple (+ 1 2) expression produces
//      a binary that outputs "3" when executed
//   2. The output file is an ELF (magic 0x7f 'E' 'L' 'F')
//   3. aura --emit-binary with a more complex expression
//      (define f (lambda (x) (* x x))) (f 7) outputs "49"
//
// Out of scope (deferred follow-ups):
//   - Performance benchmark vs JIT -O2 (fib-20)
//   - AOT for closures / recursion
//   - Cross-host AOT cache
//   - Shared library (.so) linking + dlopen load-back

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_harness.hpp"
using aura::test::g_failed;
using aura::test::g_passed;
#define PRINTLN(msg)                                                                               \
    do {                                                                                           \
        std::print("{}\n", std::string(msg));                                                      \
    } while (0)

namespace fs = std::filesystem;

namespace {

// Locate the aura binary relative to this test binary. The CMake
// target puts both in the same build directory.
//
// IMPORTANT: must check is_regular_file(), not just access(X_OK).
// access(X_OK) returns 0 for directories (X_OK = "search
// permission", which directories always have for the owner).
// Without the is_regular_file check, a CWD-relative path like
// `../aura` would resolve to the project root itself (since
// `..` from inside /home/dev/code/aura is /home/dev/code/ and
// `aura` from there is the same directory), pass the existence
// check, and then execl() would fail with ENOEXEC (exit=127).
std::string find_aura_binary() {
    // Try ./aura first (most common layout)
    if (fs::is_regular_file("./aura") && access("./aura", X_OK) == 0) {
        return fs::absolute("./aura").string();
    }
    // Try ../aura (in case test runs from a subdirectory)
    if (fs::is_regular_file("../aura") && access("../aura", X_OK) == 0) {
        return fs::absolute("../aura").string();
    }
    // Try via /proc/self/exe — go up to the build/ dir
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        fs::path p(buf);
        fs::path candidate = p.parent_path() / "aura";
        if (fs::is_regular_file(candidate))
            return candidate.string();
    }
    return "aura"; // fallback: PATH lookup
}

// Run aura --emit-binary on stdin source, return true on success.
// Captures stderr + the wait status so the test can surface
// aura's actual failure mode (crash vs error message vs clean
// exit) when the binary fails on a CI architecture we can't
// reproduce locally (e.g. x86_64 segfaults).
struct EmitResult {
    bool ok;            // true iff aura exited 0
    bool crashed;       // true iff aura was killed by signal (SIGSEGV, etc.)
    int exit_code;      // raw exit status
    int signal;         // signal number if killed, else 0
    std::string stderr; // captured stderr
};
EmitResult run_emit_binary(const std::string& aura, const std::string& src,
                           const std::string& out_path) {
    EmitResult res{};
    int in_pipe[2], err_pipe[2];
    if (pipe(in_pipe) != 0) {
        res.exit_code = -1;
        return res;
    }
    if (pipe(err_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        res.exit_code = -1;
        return res;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        res.exit_code = -1;
        return res;
    }
    if (pid == 0) {
        // child: wire stdin to in_pipe, stderr to err_pipe, exec aura
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(in_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execl(aura.c_str(), aura.c_str(), "--emit-binary", out_path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    // parent: write source to in_pipe, close, then drain err_pipe, wait
    close(in_pipe[0]);
    ssize_t w = write(in_pipe[1], src.data(), src.size());
    (void)w;
    close(in_pipe[1]);
    close(err_pipe[1]);
    // Drain stderr into res.stderr
    char buf[4096];
    ssize_t n;
    while ((n = read(err_pipe[0], buf, sizeof(buf))) > 0) {
        res.stderr.append(buf, n);
    }
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        res.exit_code = WEXITSTATUS(status);
        res.ok = (res.exit_code == 0);
    } else if (WIFSIGNALED(status)) {
        res.signal = WTERMSIG(status);
        res.crashed = true;
        res.ok = false;
        // Append the signal name to stderr for diagnostics
        res.stderr += "\n[signal] aura killed by SIG";
        switch (res.signal) {
            case SIGSEGV:
                res.stderr += "SEGV";
                break;
            case SIGBUS:
                res.stderr += "BUS";
                break;
            case SIGABRT:
                res.stderr += "ABRT";
                break;
            case SIGILL:
                res.stderr += "ILL";
                break;
            default:
                res.stderr += std::to_string(res.signal);
                break;
        }
        res.stderr += " (likely a crash in aura's IR pipeline / LLVM / linker)";
    }
    return res;
}

// Run an executable, capture stdout. Returns true on exit-0.
std::string run_capture_stdout(const std::string& exe) {
    std::array<char, 4096> buf;
    std::string out;
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return {};
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {};
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    ssize_t n;
    while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
        out.append(buf.data(), n);
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {};
    return out;
}

bool is_elf(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    char magic[4] = {0};
    f.read(magic, 4);
    return magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

} // namespace

bool test_emit_binary_simple_add() {
    PRINTLN("\n--- Test 1: aura --emit-binary on (+ 1 2) produces ELF that outputs 3 ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/test_issue_237_add_bin";
    fs::remove(out_path);
    EmitResult res = run_emit_binary(aura, "(+ 1 2)", out_path);
    // Issue #360 task 5: always surface aura's stderr + exit
    // status so CI logs can pinpoint the failure mode (clean
    // error vs crash vs non-zero exit). The pre-#360 version
    // only printed stderr when res.ok was false; downstream
    // CHECKs (is_elf, output.contains("3")) would silently
    // fail without surfacing the AOT stderr that explains why.
    if (!res.ok || g_failed > 0) {
        std::println("       [aura failure] exit={} signal={} crashed={}", res.exit_code,
                     res.signal, res.crashed);
        if (!res.stderr.empty()) {
            std::println("       [aura stderr] {}", res.stderr);
        }
    }
    CHECK(res.ok, "aura --emit-binary exited 0");
    CHECK(fs::exists(out_path), "output file exists");
    if (!fs::exists(out_path)) {
        // Belt-and-suspenders: print stderr even on the early
        // return path so CI logs always have it.
        if (!res.stderr.empty())
            std::println("       [aura stderr] {}", res.stderr);
        return false;
    }
    CHECK(is_elf(out_path), "output is ELF (magic 0x7f 'E' 'L' 'F')");
    std::string output = run_capture_stdout(out_path);
    CHECK(!output.empty(), "exec captured output");
    CHECK(output.find("3") != std::string::npos, "output contains '3'");
    if (g_failed > 0 && !res.stderr.empty()) {
        // Surface stderr one more time at the end so it's never
        // lost when multiple CHECKs fail in sequence.
        std::println("       [aura stderr tail] {}", res.stderr);
    }
    fs::remove(out_path);
    return true;
}

bool test_emit_binary_lambda() {
    PRINTLN("\n--- Test 2: aura --emit-binary with (define f (lambda ...)) (f 7) outputs 49 ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/test_issue_237_lambda_bin";
    fs::remove(out_path);
    std::string src = "(define f (lambda (x) (* x x))) (f 7)";
    EmitResult res = run_emit_binary(aura, src, out_path);
    // Issue #360 task 5: always surface aura's stderr on failure
    // (was: only when res.ok was false; downstream is_elf /
    // output.contains("49") checks could silently fail).
    if (!res.ok || g_failed > 0) {
        std::println("       [aura failure] exit={} signal={} crashed={}", res.exit_code,
                     res.signal, res.crashed);
        if (!res.stderr.empty()) {
            std::println("       [aura stderr] {}", res.stderr);
        }
    }
    CHECK(res.ok, "aura --emit-binary exited 0");
    CHECK(fs::exists(out_path), "output file exists");
    if (!fs::exists(out_path)) {
        std::println("       [test_237 early-return] aura returned {} but {} does not exist",
                     res.ok ? "ok" : "non-ok", out_path);
        if (!res.stderr.empty())
            std::println("       [aura stderr] {}", res.stderr);
        return false;
    }
    CHECK(is_elf(out_path), "output is ELF (magic 0x7f 'E' 'L' 'F')");
    std::string output = run_capture_stdout(out_path);
    CHECK(!output.empty(), "exec captured output");
    CHECK(output.find("49") != std::string::npos, "output contains '49'");
    if (g_failed > 0 && !res.stderr.empty()) {
        std::println("       [aura stderr tail] {}", res.stderr);
    }
    fs::remove(out_path);
    return true;
}

bool test_emit_binary_handles_errors() {
    PRINTLN("\n--- Test 3: aura --emit-binary on empty stdin returns non-zero ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/test_issue_237_empty_bin";
    fs::remove(out_path);
    EmitResult res = run_emit_binary(aura, "", out_path);
    CHECK(!res.ok, "empty input causes non-zero exit");
    return true;
}

int aura_issue_237_run() {
    std::fprintf(stdout, "═══ Issue #237 — AOT end-to-end test ═══\n");
    std::fprintf(stdout, "  Verifies aura --emit-binary produces working ELF executables.\n\n");

    test_emit_binary_simple_add();
    test_emit_binary_lambda();
    test_emit_binary_handles_errors();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_237_run();
}
#endif
