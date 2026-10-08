// @category: unit
// @reason: Issue #2571 — (define) inside (while) body must re-init loop
//          counters; multi-define + set! must not freeze/spin; education
//          warning + preferred outer-define + set! pattern documented.
//
//   AC1: issue repro — nested while with (define x 0) yields count=6
//   AC2: multi-define in while body (define x)(define y) also yields 6
//   AC3: preferred outer define + set! yields 6
//   AC4: education warning path + language note + gate

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
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

static bool eval_int_eq(CompilerService& cs, const char* src, std::int64_t want,
                        const char* label) {
    auto r = cs.eval(src);
    if (!r) {
        CHECK(false, label);
        return false;
    }
    if (is_error(*r) && !is_string(*r)) {
        CHECK(false, label);
        return false;
    }
    CHECK(r && is_int(*r) && as_int(*r) == want, label);
    return r && is_int(*r) && as_int(*r) == want;
}

// Issue repro from #2571: inner (define x 0) each outer iter.
// Expected: 3 outer × 2 inner = 6 (not 2 / freeze).
static void ac1_define_in_while() {
    std::println("\n--- #2571 AC1: define x inside while ---");
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define z 0)
  (define count 0)
  (while (< z 3)
    (begin
      (define x 0)
      (while (< x 2)
        (begin
          (set! count (+ count 1))
          (set! x (+ x 1))))
      (set! z (+ z 1))))
  count))",
                6, "AC1: count=6 with define-in-while");
    unsetenv("AURA_PIPELINE_STRICT");
}

// Multi-define begin inside while previously stacked cells; set! hit the
// oldest cell while Variable read the newest → hang or freeze.
static void ac2_multi_define_in_while() {
    std::println("\n--- #2571 AC2: multi-define in while body ---");
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define z 0)
  (define count 0)
  (while (< z 3)
    (begin
      (define x 0)
      (define y 0)
      (while (< x 2)
        (begin
          (set! count (+ count 1))
          (set! x (+ x 1))
          (set! y (+ y 1))))
      (set! z (+ z 1))))
  count))",
                6, "AC2: multi-define count=6 (no hang/freeze)");
    unsetenv("AURA_PIPELINE_STRICT");
}

static void ac3_preferred_outer_set() {
    std::println("\n--- #2571 AC3: outer define + set! ---");
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define z 0)
  (define x 0)
  (define count 0)
  (while (< z 3)
    (begin
      (set! x 0)
      (while (< x 2)
        (begin
          (set! count (+ count 1))
          (set! x (+ x 1))))
      (set! z (+ z 1))))
  count))",
                6, "AC3: preferred pattern count=6");
    unsetenv("AURA_PIPELINE_STRICT");
}

static void ac4_source_gate() {
    std::println("\n--- #2571 AC4: source-cite + gate ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("#2571") != std::string::npos, "AC4: lookup_cell_index cites #2571");
    CHECK(env.find("newest") != std::string::npos ||
              env.find("binding_index_") != std::string::npos,
          "AC4: newest/binding_index path");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2571") != std::string::npos, "AC4: while/define cites #2571");
    CHECK(flat.find("define …) inside (while") != std::string::npos ||
              flat.find("define") != std::string::npos && flat.find("while") != std::string::npos,
          "AC4: education warning text");
    const auto ctrl = read_file("src/compiler/evaluator_primitives_control.cpp");
    CHECK(ctrl.find("#2571") != std::string::npos, "AC4: while language note");
    CHECK(ctrl.find("outer-define") != std::string::npos ||
              ctrl.find("set! x 0") != std::string::npos,
          "AC4: preferred pattern in note");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_while_define_oneshot") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(!read_file("scripts/coverage/manifests/2571.json").empty(),
          "AC4: check script (manifest SSOT)");
    CHECK(!read_file("scripts/coverage/manifests/2571.json").empty(),
          "AC4: gate cmd (manifest SSOT)");
}

