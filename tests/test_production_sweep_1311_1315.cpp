// test_production_sweep_1311_1315.cpp — Issues #1311–#1315 Phase 1

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

    {
        auto r = cs.eval("(query:production-sweep-1311-1315-stats)");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1311-1315-stats", "schema") == 1311, "schema");
        CHECK(href(cs, "query:production-sweep-1311-1315-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1311-1315-stats", "cow-boundary-pins-mutex") == 1,
              "cow pins mutex (#1311)");
        CHECK(href(cs, "query:production-sweep-1311-1315-stats", "jit-runtime-setters-locked") == 1,
              "jit setters (#1312)");
        CHECK(href(cs, "query:production-sweep-1311-1315-stats", "issue-1315") == 1315,
              "issue-1315");
    }

    // #1313: terminal buffer + set-cell + diff
    {
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make-terminal-buffer");
        auto bid = as_int(*id);
        auto ok = cs.eval(std::format("(terminal-set-cell {} 0 0 65)", bid));
        CHECK(ok && is_bool(*ok), "terminal-set-cell");
        auto id2 = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id2 && is_int(*id2), "second buffer");
        auto bid2 = as_int(*id2);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 66)", bid2));
        auto diff = cs.eval(std::format("(terminal-diff-update {} {})", bid, bid2));
        CHECK(diff && is_int(*diff) && as_int(*diff) >= 1, "terminal-diff-update changed");
        auto creates =
            href(cs, "query:production-sweep-1311-1315-stats", "terminal-buffer-creates");
        CHECK(creates >= 2, "terminal buffer creates counted");
    }

    // #1314: present-batch writes bytes
    {
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        CHECK(id && is_int(*id), "buf for present");
        auto n = cs.eval(std::format("(terminal-present-batch {})", as_int(*id)));
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "terminal-present-batch");
        auto samples = href(cs, "query:production-sweep-1311-1315-stats", "render-hotpath-samples");
        CHECK(samples >= 1, "render hotpath sampled");
    }

    // #1315: arena-render-frame-reset + stats
    {
        auto r = cs.eval("(arena-render-frame-reset)");
        CHECK(r && is_int(*r) && as_int(*r) >= 0, "arena-render-frame-reset");
        auto st = cs.eval("(query:render-arena-frame-stats)");
        CHECK(st && is_string(*st), "query:render-arena-frame-stats string");
        auto resets =
            href(cs, "query:production-sweep-1311-1315-stats", "render-frame-reset-total");
        CHECK(resets >= 1, "render frame reset counted");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1311–#1315: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
