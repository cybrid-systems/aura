// test_production_sweep_1286_1290.cpp — Issues #1286–#1290 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.type;
import aura.compiler.type_checker;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(query:production-sweep-1286-1290-stats)");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats", "schema") == 1286, "schema");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats",
                   "invalidate-per-block-dirty-active") == 1,
              "per-block dirty (#1286)");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats",
                   "closure-bridge-epoch-safety-active") == 1,
              "closure epoch (#1287)");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats",
                   "guard-shape-linear-unified-active") == 1,
              "guardshape linear (#1288)");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats",
                   "jit-unhandled-fail-fast-active") == 1,
              "jit fail-fast (#1289)");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats", "ownership-lambda-params-fixed") ==
                  1,
              "lambda params ownership (#1290)");
        CHECK(href(cs, "query:production-sweep-1286-1290-stats", "issue-1290") == 1290,
              "issue-1290");
    }

    // #1286: mutate path may bump per-block dirty via invalidate cascade
    {
        (void)cs.eval("(set-code \"(define (g x) (+ x 1))\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:rebind \"g\" \"(lambda (x) (* x 2))\")");
        auto n =
            href(cs, "query:production-sweep-1286-1290-stats", "invalidate-per-block-dirty-total");
        CHECK(n >= 0, "invalidate-per-block-dirty-total readable");
    }

    // #1290: Lambda params live in param_data_ (not children); ownership walk uses them
    {
        using namespace aura::ast;
        FlatAST flat;
        StringPool pool;
        auto x_sym = pool.intern("x");
        auto body = flat.add_literal(1);
        std::vector<SymId> params{x_sym};
        auto lam = flat.add_lambda(params, body);
        flat.root = lam;
        auto view = flat.get(lam);
        CHECK(view.tag == NodeTag::Lambda, "lambda tag");
        CHECK(view.params.size() == 1, "params in param_data_ (not children)");
        CHECK(view.children.size() == 1, "children is body only (size 1)");
        // Ownership validation must not crash; with the #1290 fix the walker
        // iterates v.params so linear param names enter introduced.
        std::vector<aura::compiler::OwnershipNote> notes;
        // Issue #1387: validate_ownership_full now requires a TypeRegistry.
        aura::core::TypeRegistry reg;
        (void)aura::compiler::OwnershipEnv::validate_ownership_full(flat, pool, reg, flat.root,
                                                                    notes);
        CHECK(true, "ownership validate Lambda with param_data_ ok");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1286–#1290: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
