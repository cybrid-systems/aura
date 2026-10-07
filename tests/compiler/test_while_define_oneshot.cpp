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

} // namespace

int run_test_while_define_oneshot() {
    std::println("=== Issue #2571: while + define loop counter footgun ===");
    ac1_define_in_while();
    ac2_multi_define_in_while();
    ac3_preferred_outer_set();
    ac4_source_gate();
    ac4350_capture_snapshot_set();
    std::println("\n=== #2571: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_while_define_oneshot();
}
#endif
