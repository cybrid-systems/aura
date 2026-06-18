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
using aura::test::g_passed;
using aura::test::g_failed;
#define PRINTLN(msg) do { std::print("{}\n", std::string(msg)); } while(0)

namespace fs = std::filesystem;

namespace {

// Locate the aura binary relative to this test binary. The CMake
// target puts both in the same build directory.
std::string find_aura_binary() {
    // Try ./aura first (most common layout)
    if (fs::exists("./aura") && access("./aura", X_OK) == 0) {
        return fs::absolute("./aura").string();
    }
    // Try ../aura (in case test runs from a subdirectory)
    if (fs::exists("../aura") && access("../aura", X_OK) == 0) {
        return fs::absolute("../aura").string();
    }
    // Try via /proc/self/exe — go up to the build/ dir
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        fs::path p(buf);
        fs::path candidate = p.parent_path() / "aura";
        if (fs::exists(candidate)) return candidate.string();
    }
    return "aura";  // fallback: PATH lookup
}

// Run aura --emit-binary on stdin source, return true on success.
bool run_emit_binary(const std::string& aura, const std::string& src,
                     const std::string& out_path) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }
    if (pid == 0) {
        // child: wire stdin to pipe, exec aura --emit-binary out_path
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]); close(pipefd[1]);
        execl(aura.c_str(), aura.c_str(), "--emit-binary", out_path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    // parent: write source to pipe, close, wait
    close(pipefd[0]);
    ssize_t w = write(pipefd[1], src.data(), src.size());
    (void)w;
    close(pipefd[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Run an executable, capture stdout. Returns true on exit-0.
std::string run_capture_stdout(const std::string& exe) {
    std::array<char, 4096> buf;
    std::string out;
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {}; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]); close(pipefd[1]);
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
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    return out;
}

bool is_elf(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4] = {0};
    f.read(magic, 4);
    return magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

}  // namespace

bool test_emit_binary_simple_add() {
    PRINTLN("\n--- Test 1: aura --emit-binary on (+ 1 2) produces ELF that outputs 3 ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/test_issue_237_add_bin";
    fs::remove(out_path);
    bool ok = run_emit_binary(aura, "(+ 1 2)", out_path);
    CHECK(ok, "aura --emit-binary exited 0");
    CHECK(fs::exists(out_path), "output file exists");
    if (!fs::exists(out_path)) return false;
    CHECK(is_elf(out_path), "output is ELF (magic 0x7f 'E' 'L' 'F')");
    std::string output = run_capture_stdout(out_path);
    CHECK(!output.empty(), "exec captured output");
    CHECK(output.find("3") != std::string::npos, "output contains '3'");
    fs::remove(out_path);
    return true;
}

bool test_emit_binary_lambda() {
    PRINTLN("\n--- Test 2: aura --emit-binary with (define f (lambda ...)) (f 7) outputs 49 ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/test_issue_237_lambda_bin";
    fs::remove(out_path);
    std::string src = "(define f (lambda (x) (* x x))) (f 7)";
    bool ok = run_emit_binary(aura, src, out_path);
    CHECK(ok, "aura --emit-binary exited 0");
    CHECK(fs::exists(out_path), "output file exists");
    if (!fs::exists(out_path)) return false;
    std::string output = run_capture_stdout(out_path);
    CHECK(!output.empty(), "exec captured output");
    CHECK(output.find("49") != std::string::npos, "output contains '49'");
    fs::remove(out_path);
    return true;
}

bool test_emit_binary_handles_errors() {
    PRINTLN("\n--- Test 3: aura --emit-binary on empty stdin returns non-zero ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/test_issue_237_empty_bin";
    fs::remove(out_path);
    bool ok = run_emit_binary(aura, "", out_path);
    CHECK(!ok, "empty input causes non-zero exit");
    return true;
}

int main() {
    std::fprintf(stdout, "═══ Issue #237 — AOT end-to-end test ═══\n");
    std::fprintf(stdout, "  Verifies aura --emit-binary produces working ELF executables.\n\n");

    test_emit_binary_simple_add();
    test_emit_binary_lambda();
    test_emit_binary_handles_errors();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}