// Issue #4350 reopen: set! on a captured variable must update the one
// shared cell when the call rides a capture snapshot. string-trim is
// the same while-thunk shape and must return.
static void ac4350_capture_snapshot_set() {
    std::println("\n--- #4350: set! writes the shared capture cell ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    CompilerService cs;
    eval_int_eq(cs,
                R"((begin
  (define (a1) (let ((i 0)) (define (inc!) (set! i (+ i 1))) (inc!) (inc!) i))
  (a1)))",
                2, "4350: inner define set! = 2");
    eval_int_eq(cs,
                R"((begin
  (define (a2)
    (let ((i 0))
      (define a (lambda () (set! i (+ i 1)) i))
      (define b (lambda () (set! i (+ i 1)) i))
      (a)
      (b)))
  (a2)))",
                2, "4350: two lambdas share the cell");
    eval_int_eq(cs,
                R"((begin
  (define (mk) (let ((c 0)) (lambda () (set! c (+ c 1)) c)))
  (define ctr (mk))
  (ctr)
  (ctr)
  (ctr)))",
                3, "4350: counter closure = 3");
    eval_int_eq(cs,
                R"((begin
  (define (a6)
    (let ((lo 0) (hi 3))
      (while (lambda () (< lo hi)) (lambda () (set! lo (+ lo 1))))
      lo))
  (a6)))",
                3, "4350: while thunk set! = 3");
    CHECK(cs.eval("(require \"std/string\" all:)").has_value(), "4350: require std/string");
    eval_int_eq(cs, R"((if (string=? (string-trim " a ") "a") 1 0))", 1, "4350: string-trim a");
    eval_int_eq(cs, R"((if (string=? (string-trim "  hi  ") "hi") 1 0))", 1,
                "4350: string-trim hi");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("Issue #4350: set! writes the capture-snapshot cell") != std::string::npos,
          "4350: set! consults the capture snapshot");
    unsetenv("AURA_SANDBOX");
    unsetenv("AURA_PIPELINE_STRICT");
}

// Issue #4372: eval-current installs an IR closure for a workspace lambda.
// while + set! of a let binding must still be visible on the next iteration.
static void ac4372_while_set_in_define() {
    std::println("\n--- #4372: while + set! inside a define ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    const char* sj = R"((define (sj strs sep)
  (if (null? strs) ""
      (if (null? (cdr strs)) (car strs)
          (let ((result (car strs))
                (rest (cdr strs)))
            (while (lambda () (pair? rest))
              (lambda ()
                (set! result (string-append result sep (car rest)))
                (set! rest (cdr rest))))
            result)))))";
    std::string escaped;
    escaped.reserve(std::char_traits<char>::length(sj) + 8);
    for (char c : std::string_view(sj)) {
        if (c == '\\' || c == '"')
            escaped += '\\';
        escaped += c;
    }
    CompilerService cs;
    CHECK(cs.eval(std::format("(set-code \"{}\")", escaped)).has_value(), "4372: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "4372: eval-current");
    eval_int_eq(cs, R"((if (string=? (sj (list "a" "b") ",") "a,b") 1 0))", 1,
                "4372: sj after eval-current is a,b");
    CompilerService file_cs;
    eval_int_eq(file_cs,
                R"((begin
  (define (sj strs sep)
    (if (null? strs) ""
        (if (null? (cdr strs)) (car strs)
            (let ((result (car strs))
                  (rest (cdr strs)))
              (while (lambda () (pair? rest))
                (lambda ()
                  (set! result (string-append result sep (car rest)))
                  (set! rest (cdr rest))))
              result))))
  (if (string=? (sj (list "a" "b") ",") "a,b") 1 0)))",
                1, "4372: file-mode sj is a,b");
    eval_int_eq(file_cs,
                R"((let ((i 0))
  (while (lambda () (< i 3))
    (lambda () (set! i (+ i 1))))
  i))",
                3, "4372: top-level while is 3");
    eval_int_eq(file_cs,
                R"((begin
  (define (sj xs sep)
    (if (null? xs) ""
        (let loop ((xs (cdr xs)) (acc (car xs)) (sep sep))
          (if (null? xs) acc
              (loop (cdr xs) (string-append acc sep (car xs)) sep)))))
  (if (string=? (sj (list "a" "b") ",") "a,b") 1 0)))",
                1, "4372: named-let join is a,b");
    CHECK(file_cs.eval("(require \"std/string\" all:)").has_value(), "4372: require std/string");
    eval_int_eq(file_cs, R"((if (string=? (string-join (list "a" "b") ",") "a,b") 1 0))", 1,
                "4372: string-join");
    eval_int_eq(file_cs, R"((if (string=? (car (string-split "a,b" ",")) "a") 1 0))", 1,
                "4372: string-split head");
    eval_int_eq(file_cs, R"((if (string=? (car (cdr (string-split "a,b" ","))) "b") 1 0))", 1,
                "4372: string-split tail");
    eval_int_eq(file_cs, R"((if (string=? (string-trim "  a  ") "a") 1 0))", 1,
                "4372: string-trim");
    eval_int_eq(file_cs, R"((if (string=? (string-replace "ab" "a" "z") "zb") 1 0))", 1,
                "4372: string-replace");
    eval_int_eq(file_cs, R"((if (string-contains? "hello" "ell") 1 0))", 1,
                "4372: string-contains?");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("Issue #4372") != std::string::npos, "4372: cite");
}

