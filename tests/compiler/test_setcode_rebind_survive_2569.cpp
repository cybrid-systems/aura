// @category: unit
// @reason: Issue #2569 — set-code / mutate:rebind must not kill unimpacted
//          closures or hash telemetry (Aether closed-loop agent state).
//
//   AC1: define bump + set-code seed + N rebind rounds — bump stays callable
//   AC2: hash-ref/hash-set! survive rebind with correct values
//   AC3: (hash-ref h k default) honors default (IR 3-arg form)
//   AC4: source-cite + cmake + gate

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
using aura::compiler::types::as_int;
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

static bool eval_int_eq(CompilerService& cs, std::string_view e, std::int64_t n) {
    auto r = cs.eval(e);
    return r && is_int(*r) && as_int(*r) == n;
}

static void ac1_closure_survive() {
    std::println("\n--- #2569 AC1: closures survive set-code + rebind ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/mutate\" all:)").has_value(), "require mutate");
    CHECK(cs.eval("(define box (list 0))").has_value(), "define box");
    CHECK(cs.eval("(define bump (lambda () (set-car! box (+ (car box) 1)) (car box)))").has_value(),
          "define bump");
    CHECK(eval_int_eq(cs, "(bump)", 1), "bump #1");
    CHECK(cs.eval("(set-code \"(define score (lambda (x) (* x 2)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    CHECK(eval_int_eq(cs, "(bump)", 2), "bump after set-code");
    CHECK(eval_int_eq(cs, "(score 5)", 10), "score *2");
    CHECK(cs.eval("(mutate:rebind \"score\" \"(lambda (x) (* x 3))\" \"t\")").has_value(),
          "rebind *3");
    CHECK(cs.eval("(eval-current)").has_value(), "eval after rebind");
    auto b3 = cs.eval("(bump)");
    std::println("  restamp_total={}", cs.evaluator().get_live_closure_epoch_restamp_total());
    std::println("  bump after rebind: void={} int={}", b3 && is_void(*b3),
                 b3 && is_int(*b3) ? as_int(*b3) : -99);
    CHECK(b3 && is_int(*b3) && as_int(*b3) == 3, "AC1: bump after rebind");
    CHECK(eval_int_eq(cs, "(score 5)", 15), "score *3");
    // N rounds without invalid closure
    for (int i = 0; i < 4; ++i) {
        CHECK(cs.eval("(mutate:rebind \"score\" \"(lambda (x) (+ x 1))\" \"t\")").has_value(),
              "rebind loop");
        CHECK(cs.eval("(eval-current)").has_value(), "eval loop");
        auto br = cs.eval("(bump)");
        CHECK(br && is_int(*br), "AC1: bump survives rebind round");
    }
}

static void ac2_hash_survive() {
    std::println("\n--- #2569 AC2: hash telemetry survives rebind ---");
    CompilerService cs;
    CHECK(cs.eval("(require \"std/mutate\" all:)").has_value(), "require");
    CHECK(cs.eval("(define *h* (hash \"rounds\" 0 \"commits\" 0))").has_value(), "hash");
    CHECK(cs.eval("(define (hbump key) (hash-set! *h* key (+ 1 (hash-ref *h* key 0))) "
                  "(hash-ref *h* key 0))")
              .has_value(),
          "hbump");
    CHECK(eval_int_eq(cs, "(hbump \"rounds\")", 1), "hbump rounds 1");
    CHECK(cs.eval("(set-code \"(define score (lambda (x) (* x 2)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(eval_int_eq(cs, "(hbump \"rounds\")", 2), "hbump after set-code");
    CHECK(cs.eval("(mutate:rebind \"score\" \"(lambda (x) (* x 3))\" \"t\")").has_value(),
          "rebind");
    CHECK(cs.eval("(eval-current)").has_value(), "eval rebind");
    CHECK(eval_int_eq(cs, "(hbump \"commits\")", 1), "AC2: hbump commits after rebind");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"rounds\" 0)", 2), "AC2: rounds value intact");
}

static void ac3_hash_ref_default() {
    std::println("\n--- #2569 AC3: hash-ref default (3-arg IR form) ---");
    CompilerService cs;
    CHECK(cs.eval("(define *h* (hash \"a\" 1))").has_value(), "hash");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"a\")", 1), "2-arg hit");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"a\" 99)", 1), "3-arg hit keeps value");
    CHECK(eval_int_eq(cs, "(hash-ref *h* \"missing\" 99)", 99), "3-arg miss returns default");
}

static void ac4_source_gate() {
    std::println("\n--- #2569 AC4: source-cite + gate ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2569") != std::string::npos, "AC4: eval_flat cites #2569");
    CHECK(flat.find("soft-recover") != std::string::npos ||
              flat.find("soft_recover") != std::string::npos,
          "AC4: soft-recover unimpacted closures");
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("#2569") != std::string::npos, "AC4: lowering cites #2569");
    CHECK(low.find("hash-ref") != std::string::npos, "AC4: hash-ref IR fix");
    const auto vec = read_file("src/compiler/evaluator_primitives_vector.cpp");
    CHECK(vec.find("#2569") != std::string::npos, "AC4: hash-ref default");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_setcode_rebind_survive_2569") != std::string::npos, "AC4: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_setcode_rebind_2569") != std::string::npos, "AC4: check script");
    CHECK(build.find("cmd_setcode_rebind_coverage") != std::string::npos, "AC4: gate cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2569: set-code/rebind closure+hash survival ===");
    ac1_closure_survive();
    ac2_hash_survive();
    ac3_hash_ref_default();
    ac4_source_gate();
    std::println("\n=== #2569: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
