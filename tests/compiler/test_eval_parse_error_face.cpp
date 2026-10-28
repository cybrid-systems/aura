// @category: unit
// @reason: Issue #4736 — unbalanced source is a PARSE ERROR, not (). eval
//          returned make_void() on a parse failure, so a truncated/degenerate
//          candidate looked like a successful evaluation of nothing and a gate
//          built on eval could not reject it. mutate:rebind had the same root
//          cause one layer down: parse_define consumed the end-of-input token
//          where a ')' was required, so ONE missing trailing paren was silently
//          auto-closed and the truncated define installed (two missing parens
//          failed only because the inner call hit parse_list's #4342 guard).
//
//   AC1: (eval "(+ 1 (* 2 3)") is a catchable parse-error
//   AC2: (eval "(+ 1 2))") is a catchable parse-error
//   AC3: mutate:rebind with one missing ')' is a parse-error AND the old
//        definition stays installed (f 1 == 3, never the auto-closed body)
//   AC4: no-forms source (empty / comment-only) still evaluates to ()
//   AC5: source-cite — parser_impl.cpp + evaluator_primitives_eval.cpp cite
//        #4736 and parse_define refuses EOF where ')' is required

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

static bool eval_bool(CompilerService& cs, const char* src, const char* what) {
    auto r = cs.eval(src);
    const bool ok = r && is_bool(*r) && as_bool(*r);
    if (!ok)
        std::println("  [{}] result was not #t", what);
    return ok;
}

// ── AC1 + AC2: eval surfaces the parse failure ───────────────────────
// The catch clause returns a STRING sentinel (symbol identity is pool-local).
static void ac1_eval_unbalanced_is_parse_error() {
    std::println("\n--- #4736 AC1: eval of unbalanced source is a parse-error ---");
    CompilerService cs;
    CHECK(eval_bool(cs,
                    "(equal? (try (eval \"(+ 1 (* 2 3)\") (catch (e) \"raised\")) "
                    "\"raised\")",
                    "missing-close"),
          "AC1: one missing ')' is a catchable parse-error");
    CHECK(eval_bool(cs,
                    "(equal? (try (eval \"(+ 1 2))\") (catch (e) \"raised\")) "
                    "\"raised\")",
                    "extra-close"),
          "AC2: an extra ')' is a catchable parse-error");
}

// ── AC3: mutate:rebind is the gate ───────────────────────────────────
// A successful rebind returns #t; a rejected one returns the (parse-error …)
// pair. So pair? is the tag-free discriminator, and (f 1) proves the old
// definition survived.
static void ac3_rebind_one_short_rejected() {
    std::println("\n--- #4736 AC3: rebind with one missing ')' is rejected ---");
    CompilerService cs;
    const char* src = R"SRC(
(set-code "(define (f x) (+ x 1))")
(eval-current)
(define rb (try (mutate:rebind "f" "(define (f x) (+ x 2)" "probe") (catch (e) e)))
(define rb2 (try (mutate:rebind "f" "(define (f x) (+ x 4" "probe") (catch (e) e)))
(and (pair? rb) (pair? rb2) (= (f 1) 2))
)SRC";
    auto r = cs.eval(src);
    const bool ok = r && is_bool(*r) && as_bool(*r);
    CHECK(ok, "AC3: one-short and two-short rebinds are parse-errors; f stays (+ x 1)");
    // (+ x 1) with x=1 is 2: the rejected rebind installed nothing.
    auto f = cs.eval("(f 1)");
    CHECK(f && aura::compiler::types::is_int(*f) && aura::compiler::types::as_int(*f) == 2,
          "AC3: the original definition is still installed (f 1 == 2)");
}

// ── AC4: a no-forms program is still a successful empty program ──────
static void ac4_no_forms_stays_void() {
    std::println("\n--- #4736 AC4: empty / comment-only source stays () ---");
    CompilerService cs;
    CHECK(eval_bool(cs, "(equal? (eval \"\") (eval \"; only a comment\"))", "no-forms"),
          "AC4: empty and comment-only source both evaluate to ()");
}

// ── AC5: source-cite ─────────────────────────────────────────────────
static void ac5_source_cite() {
    std::println("\n--- #4736 AC5: source cites #4736 ---");
    const auto parser = read_file("src/parser/parser_impl.cpp");
    CHECK(parser.find("#4736") != std::string::npos, "AC5: parse_define cites #4736");
    CHECK(parser.find("NodeId parse_define(ParserState& s)") != std::string::npos,
          "AC5: parse_define is the strict-close source");
    const auto eval_src = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(eval_src.find("#4736") != std::string::npos, "AC5: the eval primitive cites #4736");
    CHECK(eval_src.find("\"parse-error: \"") != std::string::npos,
          "AC5: eval returns a parse-error-valued failure");
}

} // namespace

int run_test_eval_parse_error_face() {
    std::println("=== Issue #4736: unbalanced source is a parse error ===");
    ac1_eval_unbalanced_is_parse_error();
    ac3_rebind_one_short_rejected();
    ac4_no_forms_stays_void();
    ac5_source_cite();
    std::println("\n=== #4736: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_eval_parse_error_face();
}
#endif
