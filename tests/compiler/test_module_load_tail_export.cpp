// @category: unit
// @reason: Issue #2570 — module load must not drop trailing defines/exports;
//          fail-closed on mid-body error and nested require failure.
//
//   AC1: tail defines always export after require
//   AC2: mid-body unbound/error fails require; no half-loaded symbols
//   AC3: nested require of missing module fails outer load
//   AC4: source-cite + cmake + gate

#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
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
using aura::compiler::types::as_float;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_float;
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

static bool write_mod(const std::string& dir, const std::string& name, const std::string& body) {
    std::filesystem::create_directories(dir);
    std::ofstream out(dir + "/" + name);
    if (!out)
        return false;
    out << body;
    return true;
}

static void ac1_tail_exports() {
    std::println("\n--- #2570 AC1: tail defines export ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac1";
    CHECK(write_mod(dir, "tail.aura",
                    R"((export early:fn late:fn)
(define (early:fn) 1)
(define (f1 x) x)
(define (f2 x) x)
(define (f3 x) x)
(define (f4 x) x)
(define (f5 x) x)
(define (late:fn) 2)
)"),
          "write tail.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"tail\" all:)");
    CHECK(req.has_value() && !(is_error(*req) && !is_string(*req)), "require succeeds");
    auto r1 = cs.eval("(early:fn)");
    CHECK(r1 && is_int(*r1) && as_int(*r1) == 1, "early:fn");
    auto r2 = cs.eval("(late:fn)");
    CHECK(r2 && is_int(*r2) && as_int(*r2) == 2, "AC1: late:fn tail export");
    unsetenv("AURA_PATH");
}

static void ac2_mid_error_fail_closed() {
    std::println("\n--- #2570 AC2: mid-body error fails load ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac2";
    CHECK(write_mod(dir, "partial.aura",
                    R"((export early:fn late:fn)
(define (early:fn) 1)
(define bad (no-such-xyz-2570 99))
(define (late:fn) 2)
)"),
          "write partial.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"partial\" all:)");
    CHECK(req.has_value(), "require returns a value");
    CHECK(req && is_error(*req) && !is_string(*req), "AC2: require fails with error");
    auto early = cs.eval("(try (early:fn) (catch (e) #f))");
    // half-load must not inject early:fn
    CHECK(early && is_bool(*early) && !as_bool(*early), "AC2: early not injected");
    auto late = cs.eval("(try (late:fn) (catch (e) #f))");
    CHECK(late && is_bool(*late) && !as_bool(*late), "AC2: late not injected");
    unsetenv("AURA_PATH");
}

static void ac3_nested_require_fail() {
    std::println("\n--- #2570 AC3: nested require failure fails outer ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac3";
    CHECK(write_mod(dir, "outer.aura",
                    R"((export outer:ok outer:late)
(define (outer:ok) 1)
(require "missing-mod-2570" all:)
(define (outer:late) 2)
)"),
          "write outer.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"outer\" all:)");
    CHECK(req.has_value() && is_error(*req) && !is_string(*req), "AC3: outer require fails");
    auto ok = cs.eval("(try (outer:ok) (catch (e) #f))");
    CHECK(ok && is_bool(*ok) && !as_bool(*ok), "AC3: no half-inject outer:ok");
    unsetenv("AURA_PATH");
}

static void ac4_source_gate() {
    std::println("\n--- #2570 AC4: source-cite + gate ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("#2570") != std::string::npos, "AC4: module_loader cites #2570");
    CHECK(loader.find("fail-closed") != std::string::npos ||
              loader.find("fail_load") != std::string::npos,
          "AC4: fail-closed load");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2570") != std::string::npos, "AC4: begin aborts on error");
    const auto mod = read_file("src/compiler/evaluator_primitives_module.cpp");
    CHECK(mod.find("#2570") != std::string::npos, "AC4: import surfaces error");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_module_load_tail_export") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_module_load_tail_2570") != std::string::npos, "AC4: check script");
    CHECK(build.find("cmd_module_load_tail_coverage") != std::string::npos, "AC4: gate cmd");
}

// Follow-up: multi-define (letrec) pre-allocates void cells; (define ceil ceil)
// must re-export the primitive, not fail void-export / leave uncallable.
// Also covers trunc prim registration used by std/math.
static void ac5_prim_reexport_math() {
    std::println("\n--- #2570 AC5: prim re-export + trunc + std/math ---");
    const std::string dir = "/tmp/aura_test_mod2570_ac5";
    CHECK(write_mod(dir, "reexport.aura",
                    R"((export floor ceil trunc round my-sq)
(define floor floor)
(define ceil ceil)
(define trunc trunc)
(define round round)
(define (my-sq x) (* x x))
)"),
          "write reexport.aura");
    setenv("AURA_PATH", dir.c_str(), 1);
    CompilerService cs;
    auto req = cs.eval("(require \"reexport\" all:)");
    CHECK(req.has_value() && !(is_error(*req) && !is_string(*req)), "AC5: reexport require");
    auto c = cs.eval("(ceil 3.2)");
    const bool ceil_ok =
        c && ((is_int(*c) && as_int(*c) == 4) || (is_float(*c) && as_float(*c) == 4.0));
    CHECK(ceil_ok, "AC5: ceil re-export callable");
    auto t = cs.eval("(trunc 3.7)");
    CHECK(t && is_float(*t) && as_float(*t) == 3.0, "AC5: trunc → 3.0");
    auto sq = cs.eval("(my-sq 5)");
    CHECK(sq && is_int(*sq) && as_int(*sq) == 25, "AC5: non-prim define still works");
    unsetenv("AURA_PATH");

    // std/math must load (was failing: export ceil/asin void; missing trunc).
    CompilerService cs2;
    auto math = cs2.eval("(begin (require \"std/math\" all:) (square 5))");
    CHECK(math && is_int(*math) && as_int(*math) == 25, "AC5: std/math square");
    auto asin0 = cs2.eval("(asin 0)");
    CHECK(asin0.has_value() && !(is_error(*asin0) && !is_string(*asin0)), "AC5: asin export");

    const auto math_cpp = read_file("src/compiler/evaluator_primitives_math.cpp");
    CHECK(math_cpp.find("add(\"trunc\"") != std::string::npos ||
              math_cpp.find("add(\"trunc\",") != std::string::npos,
          "AC5: trunc registered in math prims");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("void") != std::string::npos && flat.find("slot_for_name") != std::string::npos,
          "AC5: Variable void-cell→prim path");
}

} // namespace

int run_test_module_load_tail_export() {
    std::println("=== Issue #2570: module load tail export / fail-closed ===");
    ac1_tail_exports();
    ac2_mid_error_fail_closed();
    ac3_nested_require_fail();
    ac4_source_gate();
    ac5_prim_reexport_math();
    std::println("\n=== #2570: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_load_tail_export();
}
#endif
