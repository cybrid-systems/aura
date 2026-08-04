// @category: unit
// @reason: Issue #2578 denseness host residuals (H1/H5/H6).
//
//   AC1: orch:parallel 2-arg typechecks (dotted-rest + namespaced .aura-type)
//   AC2: orch:parallel free-vars survive unimpacted mutate:rebind
//   AC3: workspace rebind body may call orch:parallel (2-arg)
//   AC4: multi-binding let stable across set-code + rebind (#2580)
//   AC5: two fiber:spawn workers keep distinct payloads (#2581 H5)
//   AC6: source-cite + cmake

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
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

static bool eval_ok(CompilerService& cs, std::string_view e) {
    return cs.eval(e).has_value();
}

static bool eval_int_eq(CompilerService& cs, std::string_view e, std::int64_t n) {
    auto r = cs.eval(e);
    return r && is_int(*r) && as_int(*r) == n;
}

static void ac1_orch_parallel_typecheck() {
    std::println("\n--- #2578 AC1: orch:parallel 2-arg typecheck ---");
    CompilerService cs;
    CHECK(eval_ok(cs, "(require \"std/orchestrator\" all:)"), "require orch");
    CHECK(eval_ok(cs, "(set-code \"(define (f x) (orch:parallel (list (lambda (y) y)) x))\")"),
          "set-code 2-arg");
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck-current runs");
    // No hard fail: typecheck-current returns a type report; ensure no "expected 3"
    // by re-eval that would fail mutate:rebind hard-gate if arity still 3 fixed.
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current after 2-arg orch");
    CHECK(eval_ok(cs, "(f 1)"), "call f after typecheck");
}

static void ac2_orch_freevar_survive_rebind() {
    // Issue #2581 / regression after #2579: multi-define with non-Lambda
    // (*agents*) must still snapshot private free-vars (orch-yield-safe)
    // before export filtering — two-pass letrec (lambdas then values).
    std::println("\n--- #2578 AC2: orch free-vars survive unimpacted rebind ---");
    CompilerService cs;
    CHECK(eval_ok(cs, "(require \"std/orchestrator\" all:)"), "require orch");
    CHECK(eval_ok(cs, "(require \"std/mutate\" all:)"), "require mutate");
    auto pre = cs.eval("(orch:parallel (list (lambda (y) (* y 2))) 5)");
    CHECK(pre.has_value() && is_pair(*pre), "pre orch:parallel");
    CHECK(eval_ok(cs, "(set-code \"(define (f x) (+ x 1))\")"), "set-code f");
    CHECK(eval_ok(cs, "(eval-current)"), "eval f");
    CHECK(eval_ok(cs, "(mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"t\")"), "rebind f");
    CHECK(eval_ok(cs, "(eval-current)"), "eval after rebind");
    CHECK(eval_int_eq(cs, "(f 5)", 10), "f after rebind");
    auto post = cs.eval("(orch:parallel (list (lambda (y) (* y 2))) 5)");
    CHECK(post.has_value() && is_pair(*post),
          "AC2: orch:parallel after rebind (no orch-yield-safe unbound)");
    auto step = cs.eval("(orch:step \"nonexistent\" 42)");
    CHECK(step.has_value(), "AC2: orch:step still callable (no orch:lookup-role unbound)");
}

static void ac3_rebind_body_uses_orch() {
    std::println("\n--- #2578 AC3: rebind workspace body to orch:parallel ---");
    CompilerService cs;
    CHECK(eval_ok(cs, "(require \"std/orchestrator\" all:)"), "require orch");
    CHECK(eval_ok(cs, "(require \"std/mutate\" all:)"), "require mutate");
    CHECK(eval_ok(cs, "(set-code \"(define (g x) (+ x 1))\")"), "set-code g");
    CHECK(eval_ok(cs, "(eval-current)"), "eval g");
    CHECK(eval_ok(cs, "(mutate:rebind \"g\" "
                      "\"(lambda (x) (orch:parallel (list (lambda (y) (* y 2))) x))\" "
                      "\"orch\")"),
          "rebind g → orch");
    CHECK(eval_ok(cs, "(eval-current)"), "eval after orch rebind");
    auto r = cs.eval("(g 3)");
    CHECK(r.has_value() && is_pair(*r), "AC3: g returns orch list");
}

static void ac4_multibind_let() {
    std::println("\n--- #2578 AC4: multi-binding let across rebind (#2580) ---");
    CompilerService cs;
    CHECK(eval_ok(cs, "(require \"std/mutate\" all:)"), "require mutate");
    CHECK(eval_ok(cs, "(set-code \"(define (t) (let ((a 1) (b 2) (c 3)) (list a b c)))\")"),
          "set-code multi-let");
    CHECK(eval_ok(cs, "(eval-current)"), "eval t");
    auto r1 = cs.eval("(t)");
    CHECK(r1.has_value() && is_pair(*r1), "multi-let result");
    CHECK(eval_ok(cs, "(mutate:rebind \"t\" "
                      "\"(lambda () (let ((x 10) (y 20) (z 30)) (list x y z)))\" \"m\")"),
          "rebind multi-let");
    CHECK(eval_ok(cs, "(eval-current)"), "eval after multi rebind");
    auto r2 = cs.eval("(t)");
    CHECK(r2.has_value() && is_pair(*r2), "AC4: multi-let after rebind");
    // Spot-check first element is 10 via car
    CHECK(eval_int_eq(cs, "(car (t))", 10), "AC4: first binding 10");
}

static void ac5_fiber_distinct() {
    std::println("\n--- #2578 AC5: fiber:spawn distinct workers (#2581 H5) ---");
    CompilerService cs;
    // Multiple trials; mis-capture would yield identical tags.
    CHECK(eval_ok(cs, R"(
(define fails 0)
(define (trial)
  (let ((id1 (fiber:spawn (lambda () (list "cons-w" "conservative"))))
        (id2 (fiber:spawn (lambda () (list "agg-w" "aggressive")))))
    (list (fiber:join id1) (fiber:join id2))))
(define (loop n)
  (if (= n 0)
      fails
      (begin
        (let ((r (trial)))
          (if (and (equal? (car r) (list "cons-w" "conservative"))
                   (equal? (cadr r) (list "agg-w" "aggressive")))
              #t
              (set! fails (+ fails 1))))
        (loop (- n 1)))))
)"),
          "define fiber trial");
    CHECK(eval_int_eq(cs, "(loop 20)", 0), "AC5: 20/20 fiber pairs distinct");
}

static void ac6_source_gate() {
    std::println("\n--- #2578 AC6: source-cite + cmake ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("#2578") != std::string::npos || env.find("#2581") != std::string::npos,
          "AC6: materialize cites denseness residual");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2578") != std::string::npos || flat.find("#2581") != std::string::npos,
          "AC6: soft-recover cites denseness residual");
    const auto mod = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(mod.find("#2578") != std::string::npos, "AC6: .aura-type parse cites #2578");
    const auto tc = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tc.find("variadic") != std::string::npos && tc.find("#2578") != std::string::npos,
          "AC6: FuncType.variadic cites #2578");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_aether_denseness_residual") != std::string::npos, "AC6: cmake");
}

} // namespace

int run_test_aether_denseness_residual() {
    std::println("=== Issue #2578: Aether denseness host residuals ===");
    ac1_orch_parallel_typecheck();
    ac2_orch_freevar_survive_rebind();
    ac3_rebind_body_uses_orch();
    ac4_multibind_let();
    ac5_fiber_distinct();
    ac6_source_gate();
    std::println("\n=== #2578: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_aether_denseness_residual();
}
#endif
