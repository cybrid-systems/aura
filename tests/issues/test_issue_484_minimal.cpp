// @category: integration
// @reason: minimal repro for #484
#include "test_harness.hpp"

import std;
using aura::test::g_failed;
using aura::test::g_passed;
import aura.compiler.value;
import aura.compiler.evaluator;
import aura.compiler.service;

namespace test_484_minimal_detail {

static bool set_source(aura::compiler::CompilerService& cs, std::string_view src) {
    std::string cmd = "(set-code \"";
    for (char c : src) {
        if (c == '\\' || c == '"')
            cmd += '\\';
        cmd += c;
    }
    cmd += "\")";
    auto r = cs.eval(cmd);
    return r && aura::compiler::types::is_bool(*r);
}

static int64_t run_int(aura::compiler::CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r)
        return -1;
    auto& v = *r;
    if (!aura::compiler::types::is_int(v))
        return -1;
    return aura::compiler::types::as_int(v);
}

bool test_minimal_repro() {
    std::println("\n--- #484 minimal repro ---");
    aura::compiler::CompilerService cs;
    if (!set_source(cs, "(begin (+ 1 1) (+ 2 2) (+ 3 3))")) {
        std::println(std::cerr, "set_source failed");
        ++g_failed;
        return false;
    }
    auto before = run_int(cs, "(length (query:pattern \"(+ ... ...)\"))");
    std::println(std::cerr, "before: (+ ... ...) = {}", before);
    cs.eval("(mutate:replace-pattern \"(+ ... ...)\" \"(* ... ...)\" \"test\")");
    cs.evaluator().force_build_tag_arity_index();
    auto after_plus = run_int(cs, "(length (query:pattern \"(+ ... ...)\"))");
    auto after_star = run_int(cs, "(length (query:pattern \"(* ... ...)\"))");
    std::println(std::cerr, "after_plus: {}", after_plus);
    std::println(std::cerr, "after_star: {}", after_star);
    CHECK(after_plus == 0, "after mutate: no (+ ...) (got " + std::to_string(after_plus) + ")");
    CHECK(after_star == 3, "after mutate: 3 (* ...) (got " + std::to_string(after_star) + ")");
    return true;
}

int run_tests() {
    std::println("═══ #484 minimal repro ═══");
    test_minimal_repro();
    std::println("\n═══ Results: {}/{} passed, {}/{} failed ═══", g_passed, g_passed + g_failed,
                 g_failed, g_passed + g_failed);
    return g_failed > 0 ? 1 : 0;
}

} // namespace test_484_minimal_detail

int aura_issue_484_minimal_run() {
    return test_484_minimal_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_484_minimal_run();
}
#endif
