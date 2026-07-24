// test_production_safety.cpp — Merged #1047-#1071 + #1097-#1122 (#1978).
//
// Originally test_production_safety_1047_1071.cpp +
// test_production_safety_1097_1122.cpp. Both are Phase 1
// production safety Wave2 scaffolding (schema flag checks
// via test_production_sweep target). Merged with both
// AC sets preserved verbatim.
//
// AC list (all preserved; each section cites original issue#):
//   Issue #1047-#1071 (test_production_safety_1047_1071.cpp):
//     schema 1047 + produce+safety path
//   Issue #1097-#1122 (test_production_safety_1097_1122.cpp):
//     schema 1097 + safety matrix

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

    // ── #1047–#1071 ────────────────────────────────────────────
    {
        auto r = cs.eval("(engine:metrics \"query:production-safety-1047-1071-stats\")");
        CHECK(r && is_hash(*r), "safety stats is hash");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "schema") == 1047, "schema 1047");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "active") == 1, "active");
        // #1050/#1054/#1071 — flat string-key kv hash observability
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "string-key-kv-active") == 1,
              "string-key kv active");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "dedup-string-fixed") == 1,
              "dedup string fixed");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "ref-get-meta-include-fixed") ==
                  1,
              "ref-get meta include");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "fmt-arity-fixed") == 1,
              "fmt arity fixed");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "issue-1071") == 1071,
              "issue-1071 field");
    }

    // #1050 string-key kv hash
    {
        auto r = cs.eval("(hash-ref (build_kv_hash \"safety\" \"v1.0\") \"safety_field\")");
        CHECK(r && is_int(*r) && as_int(*r) >= 0, "string-key kv returns int");
        auto present = cs.eval("(hash-has-key? (build_kv_hash \"safety\" \"v1.0\") \"k\")");
        CHECK(present && is_bool(*present), "hash-has-key? returns bool");
    }

    // #1054 fmt arity guards
    {
        auto r = cs.eval("(format)");
        CHECK(r && is_void(*r), "format no-arg → void");
        auto r2 = cs.eval("(format 123)");
        CHECK(r2 && is_void(*r2), "format non-string → void");
    }

    // #1071 ref-get meta include
    {
        auto r = cs.eval("(ref-get-meta (lambda (x) x) \"k\")");
        CHECK(r && is_int(*r), "ref-get-meta returns int");
    }

    // Regression
    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto e = cs.eval("(eval \"(+ 1 2)\")");
        CHECK(e && is_int(*e) && as_int(*e) == 3, "eval still works");
    }

    // ── #1097–#1122 ────────────────────────────────────────────
    {
        auto r = cs.eval("(engine:metrics \"query:production-safety-1097-1122-stats\")");
        CHECK(r && is_hash(*r), "safety stats is hash");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "schema") == 1097, "schema 1097");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "shape-arity-bounds-clamped") ==
                  1,
              "shape arity bounds clamped");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "sv-feedback-corrected") == 1,
              "sv feedback corrected");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "jit-fallback-status-clean") == 1,
              "jit fallback clean");
        CHECK(href(cs, "query:production-safety-1097-1122-stats", "issue-1122") == 1122,
              "issue-1122 field");
    }

    // #1104 shape arity guard
    {
        auto r = cs.eval("(shape:arity-clamp 5 8)");
        CHECK(r && is_int(*r) && as_int(*r) >= 0, "shape:arity-clamp returns int");
    }

    // Regression
    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto e = cs.eval("(eval \"(+ 1 2)\")");
        CHECK(e && is_int(*e) && as_int(*e) == 3, "eval still works");
    }

    std::println("=== production safety #1047-#1122: OK ({} passed) ===", ::aura::test::g_passed);
    return ::aura::test::g_failed ? 1 : 0;
}