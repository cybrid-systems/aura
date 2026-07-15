// test_selfevo_bugfix_941_967.cpp — Issues #941–#967 Phase 1
//
// Self-evo pipeline dashboard + bugfix surface checks.

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \'{}\')", aura::test::aura_call_expr(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    // #941–#954: self-evo pipeline dashboard
    {
        auto r = cs.eval("(engine:metrics \"query:self-evo-pipeline-stats\")");
        CHECK(r && is_hash(*r), "query:self-evo-pipeline-stats is hash");
        CHECK(href(cs, "query:self-evo-pipeline-stats", "schema") == 941, "schema 941");
        CHECK(href(cs, "query:self-evo-pipeline-stats", "issue-954") == 954, "issue-954 field");
        CHECK(href(cs, "query:self-evo-pipeline-stats", "active") == 1, "pipeline active");
    }

    for (int issue : {941, 942, 943, 944, 945, 946, 947, 948, 949, 950, 951, 952, 953, 954}) {
        auto r = cs.eval(std::format("(selfevo:audit-bump {})", issue));
        CHECK(r && is_bool(*r) && as_bool(*r),
              std::format("selfevo:audit-bump {} ok", issue).c_str());
    }
    CHECK(href(cs, "query:self-evo-pipeline-stats", "composite-tx") >= 1, "composite-tx bumped");

    // #955–#967: bugfix dashboard flags
    {
        auto r = cs.eval("(engine:metrics \"query:bugfix-941-967-stats\")");
        CHECK(r && is_hash(*r), "query:bugfix-941-967-stats is hash");
        CHECK(href(cs, "query:bugfix-941-967-stats", "schema") == 955, "schema 955");
        CHECK(href(cs, "query:bugfix-941-967-stats", "session-unregister-wired") == 1,
              "session unregister wired");
        CHECK(href(cs, "query:bugfix-941-967-stats", "http-async-unified") == 1,
              "http async unified");
        CHECK(href(cs, "query:bugfix-941-967-stats", "defuse-version-prod-api") == 1,
              "defuse prod API");
        CHECK(href(cs, "query:bugfix-941-967-stats", "eval-on-current-guard") == 1,
              "eval_on_current guard");
        CHECK(href(cs, "query:bugfix-941-967-stats", "ir-cache-max-size") == 2048,
              "ir cache max size");
        CHECK(href(cs, "query:bugfix-941-967-stats", "autofix-unbound-safe") == 1,
              "autofix unbound safe");
        CHECK(href(cs, "query:bugfix-941-967-stats", "gcsweep-shared-layout") == 1,
              "gcsweep shared layout");
        CHECK(href(cs, "query:bugfix-941-967-stats", "lexer-nul-escape") == 1, "lexer nul");
        CHECK(href(cs, "query:bugfix-941-967-stats", "hygiene-builtins-expanded") == 1,
              "hygiene expanded");
        CHECK(href(cs, "query:bugfix-941-967-stats", "module-path-heap-realpath") == 1,
              "module path heap realpath");
    }

    // #957: production defuse_version() is callable
    {
        auto v = cs.evaluator().defuse_version();
        CHECK(v >= 0, "defuse_version production API");
        (void)v;
    }

    // Registry still healthy after new primitives
    {
        auto car = cs.eval("(car (list-sort (list 2 1)))");
        CHECK(car && is_int(*car) && as_int(*car) == 1, "list-sort ascending still works");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("self-evo + bugfix #941–#967: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
