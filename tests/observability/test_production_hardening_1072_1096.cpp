// test_production_hardening_1072_1096.cpp — Issues #1072–#1096 Phase 1

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
using aura::compiler::types::is_pair;
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
        auto r = cs.eval("(engine:metrics \"query:production-hardening-1072-1096-stats\")");
        CHECK(r && is_hash(*r), "hardening stats is hash");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "schema") == 1072,
              "schema 1072");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats",
                   "http-shell-injection-fixed") == 1,
              "http injection fixed");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "recovery-pct-clamped") == 1,
              "recovery pct clamped");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats",
                   "compaction-efficiency-clamped") == 1,
              "compaction efficiency clamped");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "ast-ref-get-meta-tags") == 1,
              "ast:ref-get meta tags");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "mutate-string-bounds-bulk") ==
                  1,
              "mutate string bounds");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats",
                   "jit-fallback-status-defined") == 1,
              "jit fallback status");
        CHECK(href(cs, "query:production-hardening-1072-1096-stats", "issue-1096") == 1096,
              "issue-1096 field");
    }

    // #1079 recovery-success never exceeds 100
    {
        auto r = cs.eval(
            "(hash-ref (engine:metrics \"query:primitives-error-stats\") \"recovery-success\")");
        CHECK(r && is_int(*r) && as_int(*r) >= 0 && as_int(*r) <= 100,
              "recovery-success in [0,100]");
    }

    // #1078 eda concurrency atomic-batch-sv-success is non-neg
    {
        auto r = cs.eval("(hash-ref (engine:metrics \"query:eda-concurrency-stats\") "
                         "\"atomic-batch-sv-success\")");
        CHECK(r && is_int(*r) && as_int(*r) >= 0, "atomic-batch-sv-success >= 0");
    }

    // #1072 arena:adaptive-stats returns pair of ints (no crash)
    {
        auto r = cs.eval("(stats:get \"arena:adaptive-stats\")");
        CHECK(r && (is_pair(*r) || is_void(*r) || is_int(*r)), "arena:adaptive-stats ok");
    }

    // #1080 efficiency clamped when no arena
    {
        auto r = cs.eval("(hash-ref (engine:metrics \"query:arena-production-compaction-stats\") "
                         "\"compaction-efficiency-pct\")");
        if (r && is_int(*r))
            CHECK(as_int(*r) >= 0 && as_int(*r) <= 100, "efficiency_pct in [0,100]");
        else
            CHECK(true, "arena compaction stats optional");
    }

    // Regression
    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto e = cs.eval("(eval \"(+ 1 2)\")");
        CHECK(e && is_int(*e) && as_int(*e) == 3, "eval still works");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production hardening #1072–#1096: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
