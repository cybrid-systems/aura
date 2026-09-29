// @category: unit
// @reason: Issue #2576 — JIT PrimCall must forward N args (not only 2).
//
//   AC1: string-append 3 strings → ABC under default JIT
//   AC2: substring 3-arg → "ell"
//   AC3: string-append number->string parts → 1-2
//   AC4: 4-way string-append → ABCD
//   AC5: FORCE_IR / top-level still OK
//   AC6: source + cmake + gate
//
// Issue #4175 — R7RS string comparison completion (string>? / string>=? /
// string<=? were unbound; string=? / string<? present).
//
//   AC7: string>? bound + semantics
//   AC8: string>=? + equal boundary
//   AC9: string<=? boundaries
//   AC10: orch.aura select-best tie-break shape
//   AC11: source + linter wiring
//
// Issue #4177 — R7RS list take/drop completion: native take/drop were bound
// count-first ((take k lst), {Int, Dyn} typed rows), so every R7RS-order call
// ((take lst k)) fell into the count guard and returned an opaque
// primitive-error value (display -> <unknown>, pair? -> #f).
//
//   AC12: (take (list 1 2 3 4) 2) -> proper list (1 2), pair? #t
//   AC13: (drop (list 1 2 3 4) 2) -> proper list (3 4), pair? #t
//   AC14: boundaries — k = length / over-length / k = 0
//   AC15: error arms per repo convention + procedure? stays #t
//   AC16: source + linter wiring

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_error;
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

template <typename F> static std::string with_stdout_capture(F&& fn) {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return {};
    int saved = ::dup(STDOUT_FILENO);
    if (saved < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    ::fflush(stdout);
    if (::dup2(pipefd[1], STDOUT_FILENO) < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ::close(saved);
        return {};
    }
    ::close(pipefd[1]);
    fn();
    ::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);
    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    return out;
}

