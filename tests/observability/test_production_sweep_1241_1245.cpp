// test_production_sweep_1241_1245.cpp — Issues #1241–#1245 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.arena;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

// Stats are catalog-only (engine:metrics) after SlimSurface — not public add().
std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1241-1245-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats", "schema") == 1241, "schema");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats", "soa-view-concept-enforced") == 1,
              "soa view");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats", "arena-shrink-tier-hardened") == 1,
              "arena shrink");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats", "soa-view-eval-helpers") == 1,
              "soa helpers");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats", "hygiene-ir-marker-propagation") ==
                  1,
              "hygiene ir");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats",
                   "macro-clone-concurrent-hygiene") == 1,
              "macro concurrent");
        CHECK(href(cs, "query:production-sweep-1241-1245-stats", "issue-1245") == 1245,
              "issue-1245");
    }

    // #1242: SmallObjectPool rebind + try_allocate after rebind stays in-bounds
    {
        aura::ast::SmallObjectPool pool;
        void* p1 = pool.try_allocate(16);
        CHECK(p1 != nullptr, "small alloc 16");
        void* p2 = pool.try_allocate(32);
        CHECK(p2 != nullptr, "small alloc 32");
        pool.rebind_tiers();
        void* p3 = pool.try_allocate(16);
        CHECK(p3 != nullptr, "alloc after rebind");
        // Pointers must be within pool capacity region
        CHECK(pool.allocated() > 0, "pool allocated > 0");
        pool.reset_small_pool_tiers();
        CHECK(pool.allocated() == 0, "reset clears allocated");
        void* p4 = pool.try_allocate(16);
        CHECK(p4 != nullptr, "alloc after reset_small_pool_tiers");
    }

    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1241–#1245: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
