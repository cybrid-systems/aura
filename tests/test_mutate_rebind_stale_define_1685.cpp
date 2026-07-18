// @category: unit
// @reason: Issue #1685 — re-resolve Define after parse_to_flat so rebind /
// set-body never apply set_child to a wrong or recycled NodeId after SoA growth.
//
//   AC1: multi-define workspace — rebind only changes the named binding
//   AC2: rebind with full (define name ...) form still hits original define
//   AC3: set-body after large sibling parse growth still works
//   AC4: capacity-boundary rebind (many pad defines) keeps correct semantics

#include "test_harness.hpp"

#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

bool eval_ok(CompilerService& cs, const std::string& expr) {
    return cs.eval(expr).has_value();
}

bool rebind_ok(CompilerService& cs, const std::string& name, const std::string& code,
               const std::string& summary) {
    auto expr =
        std::string("(mutate:rebind \"") + name + "\" \"" + code + "\" \"" + summary + "\")";
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

bool set_body_ok(CompilerService& cs, const std::string& name, const std::string& body,
                 const std::string& summary) {
    auto expr =
        std::string("(mutate:set-body \"") + name + "\" \"" + body + "\" \"" + summary + "\")";
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

std::optional<std::int64_t> eval_int(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval(expr);
    if (r && is_int(*r))
        return as_int(*r);
    (void)cs.eval("(eval-current)");
    r = cs.eval(expr);
    if (r && is_int(*r))
        return as_int(*r);
    return std::nullopt;
}

bool int_eq(CompilerService& cs, const std::string& expr, std::int64_t want) {
    auto v = eval_int(cs, expr);
    return v && *v == want;
}

} // namespace

int main() {
    CompilerService cs;

    // ── AC1: rebind only the named define among many ──
    {
        std::println("\n--- AC1: multi-define rebind locality ---");
        const char* multi = "(set-code \"(define (a x) (+ x 1)) (define (b y) (+ y 2)) "
                            "(define (c z) (+ z 3))\")";
        CHECK((eval_ok(cs, multi)), "set-code a/b/c");
        CHECK((rebind_ok(cs, "b", "(lambda (y) (* y 10))", "1685-ac1")), "rebind b #t");
        CHECK((int_eq(cs, "(a 10)", 11)), "a unchanged (11)");
        CHECK((int_eq(cs, "(b 10)", 100)), "b rebound (100)");
        CHECK((int_eq(cs, "(c 10)", 13)), "c unchanged (13)");
    }

    // ── AC2: full define form still rebinds original, not parse-appended root ──
    {
        std::println("\n--- AC2: rebind with full define form ---");
        CHECK((rebind_ok(cs, "a", "(define (a x) (+ x 100))", "1685-ac2")),
              "rebind a full-define #t");
        CHECK((int_eq(cs, "(a 1)", 101)), "a → 101 after full-define rebind");
        CHECK((int_eq(cs, "(b 3)", 30)), "b still 30 (not corrupted)");
    }

    // ── AC3: set-body after growth ──
    {
        std::println("\n--- AC3: set-body after sibling rebinds ---");
        for (int i = 0; i < 8; ++i) {
            std::string code = "(lambda (z) (+ z " + std::to_string(i) + "))";
            CHECK((rebind_ok(cs, "c", code, "grow")), "grow rebind c");
        }
        CHECK((set_body_ok(cs, "c", "(* z 7)", "1685-ac3")), "set-body c #t");
        CHECK((int_eq(cs, "(c 5)", 35)), "c 5 → 35 after set-body");
        CHECK((int_eq(cs, "(a 1)", 101)), "a still 101 after set-body c");
    }

    // ── AC4: many pad defines near capacity, rebind still correct ──
    {
        std::println("\n--- AC4: capacity-boundary style pad + rebind ---");
        std::string src = "(define (target n) (+ n 1))";
        for (int i = 0; i < 64; ++i) {
            src += " (define (pad";
            src += std::to_string(i);
            src += " x) (+ x ";
            src += std::to_string(i);
            src += "))";
        }
        auto set = std::string("(set-code \"") + src + "\")";
        CHECK((eval_ok(cs, set)), "set-code target + 64 pads");
        CHECK((rebind_ok(cs, "target", "(lambda (n) (* n 2))", "1685-ac4")),
              "rebind target #t after pad");
        CHECK((int_eq(cs, "(target 21)", 42)), "target 21 → 42");
        CHECK((int_eq(cs, "(pad0 1)", 1)), "pad0 unchanged");
        CHECK((int_eq(cs, "(pad63 1)", 64)), "pad63 unchanged");
    }

    std::println("\n=== test_mutate_rebind_stale_define_1685: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
