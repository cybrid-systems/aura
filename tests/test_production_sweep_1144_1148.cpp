// test_production_sweep_1144_1148.cpp — Issues #1144–#1148 Phase 1

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
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;

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
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1144-1148-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "schema") == 1144, "schema");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "flat-hash-insert-helper") == 1,
              "flat hash helper");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "selfevo-hyg-dirty-wired") == 1,
              "selfevo wired");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "per-fiber-ex-state-wired") == 1,
              "fiber wired");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "orch-telemetry-wired") == 1,
              "orch wired");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "dead-bump-audit-script") == 1,
              "audit script");
        CHECK(href(cs, "query:production-sweep-1144-1148-stats", "issue-1148") == 1148,
              "issue-1148");
    }

    // #1147: orch:metrics path bumps counters (void outside serve-async is fine)
    {
        auto r = cs.eval("(orch:metrics)");
        CHECK(r, "orch:metrics returns");
        auto s = cs.eval("(engine:metrics \"query:orchestration-telemetry-pipeline-stats\")");
        CHECK(s && is_hash(*s), "orch telemetry stats hash");
        CHECK(href(cs, "query:orchestration-telemetry-pipeline-stats", "total") >= 1,
              "orch total bumped");
    }

    // #1144 helper path still builds hashes
    {
        auto s = cs.eval("(engine:metrics \"query:per-fiber-exception-state-stats\")");
        CHECK(s && is_hash(*s), "per-fiber stats hash");
    }

    {
        auto a = cs.eval("(+ 1 1)");
        CHECK(a && is_int(*a) && as_int(*a) == 2, "(+ 1 1)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1144–#1148: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
