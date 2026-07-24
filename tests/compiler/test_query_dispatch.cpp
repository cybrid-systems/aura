// @category: integration
// @reason: Issue #1435 (query :op) unified dispatcher

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

} // namespace

int main() {
    CompilerService cs;

    {
        const std::string setup =
            "(set-code \"(define (f x) (+ x 1)) (define (g y) (f y)) (g 2)\")";
        CHECK(eval_ok(cs, setup), "set-code workspace");
        CHECK(eval_ok(cs, "(eval-current)"), "eval-current");
    }

    // AC1: :root / :node
    {
        auto root = eval_int(cs, "(query :root)");
        CHECK(root >= 0, "query :root non-neg");
        CHECK(eval_ok(cs, "(query :node " + std::to_string(root) + ")"), "query :node");
        CHECK(eval_ok(cs, "(query:node " + std::to_string(root) + ")"), "query:node alias");
    }

    // AC2: :children + :stable
    {
        auto root = eval_int(cs, "(query :root)");
        const auto id = std::to_string(root);
        CHECK(eval_ok(cs, "(query :children " + id + ")"), "query :children");
        CHECK(eval_ok(cs, "(query :children " + id + " :stable #t)"), "query :children :stable");
        CHECK(eval_ok(cs, "(query :children-stable " + id + ")"), "query :children-stable");
        CHECK(eval_ok(cs, "(query:children " + id + ")"), "query:children alias");
        CHECK(eval_ok(cs, "(query:children-stable " + id + ")"), "query:children-stable alias");
    }

    // AC3: :parent
    {
        auto root = eval_int(cs, "(query :root)");
        CHECK(eval_ok(cs, "(query :parent " + std::to_string(root) + ")"), "query :parent");
        CHECK(eval_ok(cs, "(query:parent " + std::to_string(root) + ")"), "query:parent alias");
    }

    // AC4: :find
    {
        CHECK(eval_ok(cs, "(query :find \"f\")"), "query :find f");
        CHECK(eval_ok(cs, "(query:find \"g\")"), "query:find alias");
    }

    // AC5: :def-use
    {
        CHECK(eval_ok(cs, "(query :def-use \"f\")"), "query :def-use");
        CHECK(eval_ok(cs, "(query:def-use \"f\")"), "query:def-use alias");
    }

    // AC6: :mutation-log
    {
        CHECK(eval_ok(cs, "(mutate:rebind \"f\" \"(lambda (x) (* x 2))\" \"t\")"), "mutate");
        CHECK(eval_ok(cs, "(query :mutation-log)"), "query :mutation-log");
        CHECK(eval_ok(cs, "(query:mutation-log)"), "query:mutation-log alias");
    }

    // AC7: api-reference
    {
        auto r = cs.eval("(api-reference)");
        CHECK(r && is_string(*r), "api-reference string");
        if (r && is_string(*r)) {
            auto idx = as_string_idx(*r);
            auto heap = cs.evaluator().string_heap();
            std::string s = idx < heap.size() ? heap[idx] : "";
            CHECK(s.find("query") != std::string::npos, "lists query");
            CHECK(s.find("*deprecated*") != std::string::npos, "*deprecated* section");
            CHECK(s.find("query:children") != std::string::npos, "deprecated query:children");
        }
    }

    // AC8: unknown op
    {
        auto r = cs.eval("(query :nope-such-op)");
        (void)r;
        CHECK(true, "unknown op no crash");
    }

    if (::aura::test::g_failed) {
        std::println(std::cerr, "query dispatch #1435: FAIL ({} failed / {} passed)",
                     ::aura::test::g_failed, ::aura::test::g_passed);
        return 1;
    }
    std::println("query dispatch #1435: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
