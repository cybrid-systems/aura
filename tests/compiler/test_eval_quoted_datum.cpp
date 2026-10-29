// @category: unit
// @reason: Issue #4749 — eval only evaluated STRINGS. (eval '(+ 1 2)) was
//          () with no error and no warning, so code-as-data programs
//          (LLM/swarm-generated policies that build a form as a list) had to
//          render their own text before every eval. The datum now takes the
//          same path as its source text: a list evaluates as code, a symbol
//          resolves, a number is itself. Non-datum arguments (a closure, an
//          error) are an explicit, catchable error instead of a silent ().
//
//   AC1: (eval '(+ 1 2)) is 3
//   AC2: (eval 5) is 5 and (eval #t) is #t
//   AC3: a quoted lambda datum is a procedure and calls
//   AC4: a list built with (list 'lambda ...) is a procedure and calls
//   AC5: (eval ''a) is the symbol a
//   AC6: a datum with a special-form head runs as code
//   AC7: eval of a datum names a define, and the define is visible
//   AC8: strings are unchanged — value + #4736 parse-error face
//   AC9: a non-datum argument is a catchable error, not ()
//   AC10: source-cite — evaluator_primitives_eval.cpp cites #4749

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::is_bool;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static bool eval_true(CompilerService& cs, const char* src, const char* what) {
    auto r = cs.eval(src);
    const bool ok = r && is_bool(*r) && as_bool(*r);
    if (!ok)
        std::println("  [{}] result was not #t", what);
    return ok;
}

// ── AC1/AC2: numbers and lists ─────────────────────────────────────
static void ac1_eval_list_datum() {
    std::println("\n--- #4749 AC1: (eval '(+ 1 2)) is 3 ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(= (eval '(+ 1 2)) 3)", "eval list"), "AC1: a quoted list evaluates");
}

static void ac2_eval_atom_datums() {
    std::println("\n--- #4749 AC2: (eval 5) / (eval #t) ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(= (eval 5) 5)", "eval int"), "AC2: (eval 5) is 5");
    CHECK(eval_true(cs, "(equal? (eval #t) #t)", "eval bool"), "AC2: (eval #t) is #t");
    CHECK(eval_true(cs, "(equal? (eval 2.5) 2.5)", "eval float"), "AC2: (eval 2.5) is 2.5");
}

// ── AC3/AC4: procedures from data ──────────────────────────────────
static void ac3_eval_quoted_lambda() {
    std::println("\n--- #4749 AC3: a quoted lambda datum is a procedure ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(procedure? (eval '(lambda (x) (+ x 1))))", "eval lambda"),
          "AC3: (eval '(lambda (x) ...)) is a procedure");
    CHECK(eval_true(cs, "(begin (define f (eval '(lambda (x) (+ x 1)))) (= (f 41) 42))",
                    "eval lambda call"),
          "AC3: the procedure calls (f 41) == 42");
}

static void ac4_eval_built_list() {
    std::println("\n--- #4749 AC4: a (list 'lambda ...) datum is a procedure ---");
    CompilerService cs;
    CHECK(eval_true(cs,
                    "(begin (define g (eval (list 'lambda (list 'x) (list '+ 'x 1)))) "
                    "(= (g 1) 2))",
                    "eval built list"),
          "AC4: a list built at runtime evaluates as code");
}

// ── AC5/AC6: atoms and special forms ───────────────────────────────
static void ac5_eval_quoted_symbol() {
    std::println("\n--- #4749 AC5: (eval ''a) is the symbol a ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(symbol? (eval ''a))", "eval quoted symbol"),
          "AC5: a quoted symbol evaluates to the symbol");
}

static void ac6_eval_special_form_datum() {
    std::println("\n--- #4749 AC6: a datum with a special-form head runs ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(= (eval (quote (if #t 7 9))) 7)", "eval if datum"),
          "AC6: (eval (quote (if #t 7 9))) is 7");
    CHECK(eval_true(cs, "(= (eval (quote (let ((a 2)) (+ a 3)))) 5)", "eval let datum"),
          "AC6: (eval (quote (let ...))) is 5");
}

// ── AC7: a define through the datum face is visible ────────────────
static void ac7_eval_datum_define() {
    std::println("\n--- #4749 AC7: a datum define binds its name ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(begin (eval '(define datum-defined 7)) (= datum-defined 7))",
                    "eval define"),
          "AC7: (eval '(define ...)) binds the name");
}

// ── AC8: the string face is unchanged ──────────────────────────────
static void ac8_string_face_unchanged() {
    std::println("\n--- #4749 AC8: the string face is unchanged ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(= (eval \"(+ 1 2)\") 3)", "eval string"),
          "AC8: (eval \"(+ 1 2)\") is still 3");
    CHECK(eval_true(cs, "(equal? (try (eval \"(+ 1 (* 2 3)\") (catch (e) \"raised\")) \"raised\")",
                    "eval string parse error"),
          "AC8: #4736 parse errors stay catchable");
}

// ── AC9: a non-datum argument is an explicit error ─────────────────
static void ac9_non_datum_is_error() {
    std::println("\n--- #4749 AC9: a non-datum argument is an error ---");
    CompilerService cs;
    CHECK(eval_true(cs, "(equal? (try (eval (lambda (x) x)) (catch (e) \"raised\")) \"raised\")",
                    "eval closure raises"),
          "AC9: (eval <closure>) is a catchable error");
}

// ── AC10: source cite ─────────────────────────────────────────────
static void ac10_source_cite() {
    std::println("\n--- #4749 AC10: the eval primitive cites the datum path ---");
    const auto src = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(!src.empty(), "AC10: evaluator_primitives_eval.cpp readable");
    CHECK(src.find("#4749") != std::string::npos, "AC10: the eval primitive cites #4749");
}

} // namespace

int run_test_eval_quoted_datum() {
    std::println("=== Issue #4749: eval of a datum ===");
    ac1_eval_list_datum();
    ac2_eval_atom_datums();
    ac3_eval_quoted_lambda();
    ac4_eval_built_list();
    ac5_eval_quoted_symbol();
    ac6_eval_special_form_datum();
    ac7_eval_datum_define();
    ac8_string_face_unchanged();
    ac9_non_datum_is_error();
    ac10_source_cite();
    std::println("\n=== #4749: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_eval_quoted_datum();
}
#endif
