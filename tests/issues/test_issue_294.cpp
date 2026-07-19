// @category: integration
// @reason: Issue #294 — Phase 0 query primitives for EDA IR
//
// Validates the 3 Phase 0 query helpers added to lib/std/eda.aura:
//   - eda:query:always-ff-with-clock
//   - eda:query:assertions-involving-signal
//   - eda:query:reset-condition-for-register
//
// Tests run via the Aura --script entry point (cs.eval doesn't have
// access to lib/std/eda.aura by default). The helpers are exercised
// by piping Aura source to the build/aura binary.
#include <unistd.h>
#include <filesystem>
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;

namespace test_294_detail {

// Helper: run Aura source via a temp file, return stdout.
// Aura source contains parens that confuse bash echo, so use
// a temp file + redirection.
static std::string run_aura(const std::string& src) {
    char tmpl[] = "/tmp/test_294_aura_XXXXXX.aura";
    int fd = mkstemps(tmpl, 5);
    if (fd < 0)
        return "<mkstemps-fail>";
    write(fd, src.data(), src.size());
    close(fd);
    // Path resolution: tests are launched from build/ so a
    // relative path like "build/aura" would resolve to
    // "build/build/aura" (non-existent → rc=32512 "command
    // not found"). Try env var AURA_BIN first (set by CI).
    //
    // IMPORTANT: we must cd to the source root before
    // running the aura binary, because the Aura source
    // references relative paths like (load "lib/std/eda.aura")
    // which only resolve from the repo root. From the
    // repo root the path to the binary is `./build/aura`.
    //
    // The previous hardcoded `repo_root = ".."` worked when
    // the bundle was launched from build/ (the normal case)
    // but broke when launched from the repo root directly
    // (e.g. `./build/test_issues_jit` from /home/dev/code/aura).
    // Now we walk up from cwd looking for the lib/std/eda.aura
    // marker so the test works from either cwd. AURA_SRC_ROOT
    // env var overrides (used by CI).
    static const std::string aura_bin = []() -> std::string {
        if (auto* env = std::getenv("AURA_BIN"))
            return env;
        // Default: the aura binary lives in build/aura relative
        // to the repo root. Use readlink("/proc/self/exe") for
        // an absolute path so the (cd repo_root && ./build/aura)
        // popen command works regardless of cwd.
        char buf[4096] = {0};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            std::filesystem::path p(buf);
            // p is .../build/test_issue_NNN; the sibling aura
            // binary is at .../build/aura.
            return (p.parent_path() / "aura").string();
        }
        return "./build/aura";
    }();
    static const std::string repo_root = []() -> std::string {
        if (auto* env = std::getenv("AURA_SRC_ROOT"))
            return env;
        // Walk up from cwd looking for lib/std/eda.aura marker.
        // This works whether the bundle was launched from
        // build/ (.. = repo root) or directly from the repo
        // root (cwd = repo root). Falls back to ".." if the
        // marker isn't found in any ancestor (shouldn't happen
        // in a normal checkout).
        namespace fs = std::filesystem;
        fs::path p = fs::current_path();
        while (!p.empty()) {
            if (fs::exists(p / "lib/std/eda.aura"))
                return p.string();
            if (p == p.root_path())
                break;
            p = p.parent_path();
        }
        return "..";
    }();
    std::string cmd =
        std::string("(cd ") + repo_root + " && timeout 10 " + aura_bin + " < " + tmpl + " 2>&1)";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        unlink(tmpl);
        return "<popen-fail>";
    }
    char buf[4096];
    std::string out;
    while (std::fgets(buf, sizeof(buf), p))
        out += buf;
    int rc = pclose(p);
    unlink(tmpl);
    if (rc != 0 && out.find("error:") == std::string::npos) {
        return "<non-zero exit: rc=" + std::to_string(rc) + ">";
    }
    return out;
}

