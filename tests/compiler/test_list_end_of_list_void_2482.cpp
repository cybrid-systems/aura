// @category: unit
// @reason: Issue #2482 — is_end_of_list / null? treat only void as empty
//          list; int 0 is a number (not list terminator). List lowering
//          emits ConstVoid (not ConstI64 0) for empty tails.
//
//   AC1: (null? 0) → false; (null? (list)) → true
//   AC2: (list? 0) → false; (list? (list ...)) → true
//   AC3: (length 0) → 0; (length (cons 1 0)) → 1; proper lists OK
//   AC4: source void-only + list ConstVoid + PrimNullP void-only
//   AC5: gate wiring

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
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
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

static void ac1_null() {
    std::println("\n--- #2482 AC1: null? ---");
    CompilerService cs;
    auto lst = cs.eval("(list)");
    CHECK(lst.has_value() && is_void(*lst), "AC1: (list) is void empty list");
    auto z = cs.eval("(null? 0)");
    CHECK(z.has_value() && is_bool(*z) && !as_bool(*z), "AC1: (null? 0) → false");
    auto empty = cs.eval("(null? (list))");
    CHECK(empty.has_value() && is_bool(*empty) && as_bool(*empty), "AC1: (null? (list)) → true");
    auto cons = cs.eval("(null? (cons 1 0))");
    CHECK(cons.has_value() && is_bool(*cons) && !as_bool(*cons), "AC1: (null? (cons 1 0)) → false");
    auto one = cs.eval("(null? (list 1))");
    CHECK(one.has_value() && is_bool(*one) && !as_bool(*one), "AC1: (null? (list 1)) → false");
}

static void ac2_listp() {
    std::println("\n--- #2482 AC2: list? ---");
    CompilerService cs;
    auto z = cs.eval("(list? 0)");
    CHECK(z.has_value() && is_bool(*z) && !as_bool(*z), "AC2: (list? 0) → false");
    auto empty = cs.eval("(list? (list))");
    CHECK(empty.has_value() && is_bool(*empty) && as_bool(*empty), "AC2: (list? (list)) → true");
    auto proper = cs.eval("(list? (list 1 2 3))");
    CHECK(proper.has_value() && is_bool(*proper) && as_bool(*proper), "AC2: proper list → true");
    auto improper = cs.eval("(list? (cons 1 0))");
    CHECK(improper.has_value() && is_bool(*improper) && !as_bool(*improper),
          "AC2: (list? (cons 1 0)) → false");
}

static void ac3_length() {
    std::println("\n--- #2482 AC3: length ---");
    CompilerService cs;
    auto z = cs.eval("(length 0)");
    CHECK(z.has_value() && is_int(*z) && as_int(*z) == 0, "AC3: (length 0) → 0");
    auto improper = cs.eval("(length (cons 1 0))");
    CHECK(improper.has_value() && is_int(*improper) && as_int(*improper) == 1,
          "AC3: (length (cons 1 0)) → 1");
    auto proper = cs.eval("(length (list 1 2 3))");
    CHECK(proper.has_value() && is_int(*proper) && as_int(*proper) == 3, "AC3: length 3");
    auto empty = cs.eval("(length (list))");
    CHECK(empty.has_value() && is_int(*empty) && as_int(*empty) == 0, "AC3: length empty");
    auto mem = cs.eval("(member 1 (list 0 1 2))");
    CHECK(mem.has_value() && !is_int(*mem), "AC3: member finds 1 after 0 element");
}

static void ac4_source() {
    std::println("\n--- #2482 AC4: source ---");
    auto src = read_file("src/compiler/evaluator_primitives_list.cpp");
    CHECK(!src.empty(), "AC4: read list primitives");
    CHECK(src.find("Issue #2482") != std::string::npos, "AC4: cites #2482");
    auto pos = src.find("bool is_end_of_list");
    CHECK(pos != std::string::npos, "AC4: is_end_of_list present");
    if (pos != std::string::npos) {
        auto win = src.substr(pos, 400);
        CHECK(win.find("is_void(v)") != std::string::npos, "AC4: void-only end");
        CHECK(win.find("as_int(v) == 0") == std::string::npos, "AC4: no int-0 end check");
    }
    auto npos = src.find("add(\"null?\"");
    CHECK(npos != std::string::npos, "AC4: null? present");
    if (npos != std::string::npos) {
        auto win = src.substr(npos, 250);
        CHECK(win.find("is_void") != std::string::npos, "AC4: null? uses is_void");
        CHECK(win.find("as_int") == std::string::npos, "AC4: null? no as_int");
    }
    auto rt = read_file("src/compiler/evaluator_primitives_runtime.cpp");
    CHECK(rt.find("Issue #2482") != std::string::npos, "AC4: runtime also cites #2482");

    auto low = read_file("src/compiler/lowering_impl.cpp");
    auto lpos = low.find("Expand (list a b c)");
    if (lpos == std::string::npos)
        lpos = low.find("callee_name == \"list\"");
    CHECK(lpos != std::string::npos, "AC4: list lowering present");
    if (lpos != std::string::npos) {
        auto win = low.substr(lpos, 1100);
        CHECK(win.find("ConstVoid") != std::string::npos, "AC4: list uses ConstVoid");
        CHECK(win.find("Issue #2482") != std::string::npos, "AC4: list lowering cites #2482");
        // empty + tail must not use ConstI64 0
        CHECK(win.find("ConstI64") == std::string::npos, "AC4: list no ConstI64 empty");
    }

    auto jit = read_file("src/compiler/aura_jit.cpp");
    auto jpos = jit.find("case PrimNullP:");
    CHECK(jpos != std::string::npos, "AC4: PrimNullP present");
    if (jpos != std::string::npos) {
        auto win = jit.substr(jpos, 500);
        CHECK(win.find("Issue #2482") != std::string::npos, "AC4: PrimNullP cites #2482");
        CHECK(win.find("is_zero") == std::string::npos, "AC4: PrimNullP no is_zero");
        CHECK(win.find("CreateOr") == std::string::npos, "AC4: PrimNullP no void|zero Or");
    }
}

static void ac5_gate() {
    std::println("\n--- #2482 AC5: gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    CHECK(build.find("check_list_end_of_list_void_2482") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_list_end_of_list_void_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_list_end_of_list_void_2482") != std::string::npos, "AC5: cmake test");
    CHECK(!read_file("scripts/coverage/checks/check_list_end_of_list_void_2482.py").empty(),
          "AC5: check script exists");
}

} // namespace

int run_test_list_end_of_list_void_2482() {
    std::println("=== Issue #2482: list end-of-list is void only ===");
    ac1_null();
    ac2_listp();
    ac3_length();
    ac4_source();
    ac5_gate();
    std::println("\n=== #2482 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_list_end_of_list_void_2482();
}
#endif
