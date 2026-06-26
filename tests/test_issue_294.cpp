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
#include <iostream>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include "test_harness.hpp"
using aura::test::g_passed;
using aura::test::g_failed;

namespace test_294_detail {

// Helper: run Aura source via a temp file, return stdout.
// Aura source contains parens that confuse bash echo, so use
// a temp file + redirection.
static std::string run_aura(const std::string& src) {
    char tmpl[] = "/tmp/test_294_aura_XXXXXX.aura";
    int fd = mkstemps(tmpl, 5);
    if (fd < 0) return "<mkstemps-fail>";
    write(fd, src.data(), src.size());
    close(fd);
    std::string cmd = std::string("timeout 10 build/aura < ") + tmpl + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        unlink(tmpl);
        return "<popen-fail>";
    }
    char buf[4096];
    std::string out;
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int rc = pclose(p);
    unlink(tmpl);
    if (rc != 0 && out.find("error:") == std::string::npos) {
        return "<non-zero exit: rc=" + std::to_string(rc) + ">";
    }
    return out;
}

// AC #1: eda:query:always-ff-with-clock returns matching always_ff blocks
bool test_always_ff_with_clock() {
    std::cout << "\n--- AC #1: eda:query:always-ff-with-clock ---\n";
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
    CHECK(first_line == "1",
          "matches exactly 1 always_ff with clk_i (got \"" +
          first_line + "\")");
    return true;
}

// AC #2: eda:query:assertions-involving-signal returns matching assertions
bool test_assertions_involving() {
    std::cout << "\n--- AC #2: eda:query:assertions-involving-signal ---\n";
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
          "matches exactly 1 assertion mentioning x (got \"" +
          first_line + "\")");
    return true;
}

// AC #3: eda:query:reset-condition-for-register returns the reset signal
bool test_reset_condition() {
    std::cout << "\n--- AC #3: eda:query:reset-condition-for-register ---\n";
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
    CHECK(first_line == "rst_ni",
          "reset signal is rst_ni (got \"" + first_line + "\")");
    return true;
}

// AC #4: reset-condition returns #f when no multi-edge sensitivity
bool test_reset_no_match() {
    std::cout << "\n--- AC #4: no multi-edge → #f ---\n";
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
    std::cout << "\n--- AC #5: wrong clock returns 0 ---\n";
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
    CHECK(first_line == "0",
          "no always_ff with non-existent clk (got \"" +
          first_line + "\")");
    return true;
}

int run_tests() {
    std::cout << "═══ Issue #294 ═══\n";
    test_always_ff_with_clock();
    test_assertions_involving();
    test_reset_condition();
    test_reset_no_match();
    test_always_ff_mismatch();
    std::cout << "\n═══ Results: " << g_passed << '/' << (g_passed + g_failed)
              << " passed, " << g_failed << '/' << (g_passed + g_failed)
              << " failed ═══\n";
    return g_failed > 0 ? 1 : 0;
}

}

int aura_issue_294_run() { return test_294_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_294_run(); }
#endif