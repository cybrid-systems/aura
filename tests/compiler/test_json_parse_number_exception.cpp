// @category: unit
// @reason: Issue #2480 — json-parse parse_number catches stod/stoll
//          exceptions (out_of_range / invalid_argument → PRIM_ERROR).
//
//   AC1: oversized integer → error (not crash)
//   AC2: extreme float exp → error (not crash)
//   AC3: normal numbers still parse
//   AC4: source cites #2480 + try/catch on parse_number
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
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
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

// ── AC1: huge integer ──
static void ac1_int_out_of_range() {
    std::println("\n--- #2480 AC1: integer out of range ---");
    CompilerService cs;
    // Far beyond int64
    auto r = cs.eval(R"((json-parse "99999999999999999999999"))");
    CHECK(r.has_value(), "AC1: eval returns value");
    CHECK(r && is_error(*r), "AC1: PRIM_ERROR not exception crash");
}

// ── AC2: extreme float ──
static void ac2_float_out_of_range() {
    std::println("\n--- #2480 AC2: float exp out of range ---");
    CompilerService cs;
    auto r = cs.eval(R"((json-parse "1e10000"))");
    CHECK(r.has_value(), "AC2: eval returns value");
    // Some libstdc++ may clamp to inf rather than throw — either error or float is ok
    // as long as no crash. Prefer error when exception fires.
    CHECK(r && (is_error(*r) || is_float(*r)), "AC2: error or float (no crash)");
}

// ── AC3: normal parse ──
static void ac3_normal() {
    std::println("\n--- #2480 AC3: normal numbers ---");
    CompilerService cs;
    auto r = cs.eval(R"((json-parse "42"))");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 42, "AC3: int 42");
    auto r2 = cs.eval(R"((json-parse "3.5"))");
    CHECK(r2.has_value() && is_float(*r2), "AC3: float 3.5");
    auto r3 = cs.eval(R"((json-parse "{\"n\": -7}"))");
    CHECK(r3.has_value() && !is_error(*r3), "AC3: object with int");
}

// ── AC4: source ──
static void ac4_source() {
    std::println("\n--- #2480 AC4: source try/catch ---");
    auto src = read_file("src/compiler/evaluator_primitives_json.cpp");
    CHECK(!src.empty(), "AC4: read json primitives");
    CHECK(src.find("Issue #2480") != std::string::npos, "AC4: cites #2480");
    auto pos = src.find("auto parse_number");
    CHECK(pos != std::string::npos, "AC4: parse_number present");
    if (pos != std::string::npos) {
        auto win = src.substr(pos, 2200);
        CHECK(win.find("try") != std::string::npos, "AC4: try");
        CHECK(win.find("out_of_range") != std::string::npos, "AC4: out_of_range");
        CHECK(win.find("invalid_argument") != std::string::npos, "AC4: invalid_argument");
        CHECK(win.find("make_primitive_error") != std::string::npos, "AC4: PRIM_ERROR path");
        CHECK(win.find("number out of range") != std::string::npos, "AC4: range message");
        CHECK(win.find("SILENCE-PRIM") != std::string::npos, "AC4: SILENCE-PRIM mark");
    }
    // Registration wires error_values
    CHECK(src.find("error_values") != std::string::npos, "AC4: error_values param");
}

// ── AC5: gate ──
static void ac5_gate() {
    std::println("\n--- #2480 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/coverage/checks/check_json_parse_number_exception_2480.py");
    CHECK(build.find("check_json_parse_number_exception_2480") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_json_parse_number_exception_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_json_parse_number_exception") != std::string::npos, "AC5: cmake test");
    CHECK(!script.empty() && script.find("2480") != std::string::npos, "AC5: check script exists");
}

} // namespace

int run_test_json_parse_number_exception() {
    std::println("=== Issue #2480: json-parse number exception safety ===");
    ac1_int_out_of_range();
    ac2_float_out_of_range();
    ac3_normal();
    ac4_source();
    ac5_gate();
    std::println("\n=== #2480 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_json_parse_number_exception();
}
#endif
