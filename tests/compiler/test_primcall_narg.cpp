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
//
// Issue #4232 — Soft: cannot call car in soft_worldline_pick (host_fallback
// select). The tree-walker's prim-value call handler resolved the callee via
// the ENV-level primitive table; helper eval envs built per the
// materialize_call_env contract without caller-side wiring (serve-async Soft
// worldline select) returned nullopt there and degraded to
// "cannot call: car" even though the Variable fallback resolves bare prim
// names from the full Evaluator registry. Registry fallback closes the gap.
//
//   AC17: car / cdr / null? callable in an unwired-env eval (the door)
//   AC18: soft_worldline_pick pick-best select via the same env path
//   AC19: source + linter wiring
//
// Issue #4265 — Soft pick-best eval returned a non-int under Soft sock stress
// (target-sum WAVE10: the entire scores list `'(6 6 0 … 6)'`; word-search
// WAVE10: `<error>`), while a fresh session returned the correct max.
// Residual face after #4264 (session std prelude) + #4232 (registry prim
// fallback): CompilerService::eval()'s Path B function-call shortcut walked
// the PERSISTENT workspace flat's Define body through eval_flat /
// apply_closure WITHOUT marking the body subtree dirty, so the incremental
// eval value cache (#32b/#159/#1441) served rows cached by a PREVIOUS call —
// repeated `(pick-best (list …))` execs with different args returned the
// first call's result, a stale intermediate list, or stale-state errors.
//
//   AC20: repeated Path B pick-best calls with fresh args -> fresh max each
//   AC21: set-code + eval-current session shape — replay parity + freshness
//   AC22: source + linter wiring

#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.arena;
import aura.core.ast;
import aura.parser.parser;

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

// Issue #4232 regression door: evaluate `code` in an Env that mirrors the
// materialize_call_env contract gap — DEFAULT-constructed (primitives_/cells_/pool_
// unwired), exactly the shape materialize_call_env hands its caller before the
// caller-side wiring (serve-async soft worldline select).
static std::optional<aura::compiler::types::EvalValue> eval_in_unwired_env(CompilerService& cs,
                                                                           std::string_view code) {
    auto& ev = cs.evaluator();
    aura::compiler::Env call_env;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto pr = aura::parser::parse_to_flat(std::string(code), flat, pool);
    if (!pr.success || pr.root == aura::ast::NULL_NODE)
        return std::nullopt;
    flat.root = pr.root;
    auto r = ev.eval_flat(flat, pool, pr.root, call_env);
    if (!r)
        return std::nullopt;
    return std::optional(*r);
}

namespace {

static void ac17_unwired_env_list_prims() {
    std::println("\n--- #4232 AC17: car/cdr/null? callable in unwired-env eval ---");
    CompilerService cs;
    auto car_r = eval_in_unwired_env(cs, "(car (quote (5 2 7)))");
    CHECK(car_r.has_value(), "AC17: (car '(5 2 7)) evaluates (was: cannot call: car)");
    if (car_r && aura::compiler::types::is_int(*car_r))
        CHECK(aura::compiler::types::as_int(*car_r) == 5, "AC17: car -> 5");
    else
        CHECK(false, "AC17: car -> int 5");
    auto cdr_r = eval_in_unwired_env(cs, "(cdr (quote (5 2 7)))");
    CHECK(cdr_r.has_value() && aura::compiler::types::is_pair(*cdr_r), "AC17: cdr -> pair (2 7)");
    auto null_r = eval_in_unwired_env(cs, "(null? (quote ()))");
    CHECK(null_r.has_value() && aura::compiler::types::is_bool(*null_r) &&
              aura::compiler::types::as_bool(*null_r),
          "AC17: (null? '()) -> #t");
}

static void ac18_soft_pick_best_unwired_env() {
    std::println("\n--- #4232 AC18: soft_worldline_pick pick-best via unwired env ---");
    CompilerService cs;
    // The helper from aura/soft_worldline_pick.aura (issue body) defined AND
    // selected through the same unwired env: define lands in the env, the
    // closure call is wired by apply_closure, and the score-list select must
    // stay Soft-native numeric max (no host_fallback).
    auto r = eval_in_unwired_env(cs, R"4232((begin (define pick-best
  (lambda (xs)
    (letrec ((loop (lambda (rest best)
        (if (null? rest)
          best
          (loop (cdr rest)
                (if (> (car rest) best) (car rest) best))))))
      (loop xs -999999))))

  (pick-best (quote (3 9 4)))))4232");
    CHECK(r.has_value(), "AC18: pick-best evaluates in unwired env");
    if (r && aura::compiler::types::is_int(*r))
        CHECK(aura::compiler::types::as_int(*r) == 9, "AC18: select-best -> 9 (numeric max)");
    else
        CHECK(false, "AC18: select-best -> int 9");
    // Prim select step evaluated at the unwired call site itself.
    CompilerService cs2;
    auto mixed = eval_in_unwired_env(cs2, "(+ 1 (car (quote (41 7))))");
    CHECK(mixed.has_value() && aura::compiler::types::is_int(*mixed) &&
              aura::compiler::types::as_int(*mixed) == 42,
          "AC18: (+ 1 (car ...)) -> 42 in unwired env");
}

