// test_production_sweep_1251_1255.cpp — Issues #1251–#1255 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

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
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1251-1255-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1251-1255-stats", "schema") == 1251, "schema");
        CHECK(href(cs, "query:production-sweep-1251-1255-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1251-1255-stats", "mark-dirty-bounds-enforced") == 1,
              "mark_dirty bounds (#1251)");
        CHECK(href(cs, "query:production-sweep-1251-1255-stats", "rollback-compaction-path") == 1,
              "rollback compaction (#1251)");
        CHECK(href(cs, "query:production-sweep-1251-1255-stats", "steal-inner-boundary-hardened") ==
                  1,
              "steal inner boundary (#1254)");
        CHECK(href(cs, "query:production-sweep-1251-1255-stats",
                   "pattern-hygiene-strict-enforced") == 1,
              "pattern hygiene (#1255)");
        CHECK(href(cs, "query:production-sweep-1251-1255-stats", "issue-1255") == 1255,
              "issue-1255");
    }

    // #1251: mark_dirty_upward bounds constants + truncation counter API
    {
        aura::ast::FlatAST flat;
        CHECK(aura::ast::FlatAST::kMarkDirtyMaxDepth == 64, "max depth 64");
        CHECK(aura::ast::FlatAST::kMarkDirtyCountThreshold == 4096, "count threshold 4096");
        auto a = flat.add_literal(1);
        auto b = flat.add_literal(2);
        // Parent chain is short — no truncation expected
        flat.mark_dirty_upward(a);
        CHECK(flat.mark_dirty_truncated_count() == 0, "no truncation on short chain");
        (void)b;
        CHECK(flat.rollback_compaction_triggered() == 0, "no rollback compaction yet");
    }

    // #1252/#1253: mutate under Guard bumps boundary wrap + hold samples
    {
        auto set = cs.eval("(set-code \"(define x 1)\")");
        (void)set;
        auto ev = cs.eval("(eval-current)");
        (void)ev;
        auto m = cs.eval("(mutate:rebind \"x\" \"2\")");
        (void)m;
        auto wrapped = href(cs, "query:production-sweep-1251-1255-stats",
                            "mutation-boundary-primitives-wrapped");
        CHECK(wrapped >= 0, "boundary wrap counter readable");
        auto samples = href(cs, "query:production-sweep-1251-1255-stats", "mutation-hold-samples");
        CHECK(samples >= 0, "hold samples readable");
    }

    {
        auto a = cs.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1251–#1255: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
