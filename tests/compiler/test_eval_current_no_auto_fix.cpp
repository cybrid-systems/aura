// @category: unit
// @reason: Issue #2484 — eval-current must not auto-invoke workspace
//          Defines when the result is a closure (side-effect / DoS).
//
//   AC1: last form lambda → closure returned unchanged
//   AC2: arity-0 define → still a closure (not auto-called body result)
//   AC3: source has #2484 + no auto-fix machinery
//   AC4: gate wiring

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
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_closure;
using aura::compiler::types::is_int;
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

// AC1: set-code with a lambda last form; eval-current returns the closure.
static void ac1_closure_unchanged() {
    std::println("\n--- #2484 AC1: eval-current returns closure ---");
    CompilerService cs;
    auto r = cs.eval("(begin (set-code \"(define (double x) (* x 2))\\n(lambda (y) (+ y 1))\") "
                     "(eval-current))");
    CHECK(r.has_value(), "AC1: eval ok");
    CHECK(r && is_closure(*r), "AC1: result is closure (not auto-called int)");

    auto r3 =
        cs.eval("(begin (set-code \"(define (double x) (* x 2))\") (eval-current) (double 21))");
    CHECK(r3.has_value() && is_int(*r3) && as_int(*r3) == 42, "AC1: explicit (double 21) → 42");
}

// AC2: (define (f) 99) as workspace — old auto-fix arity-0 would call (f)
// and return 99. New path returns the define/closure result without invoking.
static void ac2_no_auto_call_arity0() {
    std::println("\n--- #2484 AC2: arity-0 define not auto-called ---");
    CompilerService cs;
    // Workspace whose last form is a define of nullary fn with body 99.
    // eval-current must NOT return 99 (that would mean auto-call ran).
    auto r = cs.eval("(begin (set-code \"(define (probe-auto-fix-2484) 99)\") (eval-current))");
    CHECK(r.has_value(), "AC2: eval ok");
    // Accept either void (define returns void) or closure — never 99.
    if (r && is_int(*r)) {
        CHECK(as_int(*r) != 99, "AC2: must not return auto-call body 99");
    } else {
        CHECK(r.has_value(), "AC2: got non-int (closure/void) without auto-call");
        if (r)
            CHECK(!is_int(*r) || as_int(*r) != 99, "AC2: not body result");
    }
    // Explicit call still yields 99
    auto call = cs.eval("(begin (set-code \"(define (probe-auto-fix-2484) 99)\") "
                        "(eval-current) (probe-auto-fix-2484))");
    CHECK(call.has_value() && is_int(*call) && as_int(*call) == 99,
          "AC2: explicit (probe-auto-fix-2484) → 99");
}

static void ac3_source() {
    std::println("\n--- #2484 AC3: source contracts ---");
    auto src = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(!src.empty(), "AC3: read eval primitives");
    CHECK(src.find("Issue #2484") != std::string::npos, "AC3: cites #2484");
    CHECK(src.find("auto-fix-on-closure REMOVED") != std::string::npos ||
              src.find("auto-fix") != std::string::npos,
          "AC3: documents auto-fix removal");
    CHECK(src.find("winning_call") == std::string::npos, "AC3: no winning_call");
    CHECK(src.find("auto_fixed") == std::string::npos, "AC3: no auto_fixed");
    CHECK(src.find("arg_pats") == std::string::npos, "AC3: no arg_pats");
    CHECK(src.find("list 3 1 4 1 5") == std::string::npos, "AC3: no hardcoded list probe");
}

static void ac4_gate() {
    std::println("\n--- #2484 AC4: gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    CHECK(build.find("check_eval_current_no_auto_fix_2484") != std::string::npos,
          "AC4: check script in build.py");
    CHECK(build.find("cmd_eval_current_no_auto_fix_coverage") != std::string::npos,
          "AC4: coverage cmd");
    CHECK(cmake.find("test_eval_current_no_auto_fix") != std::string::npos, "AC4: cmake test");
    CHECK(!read_file("scripts/coverage/checks/check_eval_current_no_auto_fix_2484.py").empty(),
          "AC4: check script exists");
}

} // namespace

