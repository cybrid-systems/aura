// @category: unit
// @reason: Issue #4607 — --jit did not report an unbound variable: a call
//          to an unbound global was compiled as the constant 0 and the
//          native dispatch then treated 0 as closure id 0 — a silent 0
//          (no closure registered yet), a wrong live closure (infinite
//          recursion → stack overflow → SIGSEGV) or a skipped form.
//
//   AC1: the lowering records every name reference it could not resolve
//        (the unbound-global sink) — an unbound callee in TAIL position,
//        in NON-TAIL position and in HIGHER-ORDER position.
//   AC2: the sink stays empty for names that DO resolve (a primitive
//        call, a function parameter, a top-level define) — the refuse
//        must not fire on a program eval can run.
//   AC3: source cites — the lowering records the substitution, exec_jit
//        refuses to compile such a program and takes the tree walker
//        (the eval face), and the canonical "unbound variable: <name>"
//        face lives in the tree walker's Variable case.
//   AC4: gate wiring — the source-cite linter, build.py and
//        scripts/coverage/root_check_allowlist.txt.

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import std;
import aura.core;
import aura.parser.parser;
import aura.compiler.evaluator;
import aura.compiler.lowering;
import aura.core.type;

namespace {

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

// Lower `code` and return the unbound-global names the lowering recorded
// (Issue #4607 sink: a name the scope chain, the free-var map, the value
// cells, the define cache, the primitive table and the self name all
// failed to resolve).
static std::vector<std::string> lower_unbound(std::string_view code) {
    std::vector<std::string> out;
    aura::ast::ASTArena arena;
    auto alloc = arena.allocator();
    aura::ast::StringPool pool(alloc);
    aura::ast::FlatAST flat(alloc);
    auto pr = aura::parser::parse_to_flat(code, flat, pool);
    if (!pr.success)
        return out;
    flat.root = pr.root;
    aura::compiler::Primitives prims;
    aura::core::TypeRegistry type_reg;
    std::vector<std::pair<std::string, aura::ast::NodeId>> sink;
    (void)aura::compiler::lower_to_ir(flat, pool, arena, &prims, &type_reg,
                                      /*narrowing_evidence=*/0, &sink);
    for (const auto& e : sink)
        out.push_back(e.first);
    return out;
}

static bool has_name(const std::vector<std::string>& names, std::string_view name) {
    for (const auto& n : names) {
        if (n == name)
            return true;
    }
    return false;
}

} // namespace

int run_test_jit_unbound_callee_4607() {
    std::println("=== Issue #4607: --jit unbound callee refuse ===");

    // ── AC1: the lowering records the unresolved name ─────────────
    {
        std::println("\n--- AC1: unbound callee positions ---");
        // TAIL position: the call is the body of a compiled function.
        auto tail = lower_unbound("(define (f x) (nope x)) (display (f 1))");
        CHECK(has_name(tail, "nope"), "AC1: tail-position unbound callee recorded");
        // NON-TAIL position: the call is an argument of another call.
        auto non_tail = lower_unbound("(define (f x) (+ 1 (nope x))) (display (f 1))");
        CHECK(has_name(non_tail, "nope"), "AC1: non-tail unbound callee recorded");
        // HIGHER-ORDER position: the unbound name is passed as a value
        // and called through a parameter.
        auto higher = lower_unbound("(define (apply1 g x) (g x)) (display (apply1 nope 1))");
        CHECK(has_name(higher, "nope"), "AC1: higher-order unbound callee recorded");
        // The issue's two top-level rows.
        auto top = lower_unbound("(display (nope 1))");
        CHECK(has_name(top, "nope"), "AC1: top-level unbound callee recorded");
        auto form = lower_unbound("(define-syntax m 1) (display 2)");
        CHECK(has_name(form, "define-syntax"), "AC1: unbound form name recorded");
    }

    // ── AC2: no false positive on a program eval can run ──────────
    {
        std::println("\n--- AC2: resolvable names are not recorded ---");
        auto prim = lower_unbound("(display (+ 1 2))");
        CHECK(prim.empty(), "AC2: primitive call records nothing");
        auto param = lower_unbound("(define (f x) (+ x 1)) (display (f 1))");
        CHECK(param.empty(), "AC2: function parameter records nothing");
        auto define = lower_unbound("(define y 5) (display y)");
        CHECK(define.empty(), "AC2: top-level define records nothing");
    }

    // ── AC3: source cites ─────────────────────────────────────────
    {
        std::println("\n--- AC3: source cites ---");
        auto low = read_file("src/compiler/lowering_impl.cpp");
        CHECK(low.find("Issue #4607") != std::string::npos, "AC3: lowering TU cites #4607");
        CHECK(low.find("unbound_globals->emplace_back") != std::string::npos,
              "AC3: lowering records the unresolved name");
        auto low_ixx = read_file("src/compiler/lowering.ixx");
        CHECK(low_ixx.find("unbound_globals") != std::string::npos,
              "AC3: lowering module exposes the sink");
        auto svc = read_file("src/compiler/service.ixx");
        CHECK(svc.find("Issue #4607") != std::string::npos, "AC3: driver TU cites #4607");
        auto jit = svc.find("[[nodiscard]] EvalResult exec_jit(");
        CHECK(jit != std::string::npos, "AC3: exec_jit entry present");
        if (jit != std::string::npos) {
            auto body = svc.substr(jit, 12000);
            CHECK(body.find("if (!unbound_globals.empty())") != std::string::npos,
                  "AC3: exec_jit refuses to compile a program with an unresolved name");
            CHECK(body.find("evaluator_.eval_flat(") != std::string::npos,
                  "AC3: the refuse takes the tree walker (the eval face)");
        }
        auto walk = read_file("src/compiler/evaluator_eval_flat.cpp");
        CHECK(walk.find("ErrorKind::UnboundVariable") != std::string::npos,
              "AC3: the canonical unbound-variable face lives in the tree walker");
    }

    // ── AC4: gate wiring ──────────────────────────────────────────
    {
        std::println("\n--- AC4: gate wiring ---");
        auto build = read_file("build.py");
        CHECK(build.find("check_jit_unbound_callee_4607.py") != std::string::npos,
              "AC4: build.py registers the linter");
        auto allow = read_file("scripts/coverage/root_check_allowlist.txt");
        CHECK(allow.find("check_jit_unbound_callee_4607.py") != std::string::npos,
              "AC4: root_check_allowlist.txt lists the linter");
    }

    std::println("\n=== #4607 jit unbound callee: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_jit_unbound_callee_4607();
}
#endif
