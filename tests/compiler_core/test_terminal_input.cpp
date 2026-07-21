// test_terminal_input.cpp — Issue #1353: raw mode + non-blocking poll + event parse

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;

namespace {

// Embed raw bytes into an Aura string literal for tui:inject-bytes.
bool inject_raw(CompilerService& cs, std::string_view bytes) {
    std::string expr = "(tui:inject-bytes \"";
    for (unsigned char c : bytes) {
        if (c == '"' || c == '\\') {
            expr.push_back('\\');
            expr.push_back(static_cast<char>(c));
        } else {
            // Pass through as raw octet (Aura string heap is byte-oriented).
            expr.push_back(static_cast<char>(c));
        }
    }
    expr += "\")";
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

bool eval_true(CompilerService& cs, const char* expr) {
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // AC3: non-blocking empty poll → #f
    {
        auto e = cs.eval("(tui:read-event 0)");
        CHECK(e && is_bool(*e) && !as_bool(*e), "read-event timeout=0 empty → #f");
    }

    // AC7: raw-mode-on idempotent; raw-mode-off
    {
        auto a = cs.eval("(tui:raw-mode-on)");
        CHECK(a && is_bool(*a) && as_bool(*a), "raw-mode-on");
        auto b = cs.eval("(tui:raw-mode-on)");
        CHECK(b && is_bool(*b) && as_bool(*b), "raw-mode-on idempotent");
        auto ir = cs.eval("(tui:is-raw-mode)");
        CHECK(ir && is_bool(*ir) && as_bool(*ir), "is-raw-mode after on");
        auto off = cs.eval("(tui:raw-mode-off)");
        CHECK(off && is_bool(*off) && as_bool(*off), "raw-mode-off");
        auto ir2 = cs.eval("(tui:is-raw-mode)");
        CHECK(ir2 && is_bool(*ir2) && !as_bool(*ir2), "is-raw-mode after off");
    }

    // Arrow keys via inject-bytes CSI
    {
        (void)cs.eval("(tui:raw-mode-on)");
        CHECK(inject_raw(cs, "\x1b[A"), "inject up arrow CSI");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "↑"))))"),
              "↑ arrow key event");

        CHECK(inject_raw(cs, "\x1b[B"), "inject down");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "↓"))))"),
              "↓ arrow key event");

        CHECK(inject_raw(cs, "\x1b[C"), "inject right");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "→"))))"),
              "→ arrow key event");

        CHECK(inject_raw(cs, "\x1b[D"), "inject left");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "←"))))"),
              "← arrow key event");
        (void)cs.eval("(tui:raw-mode-off)");
    }

    // Enter / Space / Escape / plain char
    {
        CHECK(inject_raw(cs, "\r"), "inject enter");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key"))))"),
              "Enter → key event");

        CHECK(inject_raw(cs, " "), "inject space");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) " "))))"),
              "Space → key");

        CHECK(inject_raw(cs, "\x1b"), "inject esc");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key"))))"),
              "Escape → key");

        CHECK(inject_raw(cs, "q"), "inject q");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "q"))))"),
              "q → key");
    }

    // Ctrl-C → quit
    {
        CHECK(inject_raw(cs, std::string_view("\x03", 1)), "inject ctrl-c");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "quit"))))"),
              "Ctrl-C → quit event");
    }

    // Mouse SGR: ESC [ < 0 ; 5 ; 10 M  → (mouse . (0 . (5 . 10)))
    {
        CHECK(inject_raw(cs, "\x1b[<0;5;10M"), "inject mouse press");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "mouse")
                                   (pair? (cdr e))
                                   (= (car (cdr e)) 0)
                                   (= (car (cdr (cdr e))) 5)
                                   (= (cdr (cdr (cdr e))) 10))))"),
              "mouse SGR event btn/col/row");
    }

    // tui:terminal-size returns positive pair
    {
        CHECK(eval_true(cs,
                        R"((let ((s (tui:terminal-size)))
                              (and (pair? s) (> (car s) 0) (> (cdr s) 0))))"),
              "terminal-size positive rows/cols");
    }

    // tui:enable-mouse
    {
        auto m = cs.eval("(tui:enable-mouse)");
        CHECK(m && is_bool(*m) && as_bool(*m), "enable-mouse");
    }

    // Legacy inject-key still works with tui:init
    {
        (void)cs.eval("(tui:init \"in\" 8 4)");
        (void)cs.eval("(tui:inject-key \"x\")");
        CHECK(eval_true(cs,
                        R"((let ((e (tui:read-event 0)))
                              (and (pair? e) (equal? (car e) "key")
                                   (equal? (cdr e) "x"))))"),
              "inject-key still works");
        (void)cs.eval("(tui:shutdown)");
    }

    // Empty poll after consuming all
    {
        auto e = cs.eval("(tui:read-event 0)");
        CHECK(e && is_bool(*e) && !as_bool(*e), "empty after drain → #f");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("terminal input #1353: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