static std::string trim_nl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    auto pos = s.rfind('\n');
    if (pos != std::string::npos && s.substr(pos + 1) == "#t")
        s = s.substr(0, pos);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static void ac1_append3() {
    std::println("\n--- #2576 AC1: string-append 3 ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (string-append "A" "B" "C"))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "ABC", "AC1: ABC not AB0");
    if (trim_nl(out) != "ABC")
        std::println(stderr, "  got '{}'", out);
}

static void ac2_substring() {
    std::println("\n--- #2576 AC2: substring 3-arg ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (write (substring "hello" 1 4))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "\"ell\"", "AC2: \"ell\"");
    if (trim_nl(out) != "\"ell\"")
        std::println(stderr, "  got '{}'", out);
}

static void ac3_nested_num() {
    std::println("\n--- #2576 AC3: append number->string parts ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (string-append (number->string 1) "-" (number->string 2)))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "1-2", "AC3: 1-2 not 1-0");
    if (trim_nl(out) != "1-2")
        std::println(stderr, "  got '{}'", out);
}

static void ac4_append4() {
    std::println("\n--- #2576 AC4: string-append 4 ---");
    CompilerService cs;
    auto out = with_stdout_capture([&] {
        (void)cs.eval(R"((begin
  (define (f)
    (display (string-append "A" "B" "C" "D"))
    (newline)
    #t)
  (f)))");
    });
    CHECK(trim_nl(out) == "ABCD", "AC4: ABCD");
    if (trim_nl(out) != "ABCD")
        std::println(stderr, "  got '{}'", out);
}

static void ac5_controls() {
    std::println("\n--- #2576 AC5: FORCE_IR + top-level ---");
    setenv("AURA_FORCE_IR", "1", 1);
    CompilerService cs_ir;
    auto ir = with_stdout_capture([&] {
        (void)cs_ir.eval(R"((begin
  (define (f) (display (string-append "A" "B" "C")) (newline) #t)
  (f)))");
    });
    unsetenv("AURA_FORCE_IR");
    CHECK(trim_nl(ir).find("ABC") != std::string::npos, "AC5: FORCE_IR ABC");

    CompilerService cs_top;
    auto top = with_stdout_capture(
        [&] { (void)cs_top.eval(R"((begin (display (string-append "A" "B" "C")) (newline)))"); });
    CHECK(trim_nl(top).find("ABC") != std::string::npos, "AC5: top-level ABC");
}

static void ac6_source_gate() {
    std::println("\n--- #2576 AC6: source + gate ---");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(jit.find("#2576") != std::string::npos, "AC6: jit cites #2576");
    CHECK(jit.find("kMax") != std::string::npos || jit.find("pack") != std::string::npos,
          "AC6: pack N args");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("#2576") != std::string::npos, "AC6: runtime cites #2576");
    CHECK(rt.find("int64_t* args") != std::string::npos ||
              rt.find("int64_t *args") != std::string::npos,
          "AC6: pointer ABI");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_primcall_narg") != std::string::npos, "AC6: cmake");
    const auto build = read_file("build.py");
    CHECK(!read_file("scripts/coverage/manifests/2576.json").empty(), "AC6: check (manifest SSOT)");
    CHECK(!read_file("scripts/coverage/manifests/2576.json").empty(),
          "AC6: gate cmd (manifest SSOT)");
}

// Issue #4175: R7RS string comparison completion — string>? / string>=? /
// string<=? were unbound (string=? / string<? present), breaking aura-build
// kernel modules using string>? tie-breaks (aura/orch.aura select-best).

static void ac7_string_gt() {
    std::println("\n--- #4175 AC7: string>? bound + semantics ---");
    CompilerService cs;
    auto gt = with_stdout_capture(
        [&] { (void)cs.eval(R"((begin (display (if (string>? "b" "a") 1 0)) (newline)))"); });
    CHECK(trim_nl(gt) == "1", "AC7: (string>? \"b\" \"a\") -> #t");
    CompilerService cs2;
    auto gf = with_stdout_capture(
        [&] { (void)cs2.eval(R"((begin (display (if (string>? "a" "b") 1 0)) (newline)))"); });
    CHECK(trim_nl(gf) == "0", "AC7: (string>? \"a\" \"b\") -> #f");
}

static void ac8_string_ge() {
    std::println("\n--- #4175 AC8: string>=? + equal boundary ---");
    CompilerService cs;
    auto eq = with_stdout_capture(
        [&] { (void)cs.eval(R"((begin (display (if (string>=? "a" "a") 1 0)) (newline)))"); });
    CHECK(trim_nl(eq) == "1", "AC8: (string>=? \"a\" \"a\") -> #t");
    CompilerService cs2;
    auto gf = with_stdout_capture(
        [&] { (void)cs2.eval(R"((begin (display (if (string>=? "a" "b") 1 0)) (newline)))"); });
    CHECK(trim_nl(gf) == "0", "AC8: (string>=? \"a\" \"b\") -> #f");
}

static void ac9_string_le() {
    std::println("\n--- #4175 AC9: string<=? boundaries ---");
    CompilerService cs;
    auto lt = with_stdout_capture(
        [&] { (void)cs.eval(R"((begin (display (if (string<=? "a" "b") 1 0)) (newline)))"); });
    CHECK(trim_nl(lt) == "1", "AC9: (string<=? \"a\" \"b\") -> #t");
    CompilerService cs2;
    auto eq = with_stdout_capture(
        [&] { (void)cs2.eval(R"((begin (display (if (string<=? "a" "a") 1 0)) (newline)))"); });
    CHECK(trim_nl(eq) == "1", "AC9: (string<=? \"a\" \"a\") -> #t");
    CompilerService cs3;
    auto gf = with_stdout_capture(
        [&] { (void)cs3.eval(R"((begin (display (if (string<=? "b" "a") 1 0)) (newline)))"); });
    CHECK(trim_nl(gf) == "0", "AC9: (string<=? \"b\" \"a\") -> #f");
}

static void ac10_orch_tiebreak() {
    std::println("\n--- #4175 AC10: orch.aura select-best tie-break shape ---");
    // aura/orch.aura:26 — (if (or (> wf bf) (and (= wf bf) (string>? wid bid))) ...)
    CompilerService cs;
    auto short_circuit = with_stdout_capture([&] {
        (void)cs.eval(R"((begin (display (if (or (> 1 0) (string>? "b" "a")) 1 0)) (newline)))");
    });
    CHECK(trim_nl(short_circuit) == "1", "AC10: or arm1 true -> 1");
    CompilerService cs2;
    auto decided_false = with_stdout_capture([&] {
        (void)cs2.eval(R"((begin (display (if (or (> 0 1) (string>? "a" "b")) 1 0)) (newline)))");
    });
    CHECK(trim_nl(decided_false) == "0", "AC10: string>? arm decides #f -> 0");
    CompilerService cs3;
    auto decided_true = with_stdout_capture([&] {
        (void)cs3.eval(R"((begin (display (if (or (> 0 1) (string>? "b" "a")) 1 0)) (newline)))");
    });
    CHECK(trim_nl(decided_true) == "1", "AC10: string>? arm decides #t -> 1");
}

static void ac11_source_gate() {
    std::println("\n--- #4175 AC11: source + linter wiring ---");
    const auto ir = read_file("src/compiler/ir.ixx");
    CHECK(ir.find("StringGt") != std::string::npos, "AC11: ir.ixx StringGt");
    CHECK(ir.find("StringGe") != std::string::npos, "AC11: ir.ixx StringGe");
    CHECK(ir.find("StringLe") != std::string::npos, "AC11: ir.ixx StringLe");
    CHECK(ir.find("\"string>?\"") != std::string::npos, "AC11: kPrimNames string>?");
    CHECK(ir.find("std::size(kPrimNames) == 47") != std::string::npos, "AC11: kPrimNames 47");
    const auto jit = read_file("src/compiler/aura_jit.cpp");
    CHECK(jit.find("PrimStringGt = 44") != std::string::npos, "AC11: jit PrimStringGt 44");
    CHECK(jit.find("PrimStringGe = 45") != std::string::npos, "AC11: jit PrimStringGe 45");
    CHECK(jit.find("PrimStringLe = 46") != std::string::npos, "AC11: jit PrimStringLe 46");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("\"string>=?\"") != std::string::npos, "AC11: kPrimNameTable string>=?");
    const auto lowering = read_file("src/compiler/lowering_impl.cpp");
    CHECK(lowering.find("{\"string<=?\", PrimId::StringLe}") != std::string::npos,
          "AC11: lowering string<=? -> StringLe");
    const auto tc = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tc.find("register_primitive(\"string>?\", {String, String}, Bool)") != std::string::npos,
          "AC11: typechecker string>?");
    const auto evp = read_file("src/compiler/evaluator_primitives_pair.cpp");
    CHECK(evp.find("\"string>?\"") != std::string::npos, "AC11: evaluator registers string>?");
    CHECK(evp.find("#4175") != std::string::npos, "AC11: evaluator cites #4175");
    const auto build = read_file("build.py");
    CHECK(build.find("check_r7rs_string_compare_4175") != std::string::npos,
          "AC11: build.py linter");
}

