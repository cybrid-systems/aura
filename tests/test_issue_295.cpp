// @category: integration
// @reason: Issue #295 — Self-Evolving HV Loop Phase 0
//
// Validates:
//  - (eda:query:coverage-holes m) returns signals without assertions
//  - (ws:try-mutation expr-string) wraps eval in snapshot/rollback
//  - Rollback returns #f on failure
//  - Coverage holes exclude signals that ARE asserted
#include <unistd.h>
#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

namespace test_295_detail {

static std::string run_aura(const std::string& src) {
    char tmpl[] = "/tmp/test_295_aura_XXXXXX.aura";
    int fd = mkstemps(tmpl, 5);
    if (fd < 0) return "<mkstemps-fail>";
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
    static const std::string aura_bin = []() -> std::string {
        if (auto* env = std::getenv("AURA_BIN")) return env;
        return "./build/aura";
    }();
    // Compute the repo root: parent of build/.
    static const std::string repo_root = []() -> std::string {
        if (auto* env = std::getenv("AURA_SRC_ROOT")) return env;
        return "..";
    }();
    std::string cmd = std::string("(cd ") + repo_root + " && timeout 10 " +
                      aura_bin + " < " + tmpl + " 2>&1)";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { unlink(tmpl); return "<popen-fail>"; }
    char buf[4096]; std::string out;
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int rc = pclose(p); unlink(tmpl);
    if (rc != 0 && out.find("error:") == std::string::npos) {
        return "<non-zero exit: rc=" + std::to_string(rc) + ">";
    }
    return out;
}

// Helper: read first line of output
static std::string first_line(const std::string& out) {
    auto p = out.find('\n');
    return p == std::string::npos ? out : out.substr(0, p);
}

// AC #1: coverage-holes returns signals without assertions
bool test_coverage_holes() {
    std::println("\n--- AC #1: coverage-holes returns un-asserted signals ---");
    std::string src = R"AU(
(load "lib/std/eda.aura")
(define m
  (make-eda:module 'mod
    (list
      (make-eda:port "clk_i" 'input 1)
      (make-eda:port "q" 'output 1)
      (make-eda:port "err" 'output 1))
    (list
      (make-eda:assertion 'assert "q_stable"
        (make-eda:property "q_stable"
          (make-eda:expr 'symbol (list "q")))))))
(display (length (eda:query:coverage-holes m)))
(newline)
)AU";
    auto out = run_aura(src);
    auto line = first_line(out);
    // clk_i, err are un-asserted (2 holes); q is asserted (not a hole)
    CHECK(line == "2",
          "2 un-asserted signals (got \"" + line + "\")");
    return true;
}

// AC #2: coverage-holes returns empty when all signals are asserted
bool test_coverage_holes_full() {
    std::println("\n--- AC #2: all-asserted → empty coverage holes ---");
    std::string src = R"AU(
(load "lib/std/eda.aura")
(define m
  (make-eda:module 'mod
    (list
      (make-eda:port "clk_i" 'input 1)
      (make-eda:port "q" 'output 1))
    (list
      (make-eda:assertion 'assert "all"
        (make-eda:property "all"
          (make-eda:expr 'symbol (list "clk_i"))))
      (make-eda:assertion 'assert "q"
        (make-eda:property "q"
          (make-eda:expr 'symbol (list "q")))))))
(display (length (eda:query:coverage-holes m)))
(newline)
)AU";
    auto out = run_aura(src);
    auto line = first_line(out);
    CHECK(line == "0",
          "no coverage holes when all signals asserted (got \"" + line + "\")");
    return true;
}

// AC #3: ws:try-mutation on success returns (result . snap-id)
bool test_ws_try_success() {
    std::println("\n--- AC #3: ws:try-mutation success ---");
    std::string src = R"AU(
(define r (ws:try-mutation "(+ 1 2)"))
(display "is-pair=") (display (pair? r)) (newline)
(display "car=") (display (car r)) (newline)
)AU";
    auto out = run_aura(src);
    // First line: is-pair, second: car=3
    auto lines = out;
    auto first_nl = lines.find('\n');
    auto first = lines.substr(0, first_nl);
    auto second = lines.substr(first_nl+1, lines.find('\n', first_nl+1) - first_nl - 1);
    CHECK(first == "is-pair=#t",
          "ws:try-mutation returns a pair (got \"" + first + "\")");
    CHECK(second == "car=3",
          "car is the result (got \"" + second + "\")");
    return true;
}

// AC #4: ws:try-mutation on parse failure returns #f (rollback)
bool test_ws_try_rollback() {
    std::println("\n--- AC #4: ws:try-mutation rollback on failure ---");
    std::string src = R"AU(
(define r (ws:try-mutation "((unbalanced parens"))
(display "r=") (display r) (newline)
)AU";
    auto out = run_aura(src);
    auto line = first_line(out);
    CHECK(line == "r=#f",
          "parse failure returns #f (got \"" + line + "\")");
    return true;
}

// AC #5: ws:try-mutation with bad arg returns #f
bool test_ws_try_bad_arg() {
    std::println("\n--- AC #5: ws:try-mutation bad arg ---");
    std::string src = R"AU(
(define r (ws:try-mutation 42))
(display "r=") (display r) (newline)
)AU";
    auto out = run_aura(src);
    auto line = first_line(out);
    CHECK(line == "r=#f",
          "non-string arg returns #f (got \"" + line + "\")");
    return true;
}

int run_tests() {
    std::println("═══ Issue #295 ═══");
    test_coverage_holes();
    test_coverage_holes_full();
    test_ws_try_success();
    test_ws_try_rollback();
    test_ws_try_bad_arg();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══\n", g_passed, g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

}

int aura_issue_295_run() { return test_295_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_295_run(); }
#endif
