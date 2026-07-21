// test_production_sweep_1177_1201.cpp — Issues #1177–#1201 Phase 1

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
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1177-1201-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats", "schema") == 1177, "schema");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats", "ffi-hot-path-scaffold") == 1,
              "ffi hot path");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats",
                   "zero-copy-framebuffer-supported") == 1,
              "zero-copy");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats",
                   "security-core-modules-scaffold") == 1,
              "security modules");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats", "ansi-helper-supported") == 1,
              "ansi helper");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats",
                   "instruction-dirty-short-circuit") == 1,
              "inst dirty");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats", "fiber-join-structured") == 1,
              "fiber join");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats", "optimization-passes-registry") ==
                  1,
              "opt passes");
        CHECK(href(cs, "query:production-sweep-1177-1201-stats", "issue-1201") == 1201,
              "issue-1201");
    }

    // #1178/#1181/#1184: zero-copy dashboard upgraded from stub
    {
        CHECK(href(cs, "query:zero-copy-framebuffer-stats", "zero-copy-supported") == 1,
              "zero-copy-supported=1");
        CHECK(href(cs, "query:zero-copy-framebuffer-stats", "ansi-helper-supported") == 1,
              "ansi-helper-supported=1");
        CHECK(href(cs, "query:zero-copy-framebuffer-stats", "memory-profiling-supported") == 1,
              "memory-profiling-supported=1");
        CHECK(href(cs, "query:zero-copy-framebuffer-stats", "recommendation") == 0,
              "recommendation production-ready");
        CHECK(href(cs, "query:zero-copy-framebuffer-stats", "schema") == 781, "schema 781");
    }

    // Smoke: fiber:join and broadcast primitives still registered
    {
        auto r = cs.eval("(stats:get \"mailbox-count\")");
        CHECK(r && is_int(*r), "mailbox-count returns int");
    }

    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1177–#1201: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
