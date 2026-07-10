// test_production_safety_1097_1122.cpp — Issues #1097–#1122 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "reflect/cache_format.h"

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
        auto r = cs.eval("(query:production-safety-1097-1122-stats)");
        CHECK(r && is_hash(*r), "safety stats is hash");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "schema") == 1097, "schema 1097");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "eval-async-heap-result") == 1,
              "eval async heap");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "const-fold-bool-tag-fixed") == 1,
              "const fold bool tag");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "const-fold-block-clear") == 1,
              "const fold block clear");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "reflect-bounds-checks") == 1,
              "reflect bounds");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "cache-header-validate-ext") == 1,
              "cache header validate");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "open-cache-ir-bounds") == 1,
              "open cache ir bounds");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "schema-unknown-is-object") == 1,
              "schema unknown is object");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "issue-1122") == 1122,
              "issue-1122 field");
    }

    // #1104: wild header fails extended validation
    {
        CacheHeader bad{};
        std::memcpy(bad.magic, "AURACACH", 8);
        bad.version = 5;
        bad.num_nodes = 1;
        bad.node_offset = 64;
        bad.num_strings = 0xFFFFFFFFu;
        CHECK(cache_validate_header(&bad) != 0, "wild num_strings rejected");

        CacheHeader okh{};
        std::memcpy(okh.magic, "AURACACH", 8);
        okh.version = 5;
        okh.num_nodes = 10;
        okh.node_offset = 64;
        okh.num_strings = 5;
        okh.num_functions = 1;
        okh.string_offset = 128;
        CHECK(cache_validate_header(&okh) == 0, "sane header accepted");
    }

    // Arithmetic / fold regression (bool-tag fix must not break +)
    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto e = cs.eval("(eval \"(+ 1 2)\")");
        CHECK(e && is_int(*e) && as_int(*e) == 3, "eval works");
        // Fold path: 1+2=3 must remain integer 3 (not confusable with old #f tag)
        auto t = cs.eval("(+ 1 2)");
        CHECK(t && is_int(*t) && as_int(*t) == 3, "1+2=3 pure int");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production safety #1097–#1122: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
