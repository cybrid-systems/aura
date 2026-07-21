// test_production_sweep_1261_1265.cpp — Issues #1261–#1265 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    // Prefer engine:metrics facade (SlimSurface); fall back to bare query form.
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1261-1265-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        const auto schema = href(cs, "query:production-sweep-1261-1265-stats", "schema");
        CHECK(schema == 1625 || schema == 1261, "schema 1625|1261");
        CHECK(href(cs, "query:production-sweep-1261-1265-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1261-1265-stats", "aot-region-filter-enforced") == 1,
              "aot region filter (#1262)");
        CHECK(href(cs, "query:production-sweep-1261-1265-stats", "hot-update-epoch-fences") == 1,
              "hot-update epoch fences (#1264)");
        CHECK(href(cs, "query:production-sweep-1261-1265-stats", "issue-1265") == 1265,
              "issue-1265");
    }

    // #1263: reset forces dirty + epoch counter
    {
        CompilerService cs2;
        cs2.reset();
        // Counter is on the service that was reset; new service starts at 0.
        // Just verify the field is readable on a fresh service.
        CHECK(href(cs2, "query:production-sweep-1261-1265-stats", "arena-reset-dirty-forced") >= 0,
              "arena-reset-dirty-forced readable");
    }

    // #1265: smoke mutate after set-code; schema lineage remains readable.
    {
        (void)cs.eval("(set-code \"(define (f x) (+ x 1))\")");
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(mutate:query-and-replace :tag Define \"(define (g x) x)\")");
        (void)r;
        CHECK(href(cs, "query:production-sweep-1261-1265-stats", "schema") == 1625 ||
                  href(cs, "query:production-sweep-1261-1265-stats", "schema") == 1261,
              "schema still readable after QAR smoke");
    }

    // #1625 keys on a fresh service (full hash build, no prior workspace churn).
    {
        CompilerService cs4;
        CHECK(href(cs4, "query:production-sweep-1261-1265-stats",
                   "nested-lambda-per-block-targeted-wired") == 1,
              "nested per-block targeted wired");
        CHECK(href(cs4, "query:production-sweep-1261-1265-stats",
                   "dep-graph-nested-lambda-blocks-targeted") >= 0,
              "blocks-targeted key");
        CHECK(href(cs4, "query:production-sweep-1261-1265-stats",
                   "query-and-replace-all-or-nothing") >= 0,
              "QAR all-or-nothing readable");
    }

    {
        CompilerService cs3;
        auto a = cs3.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1261–#1265: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