// AC #1: eda:query:always-ff-with-clock returns matching always_ff blocks
bool test_always_ff_with_clock() {
    std::println("\n--- AC #1: eda:query:always-ff-with-clock ---");
    std::string src = R"AU(
(load "lib/std/eda.aura")
(define m
  (make-eda:module 'mod
    (list (make-eda:port 'input "clk_i" 1))
    (list
      (make-eda:always3 'always_ff
        (list (make-eda:sensitivity 'posedge
          (make-eda:expr 'symbol (list "clk_i"))))
        (make-eda:expr 'symbol (list "q1")))
      (make-eda:always3 'always_ff
        (list (make-eda:sensitivity 'posedge
          (make-eda:expr 'symbol (list "other_clk"))))
        (make-eda:expr 'symbol (list "q2"))))))
(display (length (eda:query:always-ff-with-clock "clk_i" m)))
(newline)
)AU";
    auto out = run_aura(src);
    // First line of output is the count
    auto first_line = out.substr(0, out.find('\n'));
    CHECK(first_line == "1", "matches exactly 1 always_ff with clk_i (got \"" + first_line + "\")");
    return true;
}

// AC #2: eda:query:assertions-involving-signal returns matching assertions
bool test_assertions_involving() {
    std::println("\n--- AC #2: eda:query:assertions-involving-signal ---");
    std::string src = R"AU(
(load "lib/std/eda.aura")
(define m
  (make-eda:module 'mod
    (list)
    (list
      (make-eda:assertion 'assert "x_stable"
        (make-eda:property "x_stable"
          (make-eda:expr 'symbol (list "x"))))
      (make-eda:assertion 'assert "y_stable"
        (make-eda:property "y_stable"
          (make-eda:expr 'symbol (list "y")))))))
(display (length (eda:query:assertions-involving-signal "x" m)))
(newline)
)AU";
    auto out = run_aura(src);
    auto first_line = out.substr(0, out.find('\n'));
    CHECK(first_line == "1",
          "matches exactly 1 assertion mentioning x (got \"" + first_line + "\")");
    return true;
}

// AC #3: eda:query:reset-condition-for-register returns the reset signal
bool test_reset_condition() {
    std::println("\n--- AC #3: eda:query:reset-condition-for-register ---");
    std::string src = R"AU(
(load "lib/std/eda.aura")
(define m
  (make-eda:module 'mod
    (list)
    (list
      (make-eda:always3 'always_ff
        (list
          (make-eda:sensitivity 'posedge
            (make-eda:expr 'symbol (list "clk_i")))
          (make-eda:sensitivity 'negedge
            (make-eda:expr 'symbol (list "rst_ni"))))
        (make-eda:expr 'symbol (list "q"))))))
(display (eda:query:reset-condition-for-register "q" m))
(newline)
)AU";
    auto out = run_aura(src);
    auto first_line = out.substr(0, out.find('\n'));
    CHECK(first_line == "rst_ni", "reset signal is rst_ni (got \"" + first_line + "\")");
    return true;
}

// AC #4: reset-condition returns #f when no multi-edge sensitivity
bool test_reset_no_match() {
    std::println("\n--- AC #4: no multi-edge → #f ---");
    std::string src = R"AU(
(load "lib/std/eda.aura")
(define m
  (make-eda:module 'mod
    (list)
    (list
      (make-eda:always3 'always_ff
        (list
          (make-eda:sensitivity 'posedge
            (make-eda:expr 'symbol (list "clk_i"))))
        (make-eda:expr 'symbol (list "q"))))))
(display (eda:query:reset-condition-for-register "q" m))
(newline)
)AU";
    auto out = run_aura(src);
    auto first_line = out.substr(0, out.find('\n'));
    // Aura's #f display
    CHECK(first_line == "#f" || first_line == "()",
          "no multi-edge returns #f (got \"" + first_line + "\")");
    return true;
}

// AC #5: query:always-ff returns 0 for non-matching clk
bool test_always_ff_mismatch() {
    std::println("\n--- AC #5: wrong clock returns 0 ---");
    std::string src = R"AU(
(load "lib/std/eda.aura")
(define m
  (make-eda:module 'mod
    (list)
    (list
      (make-eda:always3 'always_ff
        (list
          (make-eda:sensitivity 'posedge
            (make-eda:expr 'symbol (list "clk_i"))))
        (make-eda:expr 'symbol (list "q"))))))
(display (length (eda:query:always-ff-with-clock "no_such_clk" m)))
(newline)
)AU";
    auto out = run_aura(src);
    auto first_line = out.substr(0, out.find('\n'));
    CHECK(first_line == "0", "no always_ff with non-existent clk (got \"" + first_line + "\")");
    return true;
}

int run_tests() {
    std::println("═══ Issue #294 ═══");
    test_always_ff_with_clock();
    test_assertions_involving();
    test_reset_condition();
    test_reset_no_match();
    test_always_ff_mismatch();
    std::println("\n═══ Results: {}{}{} passed, {}{}{} failed ═══\n", g_passed, '/',
                 (g_passed + g_failed), g_failed, '/', (g_passed + g_failed));
    return g_failed > 0 ? 1 : 0;
}

} // namespace test_294_detail

int aura_issue_294_run() {
    return test_294_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_294_run();
}
#endif