// Issue #4177: R7RS list take/drop — see the header note above. take/drop are
// evaluator prims (no JIT layer), so the gate is the flipped arg order + the
// proper-list face through the standard pair-construction path.

static void ac12_take_r7rs() {
    std::println("\n--- #4177 AC12: R7RS (take lst k) -> proper list ---");
    CompilerService cs;
    auto disp = with_stdout_capture(
        [&] { (void)cs.eval(R"((begin (display (take (list 1 2 3 4) 2)) (newline)))"); });
    CHECK(trim_nl(disp) == "(1 2)", "AC12: (display (take (list 1 2 3 4) 2)) -> (1 2)");
    CompilerService cs2;
    auto pr = with_stdout_capture([&] {
        (void)cs2.eval(R"((begin (display (if (pair? (take (list 1 2 3 4) 2)) 1 0)) (newline)))");
    });
    CHECK(trim_nl(pr) == "1", "AC12: (pair? (take (list 1 2 3 4) 2)) -> #t");
}

static void ac13_drop_r7rs() {
    std::println("\n--- #4177 AC13: R7RS (drop lst k) -> proper list ---");
    CompilerService cs;
    auto disp = with_stdout_capture(
        [&] { (void)cs.eval(R"((begin (display (drop (list 1 2 3 4) 2)) (newline)))"); });
    CHECK(trim_nl(disp) == "(3 4)", "AC13: (display (drop (list 1 2 3 4) 2)) -> (3 4)");
    CompilerService cs2;
    auto pr = with_stdout_capture([&] {
        (void)cs2.eval(R"((begin (display (if (pair? (drop (list 1 2 3 4) 2)) 1 0)) (newline)))");
    });
    CHECK(trim_nl(pr) == "1", "AC13: (pair? (drop (list 1 2 3 4) 2)) -> #t");
}

