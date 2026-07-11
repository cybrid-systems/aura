// test_production_sweep_1123_1140.cpp — Issues #1123–#1143 Phase 1 (all remaining open)

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

    {
        auto r = cs.eval("(query:production-sweep-1123-1140-stats)");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1123-1140-stats", "schema") == 1123, "schema");
        CHECK(href(cs, "query:production-sweep-1123-1140-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1123-1140-stats", "equal-zero-nil-fixed") == 1,
              "equal fixed");
        CHECK(href(cs, "query:production-sweep-1123-1140-stats", "format-void-on-error") == 1,
              "format void");
        CHECK(href(cs, "query:production-sweep-1123-1140-stats",
                   "term-metric-double-count-fixed") == 1,
              "term metrics");
        CHECK(href(cs, "query:production-sweep-1123-1140-stats", "issue-1140") == 1140,
              "issue-1140");
    }

    // #1137 — empty list is void; fixnum 0 is not equal to nil
    {
        auto r = cs.eval("(equal? 0 '())");
        CHECK(r && is_bool(*r) && !as_bool(*r), "equal? 0 '() is #f");
        auto r2 = cs.eval("(equal? 0 0)");
        CHECK(r2 && is_bool(*r2) && as_bool(*r2), "equal? 0 0 is #t");
        auto r3 = cs.eval("(equal? '() '())");
        CHECK(r3 && is_bool(*r3) && as_bool(*r3), "equal? '() '() is #t");
        auto r4 = cs.eval("(eq? 0 '())");
        CHECK(r4 && is_bool(*r4) && !as_bool(*r4), "eq? 0 '() is #f");
        auto r5 = cs.eval("((lambda (x y) (equal? x y)) 0 '())");
        CHECK(r5 && is_bool(*r5) && !as_bool(*r5), "equal? via lambda 0 '() is #f");
    }

    // #1138
    {
        auto r = cs.eval("(format)");
        CHECK(r && is_void(*r), "format no-arg → void");
        auto r2 = cs.eval("(format 123)");
        CHECK(r2 && is_void(*r2), "format non-string → void");
    }

    // #1135/#1136/#1140 — no-ops deprecated in #1351 (still bool, now #f)
    {
        CHECK(cs.eval("(terminal:present)") && is_bool(*cs.eval("(terminal:present)")),
              "terminal:present");
        auto b = cs.eval("(terminal:present-delta)");
        CHECK(b && is_bool(*b) && !as_bool(*b), "terminal:present-delta deprecated → #f");
        auto c = cs.eval("(terminal:create-buffer)");
        CHECK(c && is_bool(*c) && !as_bool(*c), "terminal:create-buffer deprecated → #f");
        auto d = cs.eval("(terminal:diff)");
        CHECK(d && is_bool(*d) && !as_bool(*d), "terminal:diff deprecated → #f");
    }

    // #1139
    {
        auto r = cs.eval("(runtime:self-heal-on-drift)");
        CHECK(r && is_bool(*r), "self-heal returns bool");
    }

    {
        auto a = cs.eval("(+ 40 2)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 40 2)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1123–#1143: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
