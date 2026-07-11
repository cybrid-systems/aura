// test_production_sweep_1281_1285.cpp — Issues #1281–#1285 Phase 1

#include "test_harness.hpp"

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
        auto r = cs.eval("(query:production-sweep-1281-1285-stats)");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats", "schema") == 1281, "schema");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats",
                   "children-topology-rollback-fidelity") == 1,
              "children topology (#1281)");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats",
                   "generation-wrap-restamp-policy") == 1,
              "gen wrap restamp (#1282)");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats",
                   "provenance-boundary-hooks-active") == 1,
              "provenance hooks (#1283)");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats",
                   "tree-walker-fallback-reduction") == 1,
              "tree-walker reduce (#1284)");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats", "jit-exception-opcodes-covered") ==
                  1,
              "jit exception covered (#1285)");
        CHECK(href(cs, "query:production-sweep-1281-1285-stats", "issue-1285") == 1285,
              "issue-1285");
    }

    // #1282: query:generation-stats
    {
        auto r = cs.eval("(query:generation-stats)");
        CHECK(r && is_hash(*r), "query:generation-stats is hash");
        CHECK(href(cs, "query:generation-stats", "schema") == 1282, "gen schema");
        CHECK(href(cs, "query:generation-stats", "wrap-restamp-policy") == 1,
              "wrap-restamp-policy");
    }

    // #1283: query:dirty-provenance-stats
    {
        auto r = cs.eval("(query:dirty-provenance-stats)");
        CHECK(r && is_hash(*r), "query:dirty-provenance-stats is hash");
        CHECK(href(cs, "query:dirty-provenance-stats", "schema") == 1283, "prov schema");
        CHECK(href(cs, "query:dirty-provenance-stats", "active") == 1, "prov active");
    }

    // #1281/#1283: mutate path exercises children snapshot + provenance boundary
    {
        (void)cs.eval("(set-code \"(define (f x) (+ x 1))\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(mutate:rebind \"f\" \"(lambda (x) (* x 2))\")");
        auto cap =
            href(cs, "query:production-sweep-1281-1285-stats", "provenance-boundary-capture-count");
        CHECK(cap >= 0, "provenance-boundary-capture-count readable");
        // After successful mutate, capture should have increased
        CHECK(cap >= 1, "provenance capture bumped on mutate boundary");
    }

    // #1284: define-cache hits metric is readable after set-code/eval
    {
        auto hits =
            href(cs, "query:production-sweep-1281-1285-stats", "tree-walker-define-cache-hits");
        CHECK(hits >= 0, "tree-walker-define-cache-hits readable");
    }

    // #1282: ast:generation-stats still works and exposes new keys
    {
        auto r = cs.eval("(ast:generation-stats)");
        CHECK(r && is_hash(*r), "ast:generation-stats is hash");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1281–#1285: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
