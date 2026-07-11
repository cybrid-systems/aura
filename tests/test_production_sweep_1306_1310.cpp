// test_production_sweep_1306_1310.cpp — Issues #1306–#1310 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern "C" {
std::int64_t aura_alloc_string(const char* s);
const char* aura_string_ref(std::int64_t val);
std::int64_t aura_alloc_float(double d);
double aura_float_ref(std::int64_t val);
void aura_reset_runtime();
}

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
        auto r = cs.eval("(query:production-sweep-1306-1310-stats)");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "schema") == 1306, "schema");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "jit-string-pool-mutex") == 1,
              "string pool mutex (#1306)");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "jit-float-pool-mutex") == 1,
              "float pool mutex (#1307)");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "jit-last-module-aot-lock") == 1,
              "last_module AOT lock (#1308)");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "jit-closure-is-arena-flag") == 1,
              "is_arena flag (#1309)");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "jit-arena-env-free-on-reset") ==
                  1,
              "env free on reset (#1310)");
        CHECK(href(cs, "query:production-sweep-1306-1310-stats", "issue-1310") == 1310,
              "issue-1310");
    }

    // #1306/#1307: concurrent string/float alloc + ref does not crash
    {
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([t] {
                for (int i = 0; i < 80; ++i) {
                    auto s = aura_alloc_string(std::format("s{}_{}", t, i).c_str());
                    (void)aura_string_ref(s);
                    auto f = aura_alloc_float(static_cast<double>(t * 100 + i));
                    (void)aura_float_ref(f);
                }
            });
        }
        for (auto& th : threads)
            th.join();
        auto s = aura_alloc_string("final");
        auto* p = aura_string_ref(s);
        CHECK(p && std::string(p) == "final", "string pool after concurrent (#1306)");
        auto f = aura_alloc_float(3.5);
        CHECK(aura_float_ref(f) == 3.5, "float pool after concurrent (#1307)");
    }

    // #1310: reset frees arena envs without crash
    {
        aura_reset_runtime();
        CHECK(true, "aura_reset_runtime after pools (#1310)");
        // Re-alloc after reset still works
        auto s = aura_alloc_string("after-reset");
        CHECK(aura_string_ref(s) && std::string(aura_string_ref(s)) == "after-reset",
              "string after reset");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1306–#1310: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
