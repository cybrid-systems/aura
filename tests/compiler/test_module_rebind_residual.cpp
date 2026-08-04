// @category: unit
// @reason: Issue #2579 — module free-vars / multi-define value init / export
//          residuals after #2566 #2569 #2570 #2581.
//          Also locks #2581 two-pass multi-define (private free-vars + export).
//
//   AC1: set-code multi-define (define g (f)) binds call result, not procedure
//   AC2: split stats+loop modules survive unimpacted mutate:rebind
//   AC3: large trailing export present on require surface
//   AC4: top-level define after rebind stays bound
//   AC5: source-cite + cmake

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace fs = std::filesystem;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
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

static fs::path find_lib_std() {
    for (const auto& p : {fs::path("lib"), fs::path("../lib"), fs::path("../../lib")}) {
        if (fs::exists(p / "std" / "mutate.aura") || fs::exists(p / "std" / "list.aura"))
            return fs::absolute(p);
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

static bool eval_bool(CompilerService& cs, std::string_view e) {
    auto r = cs.eval(e);
    return r && is_bool(*r) && as_bool(*r);
}

static void ac1_multidefine_call_init() {
    std::println("\n--- #2579 AC1: multi-define (define g (f)) call init ---");
    setenv("AURA_SANDBOX", "off", 1);
    CompilerService cs;
    CHECK(eval_ok(cs, "(set-code \"(define (f) 1) (define g (f))\")"), "set-code multi");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current");
    CHECK(eval_int_eq(cs, "g", 1), "AC1: g is 1 (not procedure)");
    CHECK(eval_bool(cs, "(number? g)"), "AC1: number? g");
    CHECK(eval_bool(cs, "(not (procedure? g))"), "AC1: not procedure? g");
    CHECK(eval_int_eq(cs, "(+ g 2)", 3), "AC1: (+ g 2) → 3");

    // Fresh service: pure value define (no multi-define pollution)
    CompilerService cs2;
    CHECK(eval_ok(cs2, "(set-code \"(define a 1)\")"), "set-code a");
    CHECK(eval_ok(cs2, "(eval-current)"), "eval a");
    CHECK(eval_int_eq(cs2, "a", 1), "AC1: a is 1 after value define");
    CHECK(eval_int_eq(cs2, "(+ a 2)", 3), "AC1: (+ a 2) after value define");
}

static void ac2_split_module_rebind() {
    std::println("\n--- #2579 AC2: split module free-vars survive rebind ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC2: lib found");
    if (lib.empty())
        return;

    auto tmp = fs::temp_directory_path() / "aura_2579_mod";
    fs::create_directories(tmp);
    {
        std::ofstream out(tmp / "aether-stats.aura");
        out << R"((export aether:stats-bump aether:stats-get)
(define *aether-stats* (hash "rounds" 0))
(define (aether:stats-bump key)
  (hash-set! *aether-stats* key (+ 1 (hash-ref *aether-stats* key 0)))
  (hash-ref *aether-stats* key 0))
(define (aether:stats-get key)
  (hash-ref *aether-stats* key 0))
)";
    }
    {
        std::ofstream out(tmp / "aether-loop.aura");
        out << R"((require "aether-stats" all:)
(export aether:loop-once)
(define (aether:loop-once)
  (aether:stats-bump "rounds")
  (aether:stats-get "rounds"))
)";
    }
    const auto path = lib.string() + ":" + tmp.string();
    setenv("AURA_PATH", path.c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);

    CompilerService cs;
    CHECK(eval_ok(cs, "(require \"aether-stats\" all:)"), "require stats");
    CHECK(eval_ok(cs, "(require \"aether-loop\" all:)"), "require loop");
    CHECK(eval_int_eq(cs, "(aether:loop-once)", 1), "loop 1");
    CHECK(eval_int_eq(cs, "(aether:loop-once)", 2), "loop 2");
    CHECK(eval_ok(cs, "(set-code \"(define (score x) (* x 2))\")"), "set-code");
    CHECK(eval_ok(cs, "(eval-current)"), "eval score");
    CHECK(eval_ok(cs, "(mutate:rebind \"score\" \"(lambda (x) (* x 3))\" \"t\")"), "rebind");
    CHECK(eval_ok(cs, "(eval-current)"), "eval after rebind");
    CHECK(eval_int_eq(cs, "(aether:loop-once)", 3), "AC2: loop after rebind");
    CHECK(eval_int_eq(cs, "(aether:stats-get \"rounds\")", 3), "AC2: stats after rebind");
    CHECK(eval_int_eq(cs, "(score 5)", 15), "AC2: score rebind live");

    fs::remove_all(tmp);
}

