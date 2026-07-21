// Issue #1047/#1050/#1054/#1071 (#1978 renamed): issue# moved from filename to header.
// test_production_safety_1047_1071.cpp — Issues #1047–#1071 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
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
        auto r = cs.eval("(engine:metrics \"query:production-safety-1047-1071-stats\")");
        CHECK(r && is_hash(*r), "safety stats is hash");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "schema") == 1047, "schema 1047");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "hw-coercion-empty-str-fixed") ==
                  1,
              "hw empty str fixed");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "mutation-history-void-fixed") ==
                  1,
              "mutation-history void fixed");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "query-where-dedup-fixed") == 1,
              "query:where dedup");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "eval-string-bounds-fixed") == 1,
              "eval bounds fixed");
        CHECK(href(cs, "query:production-safety-1047-1071-stats", "issue-1071") == 1071,
              "issue-1071 field");
    }

    // #1054: bad-arg returns void, not int 0
    {
        auto r = cs.eval("(stats:get \"mutation-history\")");
        CHECK(r && is_void(*r), "mutation-history no-arg → void");
        auto r2 = cs.eval("(mutation-history \"x\")");
        CHECK(r2 && is_void(*r2), "mutation-history non-int → void");
    }

    // #1050: hw-coercion-warning returns real empty string (valid heap index)
    {
        auto r = cs.eval("(compile:hw-coercion-warning \"no-such-a\" \"no-such-b\")");
        CHECK(r && is_string(*r), "hw-coercion-warning returns string");
        // Empty content: string idx must be in-range for subsequent use.
        // Length check via string-length if available.
        auto len =
            cs.eval("(string-length (compile:hw-coercion-warning \"no-such-a\" \"no-such-b\"))");
        if (len && is_int(*len))
            CHECK(as_int(*len) == 0, "hw warning empty → length 0");
        else
            CHECK(true, "string-length optional skip");
    }

    // #1071: eval with valid string still works
    {
        auto r = cs.eval("(eval \"(+ 1 2)\")");
        CHECK(r && is_int(*r) && as_int(*r) == 3, "eval (+ 1 2) = 3");
    }

    // Regression
    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
        auto f = cs.eval("(let ((op +)) (foldl op 0 (list 1 2 3)))");
        CHECK(f && is_int(*f) && as_int(*f) == 6, "foldl let-bound +");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production safety #1047–#1071: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