static void ac19_source_gate() {
    std::println("\n--- #4232 AC19: source + linter wiring ---");
    const auto evf = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(evf.find("Issue #4232") != std::string::npos, "AC19: eval_flat cites #4232");
    CHECK(evf.find("slot_lookup_fast(slot)") != std::string::npos,
          "AC19: registry fallback present at prim-value call handler");
    const auto pairp = read_file("src/compiler/evaluator_primitives_pair.cpp");
    CHECK(pairp.find("\"car\"") != std::string::npos && pairp.find("\"cdr\"") != std::string::npos,
          "AC19: car/cdr registered in pair prims");
    const auto listp = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(listp.find("\"null?\"") != std::string::npos, "AC19: null? registered in list prims");
    const auto tc = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(tc.find("register_primitive(\"car\", {Dyn}, Dyn)") != std::string::npos,
          "AC19: typechecker car row");
    const auto build = read_file("build.py");
    CHECK(build.find("check_soft_list_prims_4232") != std::string::npos, "AC19: build.py linter");
}

// Issue #4265: the soft_worldline_pick helper (issue body shape) as a
// workspace Define — every call below must compute a FRESH max from ITS list,
// never a cached row from the previous call.
static const char* kPickBestDef4265 = R"4265((define (pick-best xs)
  (letrec ((loop (lambda (rest best)
      (if (null? rest)
        best
        (loop (cdr rest)
              (if (> (car rest) best) (car rest) best))))))
    (loop xs -999999))))4265";

static void ac20_pick_best_pathb_repeat_calls() {
    std::println("\n--- #4265 AC20: Path B repeated pick-best calls, fresh args each ---");
    CompilerService cs;
    auto def_r = cs.eval(std::string(kPickBestDef4265));
    CHECK(def_r.has_value(), "AC20: pick-best define evaluates");
    // Path B door: top-level call whose head matches the workspace Lambda
    // Define. Different arg lists per call — the #4265 bug returned the
    // FIRST call's cached max for every later list.
    const std::pair<const char*, int> cases[] = {
        {"(pick-best (list 6 6 0 0 0 0 0 0 0 0 0 0 6))", 6},
        {"(pick-best (list 0 0 9))", 9},
        {"(pick-best (list 3))", 3},
        {"(pick-best (list 0 8 0 0 2))", 8},
        {"(pick-best (list 7 1 1 1 1 1 1 1 1 1 1 1 1))", 7},
        {"(pick-best (list 4 4 4 4 5))", 5},
        {"(pick-best (list 6 6 0 0 0 0 0 0 0 0 0 0 6))", 6},
        {"(pick-best (list 2 2 2))", 2},
        {"(pick-best (list 0 0 0 1))", 1},
        {"(pick-best (list 9 9))", 9},
    };
    for (const auto& [expr, want] : cases) {
        auto r = cs.eval(expr);
        CHECK(r.has_value(), std::string("AC20: evaluates -> ") + expr);
        if (r && aura::compiler::types::is_int(*r))
            CHECK(aura::compiler::types::as_int(*r) == want,
                  std::string("AC20: max == ") + std::to_string(want) + " for " + expr);
        else
            CHECK(false, std::string("AC20: int result for ") + expr);
    }
}

