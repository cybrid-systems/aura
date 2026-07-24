// tests/compiler/test_workspace_dispatch.cpp — Issue #1437: workspace :op dispatch contract test.
// @reason: Issue #1437 (workspace :op) unified dispatcher

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

namespace {

bool eval_ok(CompilerService& cs, const std::string& expr) {
    return static_cast<bool>(cs.eval(expr));
}

std::int64_t eval_int(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval(expr);
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

bool eval_truthy(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval(expr);
    if (!r)
        return false;
    if (is_bool(*r))
        return as_bool(*r);
    if (is_int(*r))
        return as_int(*r) != 0;
    return true;
}

} // namespace

int main() {
    CompilerService cs;

    // Seed a root workspace with some code
    CHECK(eval_ok(cs, "(set-code \"(define (f x) (+ x 1)) (f 1)\")"), "set-code");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current");

    // AC1: :create
    {
        auto id = eval_int(cs, "(workspace :create \"child-a\")");
        CHECK(id >= 0, "workspace :create returns id");
        auto id2 = eval_int(cs, "(workspace:create \"child-b\")");
        CHECK(id2 >= 0, "workspace:create alias still works");
    }

    // AC2: :switch
    {
        auto id = eval_int(cs, "(workspace :create \"switch-me\")");
        CHECK(id >= 0, "create for switch");
        CHECK(eval_truthy(cs, "(workspace :switch " + std::to_string(id) + ")"),
              "workspace :switch");
        // switch back to 0 (root) if present
        CHECK(eval_ok(cs, "(workspace :switch 0)"), "switch to root");
        CHECK(eval_ok(cs, "(workspace:switch 0)"), "workspace:switch alias");
    }

    // AC3: :lock / :unlock
    {
        auto id = eval_int(cs, "(workspace :create \"lock-me\")");
        CHECK(id >= 0, "create for lock");
        CHECK(eval_truthy(cs, "(workspace :lock " + std::to_string(id) + ")"), "workspace :lock");
        CHECK(eval_truthy(cs, "(workspace :unlock " + std::to_string(id) + ")"),
              "workspace :unlock");
        CHECK(eval_truthy(cs, "(workspace:lock " + std::to_string(id) + ")"),
              "workspace:lock alias");
        CHECK(eval_truthy(cs, "(workspace:unlock " + std::to_string(id) + ")"),
              "workspace:unlock alias");
    }

    // AC4: :merge (may no-op/fail without divergent child — still returns)
    {
        auto id = eval_int(cs, "(workspace :create \"merge-me\")");
        CHECK(id >= 0, "create for merge");
        auto r = cs.eval("(workspace :merge " + std::to_string(id) + ")");
        CHECK(r.has_value(), "workspace :merge returns");
        auto r2 = cs.eval("(workspace:merge " + std::to_string(id) + ")");
        CHECK(r2.has_value(), "workspace:merge alias returns");
    }

    // AC5: :list / :current convenience
    {
        CHECK(eval_ok(cs, "(workspace :list)"), "workspace :list");
        CHECK(eval_ok(cs, "(workspace :current)"), "workspace :current");
    }

    // AC6: api-reference
    {
        auto r = cs.eval("(api-reference)");
        CHECK(r && is_string(*r), "api-reference string");
        if (r && is_string(*r)) {
            auto idx = as_string_idx(*r);
            auto heap = cs.evaluator().string_heap();
            std::string s = idx < heap.size() ? heap[idx] : "";
            CHECK(s.find("workspace") != std::string::npos, "lists workspace");
            CHECK(s.find("*deprecated*") != std::string::npos, "*deprecated* section");
            CHECK(s.find("workspace:create") != std::string::npos, "deprecated workspace:create");
        }
    }

    // AC7: unknown op
    {
        auto r = cs.eval("(workspace :nope)");
        (void)r;
        CHECK(true, "unknown op no crash");
    }

    if (::aura::test::g_failed) {
        std::println(std::cerr, "workspace dispatch #1437: FAIL ({} failed / {} passed)",
                     ::aura::test::g_failed, ::aura::test::g_passed);
        return 1;
    }
    std::println("workspace dispatch #1437: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
