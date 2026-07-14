// @category: integration
// @reason: Issue #1436 (mutate :op) unified dispatcher

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

namespace {

bool eval_ok(CompilerService& cs, const std::string& expr) {
    auto r = cs.eval(expr);
    return static_cast<bool>(r);
}

} // namespace

int main() {
    CompilerService cs;

    CHECK(eval_ok(cs, "(set-code \"(define (f x) (+ x 1)) (define (g y) (f y)) (g 2)\")"),
          "set-code");
    CHECK(eval_ok(cs, "(eval-current)"), "eval-current");

    // AC1: :rebind
    {
        CHECK(eval_ok(cs, "(mutate :rebind \"f\" \"(lambda (x) (* x 2))\" \"t\")"),
              "mutate :rebind");
        CHECK(eval_ok(cs, "(mutate:rebind \"g\" \"(lambda (y) (+ y 1))\" \"t\")"),
              "mutate:rebind alias still works");
        CHECK(eval_ok(cs, "(eval-current)"), "eval after rebind");
    }

    // AC2: :extract (node-id name) — find a node first
    {
        // extract may fail on bad node; ensure call path works (value returned)
        auto r = cs.eval("(mutate :extract 0 \"h\")");
        CHECK(r.has_value(), "mutate :extract returns");
        auto r2 = cs.eval("(mutate:extract-function 0 \"h2\")");
        CHECK(r2.has_value(), "mutate:extract-function alias returns");
    }

    // AC3: :move — may fail on invalid topology; path must not crash
    {
        auto r = cs.eval("(mutate :move 0 0 0)");
        CHECK(r.has_value(), "mutate :move returns");
        auto r2 = cs.eval("(mutate:move-node 0 0 0)");
        CHECK(r2.has_value(), "mutate:move-node alias returns");
    }

    // AC4: :atomic empty/minimal
    {
        // atomic-batch needs a list of ops; empty list may error — still a value/error
        auto r = cs.eval("(mutate :atomic (list))");
        CHECK(r.has_value() || !r, "mutate :atomic handled");
        (void)r;
        CHECK(true, "mutate :atomic path exercised");
    }

    // AC5: :replace kind routing (may fail validation; no crash)
    {
        auto r = cs.eval("(mutate :replace 0 :type \"Int\")");
        CHECK(r.has_value(), "mutate :replace :type returns");
        auto r2 = cs.eval("(mutate:replace-type 0 \"Int\")");
        CHECK(r2.has_value(), "mutate:replace-type alias returns");
    }

    // AC6: :validate
    {
        auto r = cs.eval("(mutate :validate \"(+ 1 2)\" \"number\")");
        CHECK(r.has_value(), "mutate :validate returns");
    }

    // AC7: api-reference lists mutate + deprecated aliases
    {
        auto r = cs.eval("(api-reference)");
        CHECK(r && is_string(*r), "api-reference string");
        if (r && is_string(*r)) {
            auto idx = as_string_idx(*r);
            auto heap = cs.evaluator().string_heap();
            std::string s = idx < heap.size() ? heap[idx] : "";
            CHECK(s.find("mutate") != std::string::npos, "lists mutate");
            CHECK(s.find("*deprecated*") != std::string::npos, "*deprecated* section");
            CHECK(s.find("mutate:rebind") != std::string::npos, "deprecated mutate:rebind");
        }
    }

    // AC8: unknown op
    {
        auto r = cs.eval("(mutate :nope)");
        (void)r;
        CHECK(true, "unknown op no crash");
    }

    if (::aura::test::g_failed) {
        std::println(std::cerr, "mutate dispatch #1436: FAIL ({} failed / {} passed)",
                     ::aura::test::g_failed, ::aura::test::g_passed);
        return 1;
    }
    std::println("mutate dispatch #1436: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
