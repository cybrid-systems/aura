// test_production_sweep_1276_1280.cpp — Issues #1276–#1280 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.pass_manager;
import aura.compiler.ir;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::BasicBlock;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IROpcode;

namespace {

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref ({}) \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Diamond CFG: A → B, A → C, B → D, C → D (no loop).
// Pre-#1278 is_inlinable_branch_aware returned false (false positive).
IRFunction make_diamond_func() {
    IRFunction f;
    f.name = "diamond";
    f.blocks.resize(4);
    for (std::uint32_t i = 0; i < 4; ++i)
        f.blocks[i].id = i;
    // A: Branch cond → B (true), C (false)
    f.blocks[0].instructions.push_back({IROpcode::Branch, {0, 1, 2}});
    // B: Jump → D
    f.blocks[1].instructions.push_back({IROpcode::Jump, {3}});
    // C: Jump → D
    f.blocks[2].instructions.push_back({IROpcode::Jump, {3}});
    // D: Return
    f.blocks[3].instructions.push_back({IROpcode::Return, {0}});
    return f;
}

} // namespace

int main() {
    CompilerService cs;

    {
        auto r = cs.eval("(engine:metrics \"query:production-sweep-1276-1280-stats\")");
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats", "schema") == 1276, "schema");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats", "active") == 1, "active");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats",
                   "reflect-nested-struct-scaffold") == 1,
              "reflect nested (#1276)");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats",
                   "hygiene-violation-stats-active") == 1,
              "hygiene stats (#1277)");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats", "inline-diamond-cfg-fixed") == 1,
              "diamond cfg fixed (#1278)");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats",
                   "stable-ref-auto-refresh-enforced") == 1,
              "stable-ref auto-refresh (#1279)");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats", "pattern-hygiene-end-to-end") == 1,
              "pattern hygiene (#1280)");
        CHECK(href(cs, "query:production-sweep-1276-1280-stats", "issue-1280") == 1280,
              "issue-1280");
    }

    // #1276: reflect-schema-stats primitive
    {
        auto r = cs.eval("(engine:metrics \"query:reflect-schema-stats\")");
        CHECK(r && is_int(*r) && as_int(*r) == 1276, "query:reflect-schema-stats == 1276");
    }

    // #1278: diamond CFG is inlinable (no false-positive loop)
    {
        auto diamond = make_diamond_func();
        // Public test hook if available
        const bool ok = aura::compiler::InlinePass::is_inlinable_branch_aware_for_test(diamond);
        CHECK(ok, "diamond CFG is_inlinable_branch_aware (no false loop)");
    }

    // #1277: existing hygiene-violation-stats / dirty-impact stay reachable
    {
        auto h = cs.eval("(engine:metrics \"query:hygiene-violation-stats\")");
        CHECK(h.has_value(), "query:hygiene-violation-stats callable");
        auto d = cs.eval("(query:dirty-impact)");
        CHECK(d.has_value(), "query:dirty-impact callable");
    }

    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1276–#1280: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
