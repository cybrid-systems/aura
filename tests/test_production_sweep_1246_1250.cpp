// test_production_sweep_1246_1250.cpp — Issues #1246–#1250 Phase 1

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
        auto r = cs.eval("(query:production-sweep-1246-1250-stats)");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1246-1250-stats", "schema") == 1246, "schema");
        CHECK(href(cs, "query:production-sweep-1246-1250-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1246-1250-stats", "runtime-reflect-bridge-guard") ==
                  1,
              "runtime reflect bridge (#1246)");
        CHECK(href(cs, "query:production-sweep-1246-1250-stats",
                   "agent-string-heap-bounds-hardened") == 1,
              "agent string_heap bounds (#1249)");
        CHECK(href(cs, "query:production-sweep-1246-1250-stats", "stable-ref-full-path-enforced") ==
                  1,
              "stable-ref full path (#1250)");
        CHECK(href(cs, "query:production-sweep-1246-1250-stats", "issue-1250") == 1250,
              "issue-1250");
    }

    // #1249: agent strategy primitives must not crash on normal string args
    // (bounds-hardened heap_str path).
    {
        auto d = cs.eval(R"((define-strategy "s1249" "body"))");
        CHECK(d && aura::compiler::types::is_bool(*d), "define-strategy returns bool");
        auto f = cs.eval(R"((strategy-field "s1249" "body"))");
        CHECK(f && aura::compiler::types::is_string(*f), "strategy-field body is string");
        auto set = cs.eval(R"((strategy-set-field! "s1249" "max-attempts" 5))");
        CHECK(set && aura::compiler::types::is_bool(*set) && aura::compiler::types::as_bool(*set),
              "strategy-set-field! max-attempts");
    }

    // Smoke: basic eval still works after agent primitives.
    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1246–#1250: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
