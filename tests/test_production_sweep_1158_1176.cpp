// test_production_sweep_1158_1176.cpp — Issues #1158–#1176 Phase 1

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
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1158-1176-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "schema") == 1158, "schema");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "math-int64-ub-fixed") == 1,
              "math ub");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "http-get-no-shell") == 1,
              "http-get");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "git-stage-no-shell") == 1,
              "git-stage");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "issue-1176") == 1176,
              "issue-1176");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "file-path-deny-list") == 1,
              "deny list");
        CHECK(href(cs, "query:production-sweep-1158-1176-stats", "renderer-module-scaffold") == 1,
              "renderer scaffold");
    }

    // #1158: abs of negative fixnums (safe magnitude — tagged fixnums are
    // n<<1 so full INT64_MIN is not representable end-to-end; C++ path still
    // saturates INT64_MIN via safe_abs_i64).
    {
        auto r2 = cs.eval("(abs -42)");
        CHECK(r2 && is_int(*r2) && as_int(*r2) == 42, "abs -42");
        auto r3 = cs.eval("(abs -1000000000000)");
        CHECK(r3 && is_int(*r3) && as_int(*r3) == 1000000000000LL, "abs large neg");
        auto r4 = cs.eval("(abs 0)");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 0, "abs 0");
    }

    // #1159/#1174: quotient / modulo / gcd / lcm safe paths
    {
        auto r = cs.eval("(quotient -1000000000000 -1)");
        CHECK(r && is_int(*r) && as_int(*r) == 1000000000000LL, "quotient large / -1");
        auto r2 = cs.eval("(quotient 10 0)");
        CHECK(r2 && (is_error(*r2) || is_void(*r2)), "quotient /0 is error");
        auto r3 = cs.eval("(modulo 10 3)");
        CHECK(r3 && is_int(*r3) && as_int(*r3) == 1, "modulo 10 3");
        auto r4 = cs.eval("(gcd 12 8)");
        CHECK(r4 && is_int(*r4) && as_int(*r4) == 4, "gcd 12 8");
        auto r5 = cs.eval("(lcm 4 6)");
        CHECK(r5 && is_int(*r5) && as_int(*r5) == 12, "lcm 4 6");
    }

    // #1163 deny write to /proc/self/mem
    {
        auto r = cs.eval(R"((write-file "/proc/self/mem" "x"))");
        CHECK(r && is_void(*r), "write-file /proc/self/mem → void");
    }

    // #1160 http-get: returns without shell crash
    {
        auto r = cs.eval("(http-get \"http://example.com/x\")");
        CHECK(r, "http-get returns");
    }

    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1158–#1176: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
