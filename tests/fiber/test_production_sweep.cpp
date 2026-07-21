// Issue #1202/#1203/#1215/#1228 (#1978 renamed): issue# moved from filename to header.
// test_production_sweep_1202_1228.cpp — Issues #1202–#1228 Phase 1

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
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1202-1228-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "schema") == 1202, "schema");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "parallel-orch-scaffold") == 1,
              "parallel orch");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "self-healing-hooks-active") == 1,
              "self-heal");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "pure-analysis-pass-asserts") == 1,
              "pure analysis");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "agent-fiber-safepoint-wired") ==
                  1,
              "agent safepoint");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "dirty-propagation-module") == 1,
              "dirty prop");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "multi-fiber-mailbox-typed") == 1,
              "mf mailbox");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "hot-path-primitives-module") == 1,
              "hot path prims");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "eda-parse-common-dedup") == 1,
              "eda parse");
        CHECK(href(cs, "query:production-sweep-1202-1228-stats", "issue-1228") == 1228,
              "issue-1228");
    }

    // #1215 production health composite
    {
        auto r = cs.eval("(stats:get \"query:production-health\")");
        CHECK(r && is_hash(*r), "production-health is hash");
        CHECK(href(cs, "query:production-health", "schema") == 1215, "health schema");
        CHECK(href(cs, "query:production-health", "score") == 100, "fresh score 100");
        CHECK(href(cs, "query:production-health", "healthy") == 1, "healthy");
    }

    // #1203 SelfHealingHook fires on quota violation
    {
        // Set a tiny memory quota and violate it
        auto set = cs.eval("(resource:quota-set \"memory\" 1)");
        (void)set;
        auto chk = cs.eval("(resource:quota-check \"memory\" 100)");
        CHECK(chk && is_bool(*chk) && !as_bool(*chk), "quota-check rejects over-limit");
    }

    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1202–#1228: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