static void ac14_take_drop_boundaries() {
    std::println("\n--- #4177 AC14: take/drop boundaries ---");
    // k == length
    CompilerService cs;
    auto take_all = with_stdout_capture(
        [&] { (void)cs.eval(R"((begin (display (take (list 1 2 3) 3)) (newline)))"); });
    CHECK(trim_nl(take_all) == "(1 2 3)", "AC14: (take (list 1 2 3) 3) -> (1 2 3)");
    CompilerService cs2;
    auto drop_all = with_stdout_capture([&] {
        (void)cs2.eval(R"((begin (display (if (null? (drop (list 1 2 3) 3)) 1 0)) (newline)))");
    });
    CHECK(trim_nl(drop_all) == "1", "AC14: (drop (list 1 2 3) 3) -> ()");
    // over-length: take clamps to the full list, drop past end -> empty
    CompilerService cs3;
    auto take_over = with_stdout_capture(
        [&] { (void)cs3.eval(R"((begin (display (take (list 1 2 3) 9)) (newline)))"); });
    CHECK(trim_nl(take_over) == "(1 2 3)", "AC14: (take (list 1 2 3) 9) clamps -> (1 2 3)");
    CompilerService cs4;
    auto drop_over = with_stdout_capture([&] {
        (void)cs4.eval(R"((begin (display (if (null? (drop (list 1 2 3) 9)) 1 0)) (newline)))");
    });
    CHECK(trim_nl(drop_over) == "1", "AC14: (drop (list 1 2 3) 9) past end -> ()");
    // k = 0
    CompilerService cs5;
    auto take_zero = with_stdout_capture([&] {
        (void)cs5.eval(R"((begin (display (if (null? (take (list 1 2 3) 0)) 1 0)) (newline)))");
    });
    CHECK(trim_nl(take_zero) == "1", "AC14: (take (list 1 2 3) 0) -> ()");
    CompilerService cs6;
    auto drop_zero = with_stdout_capture(
        [&] { (void)cs6.eval(R"((begin (display (drop (list 1 2 3) 0)) (newline)))"); });
    CHECK(trim_nl(drop_zero) == "(1 2 3)", "AC14: (drop (list 1 2 3) 0) -> (1 2 3)");
}

