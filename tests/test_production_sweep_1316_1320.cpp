// test_production_sweep_1316_1320.cpp — Issues #1316–#1320 Phase 1

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
    constexpr auto Q = "query:production-sweep-1316-1320-stats";

    {
        auto r = cs.eval(std::format("({})", Q));
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, Q, "schema") == 1316, "schema");
        CHECK(href(cs, Q, "active") == 1, "active");
        CHECK(href(cs, Q, "render-stable-hot-path") == 1, "render stable hot path (#1316)");
        CHECK(href(cs, Q, "render-primitive-meta") == 1, "render primitive meta (#1317)");
        CHECK(href(cs, Q, "ir-soa-migration-phase2") == 1, "ir soa phase2 (#1318)");
        CHECK(href(cs, Q, "gap-buffer-structural-mutate-active") == 1, "gap buffer active (#1319)");
        CHECK(href(cs, Q, "arena-live-defrag-policy") == 1, "defrag policy (#1320)");
        CHECK(href(cs, Q, "issue-1320") == 1320, "issue-1320");
        CHECK(href(cs, Q, "render-deopt-throttle-window-ms") == 500, "deopt window 500ms");
    }

    // #1316: deopt throttle — first probe applies, rapid second is throttled
    {
        auto a1 = cs.eval("(render-jit-deopt-probe)");
        CHECK(a1 && is_int(*a1) && as_int(*a1) >= 1, "first deopt probe applies");
        auto a2 = cs.eval("(render-jit-deopt-probe)");
        CHECK(a2 && is_int(*a2), "second deopt probe returns int");
        auto throttled = href(cs, Q, "render-jit-deopt-throttled");
        CHECK(throttled >= 1, "second probe throttled within 500ms");
        auto st = cs.eval("(query:render-jit-stability-stats)");
        CHECK(st && is_string(*st), "query:render-jit-stability-stats string");
    }

    // #1317: terminal primitives + obs queries
    {
        auto id = cs.eval("(make-terminal-buffer 3 2)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        auto bid = as_int(*id);
        auto ok = cs.eval(std::format("(terminal-set-cell {} 0 0 65)", bid));
        CHECK(ok && is_bool(*ok), "terminal-set-cell");
        auto id2 = cs.eval("(make-terminal-buffer 3 2)");
        CHECK(id2 && is_int(*id2), "second buffer");
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 66)", as_int(*id2)));
        auto diff = cs.eval(std::format("(terminal-diff-update {} {})", bid, as_int(*id2)));
        CHECK(diff && is_int(*diff) && as_int(*diff) >= 1, "terminal-diff-update");
        auto tds = cs.eval("(query:terminal-diff-stats)");
        CHECK(tds && is_string(*tds), "query:terminal-diff-stats");
        auto present = cs.eval(std::format("(terminal-present-batch {})", bid));
        CHECK(present && is_int(*present) && as_int(*present) >= 0, "terminal-present-batch");
        auto aot = href(cs, Q, "render-jit-aot-prefer-hits");
        CHECK(aot >= 1, "present samples aot prefer");
        auto obs = href(cs, Q, "render-obs-query-hits");
        CHECK(obs >= 1, "render obs query hits");
    }

    // #1318: dual-emit bridge counter is non-negative (may be 0 if no lower yet)
    {
        auto dual = href(cs, Q, "ir-soa-dual-emit-bridge-count");
        CHECK(dual >= 0, "dual-emit bridge count readable");
        // Force a compile/eval that may lower:
        auto r = cs.eval("(+ 1 2)");
        CHECK(r && is_int(*r) && as_int(*r) == 3, "(+ 1 2)");
    }

    // #1319: gap-buffer structural mutate demo
    {
        auto before = href(cs, Q, "gap-buffer-structural-mutate-hits");
        auto sz = cs.eval("(gap-buffer-structural-mutate-demo 32)");
        CHECK(sz && is_int(*sz) && as_int(*sz) >= 0, "gap-buffer-structural-mutate-demo");
        auto after = href(cs, Q, "gap-buffer-structural-mutate-hits");
        CHECK(after > before, "gap buffer hits increased");
        auto inserts = href(cs, Q, "gap-buffer-insert-total");
        CHECK(inserts >= 1, "gap buffer inserts counted");
    }

    // #1320: arena:defrag-now + stats
    {
        auto d = cs.eval("(arena:defrag-now)");
        CHECK(d && is_int(*d) && as_int(*d) >= 0, "arena:defrag-now");
        auto calls = href(cs, Q, "arena-defrag-now-calls");
        CHECK(calls >= 1, "defrag-now calls counted");
        auto attempted = href(cs, Q, "arena-defrag-attempted-total");
        CHECK(attempted >= 1, "defrag attempted counted");
        auto reset = cs.eval("(arena-render-frame-reset)");
        CHECK(reset && is_int(*reset) && as_int(*reset) >= 0, "arena-render-frame-reset");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(* 6 7)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(* 6 7)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1316–#1320: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
