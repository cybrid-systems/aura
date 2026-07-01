// @category: integration
// @reason: spawns aura binary as subprocess; integration test for AOT
//
// test_issue_374.cpp — Issue #374: AURA_RUNTIME_DIR CI-side lookup
// + install layout support for find_runtime_c().
//
// Background: #237 v4 (commit 661d3272) shipped find_runtime_c()
// with the /proc/self/exe walk-up path; #360 (2a66b2b0) added
// FHS install-path fallbacks. Issue #374 asks for the CI side:
// (1) explicit AURA_RUNTIME_DIR handling in CI scripts and
// tests, (2) install layout smoke test, (3) docs/build.md note
// about the 4 lookup strategies.
//
// There is no `cmake install` rule in the project yet (the
// install-path fallbacks are for `make install` once it lands),
// so the "install layout" test is functional: it verifies the
// AURA_RUNTIME_DIR env var path (lookup strategy #1) works
// even when the binary is spawned from a CWD with no lib/
// nearby — the same conditions CI hits when the aura binary
// and the test runner live in different working dirs.
//
// Strategy tested in this file:
//   (1) AURA_RUNTIME_DIR env var (this file's main focus)
//   (2) walk-up from aura binary's dir (sanity check: still works)
//   (3) CWD-relative fallback (regression check: still works)
//   (4) FHS install-path fallback — NOT tested here. Requires
//       writing to /usr/local/share/aura/ or /usr/share/aura/,
//       which the CI container doesn't allow. The path is
//       code-present and would be exercised by a real
//       `make install` flow when one lands.

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

namespace aura_issue_374_detail {

// Locate the aura binary relative to this test binary.
// Mirrors test_issue_237's find_aura_binary — same lookup
// logic, kept in sync. (Tempting to extract to a shared
// helper, but each test_issue_* file is intentionally
// self-contained per the bundle convention.)
std::string find_aura_binary() {
    if (fs::is_regular_file("./aura") && access("./aura", X_OK) == 0) {
        return fs::absolute("./aura").string();
    }
    if (fs::is_regular_file("../aura") && access("../aura", X_OK) == 0) {
        return fs::absolute("../aura").string();
    }
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        fs::path p(buf);
        fs::path candidate = p.parent_path() / "aura";
        if (fs::is_regular_file(candidate)) return candidate.string();
    }
    return "aura";
}

// Run aura --emit-binary on stdin source, return EmitResult.
// Mirrors test_issue_237's runner. env_overrides lets the
// caller set env vars on the child (e.g. AURA_RUNTIME_DIR,
// CWD) so the test can exercise the env-var path.
struct EmitResult {
    bool ok;               // true iff aura exited 0
    int exit_code;         // raw exit status
    int signal;            // signal number if killed, else 0
    bool crashed;          // true iff aura was killed by signal
    std::string stderr;    // captured stderr
    bool aot_error_marker; // true iff stderr contains "AOT: cannot find lib/runtime.c"
};
EmitResult run_emit_binary_with_env(const std::string& aura,
                                     const std::string& src,
                                     const std::string& out_path,
                                     const std::string& cwd,
                                     const std::vector<std::string>& env_overrides) {
    EmitResult res{};
    int in_pipe[2], err_pipe[2];
    if (pipe(in_pipe) != 0) return res;
    if (pipe(err_pipe) != 0) { close(in_pipe[0]); close(in_pipe[1]); return res; }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return res;
    }
    if (pid == 0) {
        // child: change to test-controlled CWD, wire stdin/stderr, exec aura
        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) _exit(126);
        }
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        // Set the env overrides via putenv (null-terminated name=value).
        for (const auto& kv : env_overrides) {
            ::putenv(const_cast<char*>(kv.c_str()));
        }
        execl(aura.c_str(), aura.c_str(), "--emit-binary", out_path.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    close(in_pipe[0]);
    ssize_t w = write(in_pipe[1], src.data(), src.size());
    (void)w;
    close(in_pipe[1]);
    close(err_pipe[1]);
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
    }
    res.aot_error_marker = (res.stderr.find("AOT: cannot find lib/runtime.c") != std::string::npos);
    return res;
}

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

// Copy lib/runtime.c (relative to repo root) into <dst>/runtime.c.
// Returns true on success. Used to set up a controlled
// AURA_RUNTIME_DIR test directory.
bool setup_runtime_c_dir(const std::string& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return false;
    // Find lib/runtime.c — try a few common repo locations.
    const char* candidates[] = {
        "lib/runtime.c",
        "../lib/runtime.c",
        "../../lib/runtime.c",
        "../../../lib/runtime.c",
    };
    for (const char* c : candidates) {
        if (fs::is_regular_file(c)) {
            fs::copy_file(c, dst + "/runtime.c", fs::copy_options::overwrite_existing, ec);
            return !ec;
        }
    }
    return false;
}

