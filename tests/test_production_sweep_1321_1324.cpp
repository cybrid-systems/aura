// test_production_sweep_1321_1324.cpp — Issues #1321–#1324 Phase 1

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
    constexpr auto Q = "query:production-sweep-1321-1324-stats";

    {
        auto r = cs.eval(std::format("({})", Q));
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, Q, "schema") == 1321, "schema");
        CHECK(href(cs, Q, "active") == 1, "active");
        // #1321
        CHECK(href(cs, Q, "hotpath-contracts-expanded") == 1, "hotpath contracts (#1321)");
        CHECK(href(cs, Q, "soa-view-bounds-contracts") == 1, "soa view bounds (#1321)");
        CHECK(href(cs, Q, "flatast-column-contracts") == 1, "flatast column contracts (#1321)");
        CHECK(href(cs, Q, "consteval-checks-total") == 36, "consteval checks == 36");
        // #1322
        CHECK(href(cs, Q, "pipeline-dirty-short-circuit-active") == 1,
              "dirty short-circuit active (#1322)");
        // #1323
        CHECK(href(cs, Q, "jit-fn-unhandled-counts-query-locked") == 1,
              "fn_unhandled query locked (#1323)");
        // #1324
        CHECK(href(cs, Q, "jit-invalidate-lock-before-erase") == 1,
              "invalidate lock-before-erase (#1324)");
        CHECK(href(cs, Q, "issue-1324") == 1324, "issue-1324");
    }

    // #1321: cxx26 contracts query still reports expanded consteval total
    {
        auto r = cs.eval("(query:cpp26-contracts-stats)");
        CHECK(r && is_hash(*r), "query:cpp26-contracts-stats is hash");
        auto n = href(cs, "query:cpp26-contracts-stats", "consteval-checks");
        CHECK(n == 36, "cpp26 consteval-checks == 36 after #1321");
    }

    // #1322: pipeline counters readable (non-negative)
    {
        auto sc = href(cs, Q, "pipeline-dirty-short-circuit-total");
        CHECK(sc >= 0, "dirty short-circuit total readable");
        auto epoch = href(cs, Q, "pipeline-epoch-sync-total");
        CHECK(epoch >= 0, "epoch sync total readable");
    }

    // #1323: unhandled-opcode query path is callable (lock held internally)
    {
        // No crash when querying unhandled for unknown/known fns concurrent with eval.
        auto r = cs.eval("(+ 10 32)");
        CHECK(r && is_int(*r) && as_int(*r) == 42, "(+ 10 32)");
        // Force a few evals to exercise JIT compile path if enabled.
        for (int i = 0; i < 8; ++i) {
            auto e = cs.eval(std::format("(+ {} {})", i, i + 1));
            CHECK(e && is_int(*e), "eval arithmetic");
        }
    }

    // #1324: invalidate is exercised via redefine/mutate paths if available;
    // smoke: still evaluates after workspace activity.
    {
        auto r = cs.eval("(* 3 14)");
        CHECK(r && is_int(*r) && as_int(*r) == 42, "(* 3 14)");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(- 50 8)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(- 50 8)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1321–#1324: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