static void ac3915_define_rhs_binds() {
    std::println("\n--- #3915: define-RHS of set-code+eval-current binds ---");
    CompilerService cs;
    auto r = cs.eval(
        "(begin (define seeded (begin (set-code \"(define (f x) x)\") (eval-current) #t)) seeded)");
    CHECK(r.has_value() && is_bool(*r) && as_bool(*r), "3915 AC: seeded bound to #t");
    auto f3 = cs.eval("(f 3)");
    CHECK(f3.has_value() && is_int(*f3) && as_int(*f3) == 3, "3915 AC: workspace f still live");
    auto ge = cs.eval("(>= 1 0)");
    CHECK(ge.has_value() && is_bool(*ge) && as_bool(*ge), "3915 AC: >= still bound");
    const auto ev = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(ev.find("Issue #3915") != std::string::npos, "3915 AC: eval-current cites restore pool");
    CHECK(ev.find("RestoreTopPool") != std::string::npos, "3915 AC: RestoreTopPool");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("Issue #3915") != std::string::npos, "3915 AC: rest-args TCO cites");
}

static void ac3915_round_once_rest_args() {
    std::println("\n--- #3915: round-once rest-args closed-loop-once ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/agent\" all:)").has_value(), "3915B require agent");
    CHECK(cs.eval("(require \"std/mutate\" all:)").has_value(), "3915B require mutate");
    CHECK(cs.eval("(set-code \"(define (f x) x)\")").has_value(), "3915B set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3915B seed");
    CHECK(cs.eval("(define (round-once body) (agent:closed-loop-once :skip-set-code :rebind "
                  "\"f\" body :summary \"r\"))")
              .has_value(),
          "3915B define round-once");
    for (int i = 0; i < 3; ++i) {
        auto r = cs.eval("(round-once \"(lambda (x) x)\")");
        CHECK(r.has_value(), "3915B AC: round-once callable");
    }
    auto ge = cs.eval("(>= 1 0)");
    CHECK(ge.has_value() && is_bool(*ge) && as_bool(*ge), "3915B AC: >= still bound");
    auto wr = cs.eval("(begin (write 1) 1)");
    CHECK(wr.has_value() && is_int(*wr) && as_int(*wr) == 1, "3915C AC: write still bound");
}

static void ac3917_extra_paren_cli() {
    std::println("\n--- #3917: extra ) at top level is a hard CLI read error ---");
    const auto main = read_file("src/main.cpp");
    CHECK(main.find("Issue #3917") != std::string::npos, "3917 AC: main.cpp cites");
    CHECK(main.find("unexpected '{}'") != std::string::npos ||
              main.find("unexpected") != std::string::npos,
          "3917 AC: unexpected extra close");
    CHECK(main.find("depth == 0") != std::string::npos, "3917 AC: depth-0 extra close");
    CompilerService cs;
    auto star = cs.eval("(begin (define *skip-left* 2) (set! *skip-left* (- *skip-left* 1)) "
                        "*skip-left*)");
    CHECK(star.has_value() && is_int(*star) && as_int(*star) == 1,
          "3917 AC: *skip-left* is one identifier");
}