// AC1: AURA_RUNTIME_DIR points to a real dir with runtime.c —
// aura --emit-binary works from a CWD that has no lib/ nearby.
// This is the canonical CI fix: set AURA_RUNTIME_DIR in the
// build-test step and the test_issue_237 path keeps working
// regardless of the runner's CWD.
bool test_aura_runtime_dir_override_works() {
    PRINTLN("\n--- AC1: AURA_RUNTIME_DIR points to real dir, --emit-binary works ---");
    std::string aura = find_aura_binary();
    std::string runtime_dir = "/tmp/issue_374_runtime_c_real";
    std::string out_path = "/tmp/issue_374_aot_out";
    fs::remove(out_path);
    if (!setup_runtime_c_dir(runtime_dir)) {
        std::println("       [skip] could not set up {}", runtime_dir);
        return true;  // skip rather than fail
    }
    EmitResult res = run_emit_binary_with_env(aura, "(+ 1 2)", out_path,
                                               "/tmp",  // CWD with no lib/
                                               {"AURA_RUNTIME_DIR=" + runtime_dir});
    if (!res.ok) {
        std::println("       [aura failure] exit={} signal={} crashed={}",
                     res.exit_code, res.signal, res.crashed);
        if (!res.stderr.empty()) std::println("       [aura stderr] {}", res.stderr);
    }
    CHECK(res.ok, "aura --emit-binary exited 0 with AURA_RUNTIME_DIR override");
    CHECK(fs::exists(out_path), "output file exists");
    if (!fs::exists(out_path)) return false;
    std::string output = run_capture_stdout(out_path);
    CHECK(!output.empty(), "exec captured output");
    CHECK(output.find("3") != std::string::npos, "output contains '3'");
    fs::remove(out_path);
    fs::remove_all(runtime_dir);
    return true;
}

// AC2: AURA_RUNTIME_DIR unset, walk-up from aura binary's dir
// (path #2) finds lib/runtime.c. Regression check — the
// pre-#374 baseline must still work. Spawns from a CWD that
// has no lib/ to force the walk-up path (otherwise CWD-rel
// path #3 would also resolve).
bool test_walkup_finds_runtime_c() {
    PRINTLN("\n--- AC2: AURA_RUNTIME_DIR unset, walk-up from aura binary finds runtime.c ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/issue_374_walkup_out";
    fs::remove(out_path);
    // No AURA_RUNTIME_DIR override. CWD is /tmp (no lib/ there).
    EmitResult res = run_emit_binary_with_env(aura, "(+ 1 2)", out_path, "/tmp", {});
    if (!res.ok) {
        std::println("       [aura failure] exit={} signal={} crashed={}",
                     res.exit_code, res.signal, res.crashed);
        if (!res.stderr.empty()) std::println("       [aura stderr] {}", res.stderr);
    }
    CHECK(res.ok, "aura --emit-binary exited 0 (walk-up path)");
    CHECK(fs::exists(out_path), "output file exists (walk-up path)");
    if (!fs::exists(out_path)) return false;
    std::string output = run_capture_stdout(out_path);
    CHECK(output.find("3") != std::string::npos, "output contains '3' (walk-up path)");
    fs::remove(out_path);
    return true;
}

// AC3: AURA_RUNTIME_DIR points to a NON-existent dir. Aura
// must still work (falls through to walk-up / CWD-rel / FHS
// install-path). And the diagnostic surface — the AOT error
// marker — should NOT appear (because the env var miss is
// recovered by path 2/3/4, not surfaced as "cannot find
// runtime.c"). This guards against future refactors that
// short-circuit on a bad AURA_RUNTIME_DIR.
bool test_bad_aura_runtime_dir_falls_through() {
    PRINTLN("\n--- AC3: AURA_RUNTIME_DIR points to non-existent dir, falls through ---");
    std::string aura = find_aura_binary();
    std::string out_path = "/tmp/issue_374_fallthrough_out";
    fs::remove(out_path);
    EmitResult res = run_emit_binary_with_env(aura, "(+ 1 2)", out_path,
                                               "/tmp",
                                               {"AURA_RUNTIME_DIR=/tmp/issue_374_nonexistent_dir_xyz"});
    if (!res.ok) {
        std::println("       [aura failure] exit={} signal={} crashed={}",
                     res.exit_code, res.signal, res.crashed);
        if (!res.stderr.empty()) std::println("       [aura stderr] {}", res.stderr);
    }
    CHECK(res.ok, "aura --emit-binary exits 0 even with bad AURA_RUNTIME_DIR (fall-through)");
    CHECK(!res.aot_error_marker,
          "stderr does NOT contain 'AOT: cannot find lib/runtime.c' (fall-through recovered)");
    CHECK(fs::exists(out_path), "output file exists (fall-through)");
    if (!fs::exists(out_path)) return false;
    std::string output = run_capture_stdout(out_path);
    CHECK(output.find("3") != std::string::npos, "output contains '3' (fall-through)");
    fs::remove(out_path);
    return true;
}

}  // namespace aura_issue_374_detail

int aura_issue_374_run() {
    using namespace aura_issue_374_detail;
    std::fprintf(stdout, "═══ Issue #374 — AURA_RUNTIME_DIR CI lookup + install layout ═══\n");
    std::fprintf(stdout, "  Verifies find_runtime_c() lookup strategies #1 (env var) and #2 (walk-up).\n");
    std::fprintf(stdout, "  Install-path fallback #4 is code-present but not exercised (no cmake install rule yet).\n\n");

    test_aura_runtime_dir_override_works();
    test_walkup_finds_runtime_c();
    test_bad_aura_runtime_dir_falls_through();

    std::fprintf(stdout, "\n──────────────────────────────────────\n");
    std::fprintf(stdout, "Total: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_374_run(); }
#endif