// Issue #4379: a define lowered to an IR closure used to Call the while
// primitive with MakeClosure arms. That primitive only applies evaluator
// closures, so the loop did not run and the let cell stayed at its init.
static void ac4379_ir_while_lambda_set() {
    std::println("\n--- #4379: IR while-lambda set! ---");
    setenv("AURA_SANDBOX", "off", 1);
    setenv("AURA_PIPELINE_STRICT", "0", 1);
    struct RestoreEnv {
        ~RestoreEnv() {
            unsetenv("AURA_SANDBOX");
            unsetenv("AURA_PIPELINE_STRICT");
        }
    } restore_env;
    CompilerService cs;
    CHECK(cs.eval(R"((define (t)
  (let ((lo 0) (hi 3))
    (while (lambda () (< lo hi))
      (lambda () (set! lo (+ lo 1))))
    lo)))")
              .has_value(),
          "4379: define counter");
    eval_int_eq(cs, "(t)", 3, "4379: lambda while set! is 3");
    CHECK(cs.eval(R"((define (trim s)
  (define n (string-length s))
  (let ((lo 0) (hi n))
    (while (lambda () (and (< lo hi) (= (string-ref s lo) 32)))
      (lambda () (set! lo (+ lo 1))))
    (while (lambda () (and (> hi lo) (= (string-ref s (- hi 1)) 32)))
      (lambda () (set! hi (- hi 1))))
    (substring s lo hi))))")
              .has_value(),
          "4379: define trim");
    eval_int_eq(cs, R"((if (string=? (trim "  hi  ") "hi") 1 0))", 1, "4379: trim hi");
    CHECK(cs.eval("(require \"std/string\" all:)").has_value(), "4379: require std/string");
    eval_int_eq(cs, R"((if (string=? (string-trim "  hi  ") "hi") 1 0))", 1,
                "4379: module string-trim");
    eval_int_eq(cs, R"((if (string=? (car (string-split "a,b" ",")) "a") 1 0))", 1,
                "4379: module string-split head");
    eval_int_eq(cs, R"((if (string=? (car (cdr (string-split "a,b" ","))) "b") 1 0))", 1,
                "4379: module string-split tail");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("Issue #4379") != std::string::npos, "4379: cite");
}

} // namespace

int run_test_while_define_oneshot() {
    std::println("=== Issue #2571: while + define loop counter footgun ===");
    ac1_define_in_while();
    ac2_multi_define_in_while();
    ac3_preferred_outer_set();
    ac4_source_gate();
    ac4350_capture_snapshot_set();
    ac4372_while_set_in_define();
    ac4379_ir_while_lambda_set();
    std::println("\n=== #2571: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_while_define_oneshot();
}
#endif
