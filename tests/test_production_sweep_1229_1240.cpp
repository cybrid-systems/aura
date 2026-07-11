// test_production_sweep_1229_1240.cpp — Issues #1229–#1240 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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
        auto r = cs.eval("(query:production-sweep-1229-1240-stats)");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1229-1240-stats", "schema") == 1229, "schema");
        CHECK(href(cs, "query:production-sweep-1229-1240-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1229-1240-stats", "agent-capability-gates") == 1,
              "agent caps");
        CHECK(href(cs, "query:production-sweep-1229-1240-stats", "synthesize-json-escape-fixed") ==
                  1,
              "json escape");
        CHECK(href(cs, "query:production-sweep-1229-1240-stats", "ffi-opaque-tracking-hardened") ==
                  1,
              "ffi opaque");
        CHECK(href(cs, "query:production-sweep-1229-1240-stats", "value-tag-consteval-contracts") ==
                  1,
              "value tags");
        CHECK(href(cs, "query:production-sweep-1229-1240-stats", "issue-1240") == 1240,
              "issue-1240");
    }

    // #1231: stdlib dashboard surfaces EDA/FFI keys
    {
        auto r = cs.eval("(query:stdlib-production-review-stats)");
        CHECK(r && is_hash(*r), "stdlib review is hash");
        CHECK(href(cs, "query:stdlib-production-review-stats", "eda-parse-total") >= 0,
              "eda-parse-total key");
        CHECK(href(cs, "query:stdlib-production-review-stats", "eda-hash-creates") >= 0,
              "eda-hash-creates key");
        CHECK(href(cs, "query:stdlib-production-review-stats", "ffi-opaque-tracking") == 1,
              "ffi-opaque-tracking");
    }

    // #1230: ffi:opaque-stats returns int count
    {
        auto r = cs.eval("(ffi:opaque-stats)");
        CHECK(r && is_int(*r) && as_int(*r) >= 0, "ffi:opaque-stats count");
    }

    // #1232: without sandbox, agent primitives still work (gate only when sandbox on)
    {
        auto r = cs.eval("(auto-evolve-running?)");
        CHECK(r, "auto-evolve-running? returns");
    }

    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1229–#1240: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