static void ac3_large_trailing_export() {
    std::println("\n--- #2579 AC3: large trailing export ---");
    const auto lib = find_lib_std();
    CHECK(!lib.empty(), "AC3: lib found");
    if (lib.empty())
        return;
    auto tmp = fs::temp_directory_path() / "aura_2579_big";
    fs::create_directories(tmp);
    {
        std::ofstream out(tmp / "bigmod.aura");
        out << "(export e1 e2 e3 loop-once)\n";
        out << "(define (e1 x) (+ x 1))\n";
        out << "(define (e2 x) (+ x 2))\n";
        out << "(define (e3 x) (+ x 3))\n";
        out << "(define (loop-once x)\n  (define acc 0)\n";
        for (int i = 0; i < 100; ++i)
            out << "  (set! acc (+ acc " << i << "))\n";
        out << "  (+ acc x))\n";
    }
    setenv("AURA_PATH", (lib.string() + ":" + tmp.string()).c_str(), 1);
    setenv("AURA_SANDBOX", "off", 1);
    CompilerService cs;
    CHECK(eval_ok(cs, "(require \"bigmod\" all:)"), "require bigmod");
    CHECK(eval_bool(cs, "(procedure? e1)"), "e1 export");
    CHECK(eval_bool(cs, "(procedure? e3)"), "e3 export");
    CHECK(eval_bool(cs, "(procedure? loop-once)"), "AC3: trailing loop-once export");
    CHECK(eval_int_eq(cs, "(e1 0)", 1), "e1 call");
    // sum 0..99 = 4950
    CHECK(eval_int_eq(cs, "(loop-once 0)", 4950), "AC3: large body runs");
    fs::remove_all(tmp);
}

static void ac4_define_after_rebind() {
    std::println("\n--- #2579 AC4: define after rebind ---");
    setenv("AURA_SANDBOX", "off", 1);
    CompilerService cs;
    CHECK(eval_ok(cs, "(set-code \"(define (score x) (* x 2))\")"), "set-code");
    CHECK(eval_ok(cs, "(eval-current)"), "eval");
    CHECK(eval_ok(cs, "(mutate:rebind \"score\" \"(lambda (x) (* x 3))\" \"t\")"), "rebind");
    CHECK(eval_ok(cs, "(eval-current)"), "eval rebind");
    CHECK(eval_int_eq(cs, "(score 5)", 15), "score *3");
    CHECK(eval_ok(cs, "(define post-cell 10)"), "define after rebind");
    CHECK(eval_int_eq(cs, "post-cell", 10), "AC4: post-cell bound");
    CHECK(eval_ok(cs, "(define (post-fn x) (+ x post-cell))"), "define fn after");
    CHECK(eval_int_eq(cs, "(post-fn 1)", 11), "AC4: post-fn works");
    CHECK(eval_ok(cs, "(eval-current)"), "re-eval workspace");
    CHECK(eval_int_eq(cs, "post-cell", 10), "AC4: post-cell survives re-eval");
}

static void ac5_source_gate() {
    std::println("\n--- #2579 AC5: source-cite + cmake ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2579") != std::string::npos, "AC5: multi-define cites #2579");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("#2579") != std::string::npos, "AC5: service cites #2579");
    CHECK(svc.find("sync_workspace_value_cells_from_env") != std::string::npos,
          "AC5: value-cell sync hook");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_module_rebind_residual") != std::string::npos, "AC5: cmake");
}

} // namespace

int run_test_module_rebind_residual() {
    std::println("=== Issue #2579: module rebind / multi-define residual ===");
    ac1_multidefine_call_init();
    ac2_split_module_rebind();
    ac3_large_trailing_export();
    ac4_define_after_rebind();
    ac5_source_gate();
    std::println("\n=== #2579: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_rebind_residual();
}
#endif