static void ac15_take_drop_errors() {
    std::println("\n--- #4177 AC15: error arms + procedure? face ---");
    // Runtime-reachable guard arms: not-a-list on the Dyn slot and negative
    // count on the Int slot. A non-int count literal is intentionally NOT
    // probed here — a wrong-typed value on the declared-Int slot is rejected
    // by the static type checker / slot coercion before prim dispatch (the
    // repo convention for typed params; AC16 pins the {Dyn, Int} rows), so
    // the prim's own count guard is defense-in-depth for direct dispatch.
    CompilerService cs;
    auto take_bad_lst = with_stdout_capture(
        [&] { (void)cs.eval(R"((begin (display (if (error? (take "x" 2)) 1 0)) (newline)))"); });
    CHECK(trim_nl(take_bad_lst) == "1", "AC15: (take \"x\" 2) -> error");
    CompilerService cs2;
    auto take_neg = with_stdout_capture([&] {
        (void)cs2.eval(R"((begin (display (if (error? (take (list 1 2) -1)) 1 0)) (newline)))");
    });
    CHECK(trim_nl(take_neg) == "1", "AC15: (take (list 1 2) -1) -> error");
    CompilerService cs3;
    auto drop_bad_lst = with_stdout_capture(
        [&] { (void)cs3.eval(R"((begin (display (if (error? (drop 42 1)) 1 0)) (newline)))"); });
    CHECK(trim_nl(drop_bad_lst) == "1", "AC15: (drop 42 1) -> error");
    CompilerService cs4;
    auto drop_neg = with_stdout_capture([&] {
        (void)cs4.eval(R"((begin (display (if (error? (drop (list 1 2) -1)) 1 0)) (newline)))");
    });
    CHECK(trim_nl(drop_neg) == "1", "AC15: (drop (list 1 2) -1) -> error");
    // `and` is tree-walker-only (strict pipeline fallback-forbidden), so the
    // conjunctive face is expressed as nested ifs.
    CompilerService cs5;
    auto proc = with_stdout_capture([&] {
        (void)cs5.eval(
            R"((begin (display (if (procedure? take) (if (procedure? drop) 1 0) 0)) (newline)))");
    });
    CHECK(trim_nl(proc) == "1", "AC15: take/drop stay procedures");
}

static void ac16_source_gate() {
    std::println("\n--- #4177 AC16: source + linter wiring ---");
    const auto evp = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(evp.find("Issue #4177") != std::string::npos, "AC16: evaluator cites #4177");
    CHECK(evp.find("\"take: count must be a non-negative integer\"") != std::string::npos,
          "AC16: take count guard message");
    CHECK(evp.find("\"take: not a list\"") != std::string::npos, "AC16: take not-a-list guard");
    CHECK(evp.find("\"drop: count must be a non-negative integer\"") != std::string::npos,
          "AC16: drop count guard message");
    const auto tc = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tc.find("register_primitive(\"take\", {Dyn, Int}, Dyn)") != std::string::npos,
          "AC16: typechecker take (Dyn, Int)");
    CHECK(tc.find("register_primitive(\"drop\", {Dyn, Int}, Dyn)") != std::string::npos,
          "AC16: typechecker drop (Dyn, Int)");
    CHECK(tc.find("register_primitive(\"take\", {Int, Dyn}, Dyn)") == std::string::npos,
          "AC16: old count-first take registration gone");
    const auto mfn = read_file("tests/suite/multiframe_named_let_2873.aura");
    CHECK(mfn.find("(take (list 9 8 7 6) 2)") != std::string::npos,
          "AC16: 2873 fixture flipped to R7RS order");
    const auto qp = read_file("tests/suite/query_primitives_split_2914.aura");
    CHECK(qp.find("(take (list 1) \"x\")") != std::string::npos,
          "AC16: 2914 bad-count arm in R7RS order");
    CHECK(qp.find("(drop (list 1) 9)") != std::string::npos,
          "AC16: 2914 past-end arm in R7RS order");
    const auto build = read_file("build.py");
    CHECK(build.find("check_list_take_drop_4177") != std::string::npos, "AC16: build.py linter");
}

} // namespace

int run_test_primcall_narg() {
    std::println("=== Issue #2576: PrimCall N-arg ===");
    ac1_append3();
    ac2_substring();
    ac3_nested_num();
    ac4_append4();
    ac5_controls();
    ac6_source_gate();
    ac7_string_gt();
    ac8_string_ge();
    ac9_string_le();
    ac10_orch_tiebreak();
    ac11_source_gate();
    ac12_take_r7rs();
    ac13_drop_r7rs();
    ac14_take_drop_boundaries();
    ac15_take_drop_errors();
    ac16_source_gate();
    std::println("\n=== #2576+#4175+#4177: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_primcall_narg();
}
#endif