static void ac3927_set_then_compare() {
    std::println("\n--- #3927: set! of top-level cells is visible to < / = / - ---");
    {
        CompilerService cs;
        CHECK(cs.eval("(define *a* 0)").has_value(), "3927 AC: define *a*");
        CHECK(cs.eval("(define *b* 0)").has_value(), "3927 AC: define *b*");
        CHECK(cs.eval("(define (go x y) (begin (set! *a* x) (set! *b* y)))").has_value(),
              "3927 AC: define go");
        CHECK(cs.eval("(go 3 0)").has_value(), "3927 AC: (go 3 0)");
        auto da = cs.eval("*a*");
        CHECK(da && is_int(*da) && as_int(*da) == 3, "3927 AC: *a* is 3 after go");
        auto db = cs.eval("*b*");
        CHECK(db && is_int(*db) && as_int(*db) == 0, "3927 AC: *b* is 0 after go");
        auto lt = cs.eval("(< *b* *a*)");
        CHECK(lt && is_bool(*lt) && as_bool(*lt), "3927 AC1: (go 3 0) then (< *b* *a*) is #t");
        auto eq = cs.eval("(= *a* *b*)");
        CHECK(eq && is_bool(*eq) && !as_bool(*eq), "3927 AC2: (= *a* *b*) is #f");
        auto sub = cs.eval("(- *a* *b*)");
        CHECK(sub && is_int(*sub) && as_int(*sub) == 3, "3927 AC2: (- *a* *b*) is 3");
    }
    {
        CompilerService cs;
        CHECK(cs.eval("(define *a* 0)").has_value(), "3927 AC3: define *a*");
        CHECK(cs.eval("(define *b* 0)").has_value(), "3927 AC3: define *b*");
        CHECK(cs.eval("(define (go x y) (begin (set! *a* x) (set! *b* y)))").has_value(),
              "3927 AC3: define go");
        CHECK(cs.eval("(define (worse? a b) (< a b))").has_value(), "3927 AC3: define worse?");
        CHECK(cs.eval("(go 3 0)").has_value(), "3927 AC3: (go 3 0)");
        auto w = cs.eval("(worse? *b* *a*)");
        CHECK(w && is_bool(*w) && as_bool(*w), "3927 AC3: (worse? *b* *a*) is #t");
    }
    {
        CompilerService cs;
        CHECK(cs.eval("(define *a* 0)").has_value(), "3927 AC4: define *a*");
        CHECK(cs.eval("(define *b* 0)").has_value(), "3927 AC4: define *b*");
        CHECK(cs.eval("(set! *a* 3)").has_value(), "3927 AC4: top-level set! *a*");
        CHECK(cs.eval("(set! *b* 0)").has_value(), "3927 AC4: top-level set! *b*");
        auto lt = cs.eval("(< *b* *a*)");
        CHECK(lt && is_bool(*lt) && as_bool(*lt), "3927 AC4: direct top-level set! (< *b* *a*) #t");
    }
    {
        CompilerService cs;
        auto r = cs.eval(R"(
            (begin
              (define *a* 0)
              (define *b* 0)
              (define (go x y) (begin (set! *a* x) (set! *b* y)))
              (go 3 0)
              (< *b* *a*))
        )");
        CHECK(r && is_bool(*r) && as_bool(*r), "3927 AC1: whole-program Begin (< *b* *a*) #t");
    }
    {
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define *pre* 0)\\n(define *post* 0)\\n"
                      "(define (live-fit p q) (begin (set! *pre* p) (set! *post* q)))\\n"
                      "(live-fit 3 0)\")")
                  .has_value(),
              "3927 Strand: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "3927 Strand: eval-current");
        auto lt = cs.eval("(< *post* *pre*)");
        CHECK(lt && is_bool(*lt) && as_bool(*lt),
              "3927 Strand: eval-current then (< *post* *pre*) #t");
    }
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("#3927") != std::string::npos, "3927 AC5: lowering cites");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("#3927") != std::string::npos, "3927 AC5: cache_define cites");
    CHECK(read_file("docs/design/3927-set-const-fold.md").empty(), "3927: no docs/design");
    CHECK(read_file("tests/compiler/test_issue_3927.cpp").empty(), "3927: no test_issue_3927");
}

static void ac3918_batch_rebind_env() {
    std::println("\n--- #3918: atomic-batch sole-define rebind refreshes env ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/mutate\" all:)").has_value(), "3918 require");
    CHECK(cs.eval("(set-code \"(define (f x) x)\")").has_value(), "3918 set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3918 seed");
    auto br = cs.eval("(mutate:atomic-batch (list (list \"mutate:rebind\" \"f\" "
                      "\"(lambda (x) (if (< x 0) (* x -1) x))\" \"abs\")) \"abs\")");
    CHECK(br && is_bool(*br) && as_bool(*br), "3918 AC: batch #t");
    auto direct = cs.eval("(f -4)");
    CHECK(direct.has_value() && is_int(*direct) && as_int(*direct) == 4,
          "3918 AC: direct (f -4)→4");
    auto ho = cs.eval("(let ((g f)) (g -4))");
    CHECK(ho.has_value() && is_int(*ho) && as_int(*ho) == 4, "3918 AC: HO (f -4)→4");
    auto f2 = cs.eval("(f 2)");
    CHECK(f2.has_value() && is_int(*f2) && as_int(*f2) == 2, "3918 AC: (f 2)→2");
    auto nested = cs.eval("(begin (f -4))");
    CHECK(nested.has_value() && is_int(*nested) && as_int(*nested) == 4,
          "3918 AC: nested (begin (f -4))→4");
    const auto src = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(src.find("Issue #3918") != std::string::npos, "3918 AC: lockless cites");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("Issue #3918") != std::string::npos, "3918 AC: Path B env SSOT cites");
}

int run_test_eval_current_no_auto_fix() {
    std::println("=== Issue #2484: eval-current no auto-fix ===");
    ac1_closure_unchanged();
    ac2_no_auto_call_arity0();
    ac3_source();
    ac4_gate();
    ac3915_define_rhs_binds();
    ac3915_round_once_rest_args();
    ac3917_extra_paren_cli();
    ac3918_batch_rebind_env();
    ac3927_set_then_compare();
    std::println("\n=== #2484/#3915/#3917/#3918/#3927 results: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_eval_current_no_auto_fix();
}
#endif
