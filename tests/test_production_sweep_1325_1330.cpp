// test_production_sweep_1325_1330.cpp — Issues #1325–#1330 Phase 1
// Architecture: reduce 700+ primitives → ~50 (progressive hooks).

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;

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
    constexpr auto Q = "query:production-sweep-1325-1330-stats";

    {
        auto r = cs.eval(std::format("({})", Q));
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, Q, "schema") == 1325, "schema");
        CHECK(href(cs, Q, "active") == 1, "active");
        // #1325 META
        CHECK(href(cs, Q, "prim-surface-reduction-plan") == 1, "reduction plan (#1325)");
        CHECK(href(cs, Q, "prim-surface-target-count") == 50, "target ~50 primitives");
        CHECK(href(cs, Q, "prim-surface-phases-total") == 5, "5 phases");
        // #1326
        CHECK(href(cs, Q, "write-side-demotion-active") == 1, "write-side demotion (#1326)");
        CHECK(href(cs, Q, "stats-namespace-active") == 1, "stats namespace (#1326)");
        // #1327
        CHECK(href(cs, Q, "agent-service-bridge") == 1, "agent bridge (#1327)");
        // #1328
        CHECK(href(cs, Q, "query-essentials-plan") == 1, "query essentials (#1328)");
        CHECK(href(cs, Q, "query-essentials-keep-count") == 10, "keep ~10 query prims");
        // #1329
        CHECK(href(cs, Q, "stdlib-sys-bindings") == 1, "sys bindings (#1329)");
        // #1330
        CHECK(href(cs, Q, "cap-retrofit-scaffold") == 1, "cap retrofit (#1330)");
        CHECK(href(cs, Q, "cap-capability-constant-count") == 8, "new cap constants");
        CHECK(href(cs, Q, "issue-1330") == 1330, "issue-1330");
    }

    // #1326: stats:dirty-count alias
    {
        auto d = cs.eval("(stats:dirty-count)");
        CHECK(d && is_int(*d) && as_int(*d) >= 0, "stats:dirty-count");
        auto hits = href(cs, Q, "stats-alias-hits");
        CHECK(hits >= 1, "stats alias hits counted");
        auto dep = cs.eval("(stats:deopt-count)");
        CHECK(dep && is_int(*dep) && as_int(*dep) >= 0, "stats:deopt-count");
    }

    // #1326: write-side deprecation counter (call still works outside sandbox)
    {
        auto before = href(cs, Q, "write-side-deprecation-hits");
        auto r = cs.eval("(compile:mark-block-dirty! \"no-such\" 0 0)");
        CHECK(r, "mark-block-dirty still callable (deprecation cycle)");
        auto after = href(cs, Q, "write-side-deprecation-hits");
        CHECK(after > before, "write-side deprecation hit counted");
    }

    // #1327: agent:tick / agent:running?
    {
        auto run = cs.eval("(agent:running?)");
        CHECK(run && is_bool(*run), "agent:running?");
        auto tick = cs.eval("(agent:tick)");
        CHECK(tick, "agent:tick callable");
        auto ticks = href(cs, Q, "agent-tick-total");
        CHECK(ticks >= 1, "agent tick counted");
    }

    // #1329: sys-open / sys-read / sys-write (non-sandbox: allowed)
    {
        // Open /dev/null read-only
        auto fd = cs.eval("(sys-open \"/dev/null\" 0)");
        CHECK(fd && is_int(*fd) && as_int(*fd) >= 0, "sys-open /dev/null");
        auto n = cs.eval(std::format("(sys-read {} 0)", as_int(*fd)));
        CHECK(n && is_string(*n), "sys-read returns string");
        auto opens = href(cs, Q, "sys-open-calls");
        CHECK(opens >= 1, "sys-open counted");
        auto reads = href(cs, Q, "sys-read-calls");
        CHECK(reads >= 1, "sys-read counted");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 21 21)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 21 21)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1325–#1330: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
