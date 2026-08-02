// @category: unit
// @reason: Issue #2568 — symbol eq?/equal? for quoted symbols (agent decision tags).
//
//   AC1: (eq? 'commit 'commit) → #t  (interned short-str cache)
//   AC2: (let ((d 'skip)) (eq? d 'skip)) → #t
//   AC3: (define d 'commit) then (eq? d 'commit) / (equal? d 'commit) → #t
//        (CLI multi-form path: Quote value-define must not IR-bind as 0)
//   AC4: equal? content for same-name strings/symbols; string? on defined quote
//   AC5: source-cite + test + cmake + gate

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
using aura::compiler::types::is_bool;
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

static bool eval_bool(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    return r && is_bool(*r) && as_bool(*r);
}

// ── AC1: literal quote identity ──
static void ac1_literal_eq() {
    std::println("\n--- #2568 AC1: (eq? 'commit 'commit) ---");
    CompilerService cs;
    CHECK(eval_bool(cs, "(eq? 'commit 'commit)"), "AC1: eq? same quote literals");
    CHECK(eval_bool(cs, "(eq? 'a 'a)"), "AC1: eq? short symbol 'a");
    CHECK(eval_bool(cs, "(equal? 'commit 'commit)"), "AC1: equal? same quote literals");
    CHECK(!eval_bool(cs, "(eq? 'commit 'skip)"), "AC1: different names → eq? #f");
}

// ── AC2: let binding ──
static void ac2_let_eq() {
    std::println("\n--- #2568 AC2: (let ((d 'skip)) (eq? d 'skip)) ---");
    CompilerService cs;
    CHECK(eval_bool(cs, "(let ((d 'skip)) (eq? d 'skip))"), "AC2: let-bound quote eq?");
    CHECK(eval_bool(cs, "(let ((d 'skip)) (equal? d 'skip))"), "AC2: let-bound equal?");
}

// ── AC3: define of quote (CLI multi-form / value-define path) ──
static void ac3_define_quote() {
    std::println("\n--- #2568 AC3: (define d 'commit) then eq?/equal? ---");
    CompilerService cs;
    // Sequential evals mirror pipe-mode multi-form CLI (each form separate eval).
    auto def = cs.eval("(define d 'commit)");
    CHECK(def.has_value(), "AC3: define succeeds");
    CHECK(eval_bool(cs, "(string? d)"), "AC3: d is string (symbol), not fixnum 0");
    CHECK(eval_bool(cs, "(eq? d 'commit)"), "AC3: eq? defined quote vs literal");
    CHECK(eval_bool(cs, "(equal? d 'commit)"), "AC3: equal? defined quote vs literal");
    // Redefine to another decision tag
    CHECK(cs.eval("(define d 'skip)").has_value(), "AC3: redefine to 'skip");
    CHECK(eval_bool(cs, "(eq? d 'skip)"), "AC3: eq? after redefine");
    CHECK(!eval_bool(cs, "(eq? d 'commit)"), "AC3: not equal to old tag");
}

// ── AC4: equal? content + agent-style tags ──
static void ac4_equal_and_tags() {
    std::println("\n--- #2568 AC4: equal? content + decision tags ---");
    CompilerService cs;
    CHECK(eval_bool(cs, "(equal? \"commit\" \"commit\")"), "AC4: string equal?");
    CHECK(cs.eval("(define decision 'rollback)").has_value(), "AC4: define decision");
    CHECK(eval_bool(cs, "(or (eq? decision 'commit) (eq? decision 'skip) "
                        "(eq? decision 'rollback))"),
          "AC4: agent decision branch on symbol tags");
    // Quote via (quote …) sugar-equivalent
    CHECK(cs.eval("(define e (quote commit))").has_value(), "AC4: define (quote commit)");
    CHECK(eval_bool(cs, "(eq? e 'commit)"), "AC4: (quote commit) same as 'commit");
}

// ── AC5: source + gate ──
static void ac5_source_gate() {
    std::println("\n--- #2568 AC5: source-cite + gate ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2568") != std::string::npos, "AC5: eval_flat cites #2568");
    CHECK(flat.find("short_str_cache_") != std::string::npos, "AC5: short_str_cache intern");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("#2568") != std::string::npos, "AC5: service Quote-before-IR");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("#2568") != std::string::npos, "AC5: lowering Quote Variable→ConstString");
    const auto builtins = read_file("src/compiler/evaluator_primitives_builtins.cpp");
    CHECK(builtins.find("#2568") != std::string::npos, "AC5: eq? string path");
    const auto runtime = read_file("src/compiler/evaluator_primitives_runtime.cpp");
    CHECK(runtime.find("#2568") != std::string::npos, "AC5: equal? string path");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_symbol_eq_2568") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_symbol_eq_2568") != std::string::npos, "AC5: check script");
    CHECK(build.find("cmd_symbol_eq_coverage") != std::string::npos, "AC5: gate cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2568: symbol eq?/equal? for quoted symbols ===");
    ac1_literal_eq();
    ac2_let_eq();
    ac3_define_quote();
    ac4_equal_and_tags();
    ac5_source_gate();
    std::println("\n=== #2568: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
