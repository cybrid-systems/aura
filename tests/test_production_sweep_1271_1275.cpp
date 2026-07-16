// test_production_sweep_1271_1275.cpp — Issues #1271–#1275 Phase 1

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

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
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1271-1275-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1271-1275-stats", "schema") == 1271, "schema");
        CHECK(href(cs, "query:production-sweep-1271-1275-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1271-1275-stats", "runtime-obs-export-ready") == 1,
              "runtime obs (#1272)");
        CHECK(href(cs, "query:production-sweep-1271-1275-stats",
                   "ir-hygiene-macro-marker-enforced") == 1,
              "ir hygiene (#1273)");
        CHECK(href(cs, "query:production-sweep-1271-1275-stats", "hygiene-edsl-awareness") == 1,
              "hygiene edsl (#1275)");
        CHECK(href(cs, "query:production-sweep-1271-1275-stats", "issue-1275") == 1275,
              "issue-1275");
    }

    // #1271: reemit skeleton is callable
    {
        const auto n = aura_reemit_aot_for_dirty(0);
        CHECK(n == 0, "reemit dirty skeleton returns 0");
        (void)aura_aot_last_commit_epoch();
    }

    // #1274: mutate path exercises Guard flush (dirty→IR may bump)
    {
        (void)cs.eval("(set-code \"(define (h x) (+ x 1))\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:rebind \"h\" \"(lambda (x) (* x 2))\")");
        auto dirty = href(cs, "query:production-sweep-1271-1275-stats", "dirty-propagation-to-ir");
        CHECK(dirty >= 0, "dirty-propagation-to-ir readable");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1271–#1275: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