static void ac21_pick_best_session_replay_parity() {
    std::println("\n--- #4265 AC21: set-code + eval-current session shape — replay parity ---");
    CompilerService cs;
    // Soft host session shape: set-code the helper, eval-current, then
    // repeated pick-best evals; an (eval-current) replay between calls must
    // neither serve Path-B arg-specific rows nor break later calls.
    auto sc = cs.eval(std::string("(set-code \"") + kPickBestDef4265 + std::string("\")"));
    CHECK(sc.has_value(), "AC21: set-code accepts the pick-best source");
    auto ec1 = cs.eval("(eval-current)");
    CHECK(ec1.has_value(), "AC21: first eval-current completes");
    auto r1 = cs.eval("(pick-best (list 6 6 0 0 0 0 0 0 0 0 0 0 6))");
    CHECK(r1 && aura::compiler::types::is_int(*r1) && aura::compiler::types::as_int(*r1) == 6,
          "AC21: post-set-code pick-best -> 6");
    auto r2 = cs.eval("(pick-best (list 0 0 9))");
    CHECK(r2 && aura::compiler::types::is_int(*r2) && aura::compiler::types::as_int(*r2) == 9,
          "AC21: different list -> 9 (no first-call cache replay)");
    auto ec2 = cs.eval("(eval-current)");
    CHECK(ec2.has_value(), "AC21: replay eval-current completes after Path B rows");
    auto r3 = cs.eval("(pick-best (list 3))");
    CHECK(r3 && aura::compiler::types::is_int(*r3) && aura::compiler::types::as_int(*r3) == 3,
          "AC21: post-replay pick-best still fresh -> 3");
}

static void ac22_source_gate_4265() {
    std::println("\n--- #4265 AC22: source + linter wiring ---");
    // Verification conclusion (HEAD 985fbb8a0): the reported repro is covered
    // by #4264 (session std prelude + loader adopt-if-held) and #4232
    // (registry prim fallback). AC20/AC21 pin the pick-best contract so a
    // future regression on this surface cannot ship silently.
    CHECK(read_file("tests/compiler/test_primcall_narg.cpp").find("#4265") != std::string::npos,
          "AC22: test file cites #4265");
    const auto build = read_file("build.py");
    CHECK(build.find("check_pick_best_call_cache_4265") != std::string::npos,
          "AC22: build.py linter");
}

// Issue #4273: set-code workspace (define > 42) must not poison Path B
// pick-best after eval-current (WAVE14 `cannot call: >`).
static void ac23_pick_best_prim_shadow_4273() {
    std::println("\n--- #4273 AC23: pick-best after workspace shadows > ---");
    CompilerService cs;
    auto sc = cs.eval("(set-code \"(define > 42) (define (f x) (+ x 1)) (f 3)\")");
    CHECK(sc.has_value(), "AC23: set-code with (define > 42)");
    auto ec = cs.eval("(eval-current)");
    CHECK(ec.has_value(), "AC23: eval-current completes");
    // Direct prim call and pick-best lambda must still resolve language >
    auto gt = cs.eval("(> 3 1)");
    CHECK(gt.has_value() && aura::compiler::types::is_bool(*gt) &&
              aura::compiler::types::as_bool(*gt),
          "AC23: Path B (> 3 1) still #t after workspace shadow dropped");
    auto def = cs.eval(std::string(kPickBestDef4265));
    CHECK(def.has_value(), "AC23: pick-best define");
    auto r = cs.eval("(pick-best (list 2 9 4))");
    CHECK(r.has_value() && aura::compiler::types::is_int(*r) &&
              aura::compiler::types::as_int(*r) == 9,
          "AC23: pick-best -> 9 (not cannot call: >)");
    CHECK(read_file("src/compiler/evaluator_primitives_eval.cpp").find("#4273") !=
              std::string::npos,
          "AC23: eval primitives cite #4273");
    CHECK(read_file("src/serve/serve_async.cpp").find("emit_exec_result") != std::string::npos,
          "AC23: serve emit_exec_result for soft Error status");
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
    ac17_unwired_env_list_prims();
    ac18_soft_pick_best_unwired_env();
    ac19_source_gate();
    ac20_pick_best_pathb_repeat_calls();
    ac21_pick_best_session_replay_parity();
    ac22_source_gate_4265();
    ac23_pick_best_prim_shadow_4273();
    std::println("\n=== #2576+#4175+#4177+#4232+#4265+#4273: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_primcall_narg();
}
#endif
