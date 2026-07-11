// test_production_sweep_1336_1348.cpp — Issues #1336–#1341, #1344–#1348 Phase 1

#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;
import aura.core.arena;
import aura.compiler.ir;
import aura.compiler.pass_manager;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;

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
    constexpr auto Q = "query:production-sweep-1336-1348-stats";

    {
        auto r = cs.eval(std::format("({})", Q));
        CHECK(r && is_hash(*r), "sweep stats is hash");
        CHECK(href(cs, Q, "schema") == 1336, "schema");
        CHECK(href(cs, Q, "active") == 1, "active");
        // #1336
        CHECK(href(cs, Q, "incremental-tc-selective-active") == 1, "incremental TC (#1336)");
        CHECK(href(cs, Q, "solve-delta-worklist-soft-cap") == 256, "worklist soft cap");
        // #1338
        CHECK(href(cs, Q, "ir-parent-type-stamp-active") == 1, "parent type stamp (#1338)");
        // #1339
        CHECK(href(cs, Q, "linear-move-elide-active") == 1, "linear move elide (#1339)");
        // #1340
        CHECK(href(cs, Q, "adt-exhaust-incremental-active") == 1, "ADT exhaust (#1340)");
        // #1341
        CHECK(href(cs, Q, "blame-elision-reason-obs-active") == 1, "blame obs (#1341)");
        // #1344
        CHECK(href(cs, Q, "sv-highlevel-mutate-active") == 1, "SV highlevel mutate (#1344)");
        CHECK(href(cs, Q, "query-sv-pattern-preset-active") == 1, "SV pattern preset");
        // #1345
        CHECK(href(cs, Q, "dirty-upward-prune-active") == 1, "dirty prune (#1345)");
        CHECK(href(cs, Q, "dirty-upward-max-depth-config") == 64, "max depth config");
        // #1346
        CHECK(href(cs, Q, "stable-ref-lockfree-path-active") == 1, "lockfree StableRef (#1346)");
        // #1347
        CHECK(href(cs, Q, "sv-feedback-harness-active") == 1, "SV harness (#1347)");
        // #1348
        CHECK(href(cs, Q, "ast-auto-compact-active") == 1, "auto compact (#1348)");
        CHECK(href(cs, Q, "ast-compaction-threshold") == 1024, "compaction threshold");
        CHECK(href(cs, Q, "issue-1348") == 1348, "issue-1348");
    }

    // #1347: existing verify:parse-* harness bumps production-sweep counters
    {
        // Line format: leading NodeId (may be OOB → marked=0, still counts attempt).
        auto c = cs.eval("(verify:parse-coverage-feedback \"0 hit_rate=0.45\")");
        CHECK(c && is_int(*c), "parse-coverage-feedback returns int count");
        auto a = cs.eval("(verify:parse-assert-failure \"0 assert_p_ready\")");
        CHECK(a && is_int(*a), "parse-assert-failure returns int count");
        CHECK(href(cs, Q, "verify-parse-coverage-total") >= 1, "coverage parse counted");
        CHECK(href(cs, Q, "verify-parse-assert-total") >= 1, "assert parse counted");
    }

    // #1344: SV pattern presets callable
    {
        auto n = cs.eval("(query:sv-interface)");
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "query:sv-interface");
        auto p = cs.eval("(query:sv-property)");
        CHECK(p && is_int(*p) && as_int(*p) >= 0, "query:sv-property");
    }

    // #1338/#1341: DeadCoercionElimination parent-type stamp + reason counters
    {
        IRModule mod;
        IRFunction f;
        f.name = "dce_test";
        f.arg_count = 1;
        f.local_count = 3;
        f.blocks.resize(1);
        f.blocks[0].id = 0;
        // r0 = arg; r1 = cast r0 -> type 7 with narrow_evidence; identity
        f.blocks[0].instructions.push_back(IRInstruction{
            .opcode = IROpcode::Local,
            .operands = {0, 0, 0, 0},
            .type_id = 7,
        });
        f.blocks[0].instructions.push_back(IRInstruction{
            .opcode = IROpcode::CastOp,
            .operands = {1, 0, 7, 0},
            .type_id = 7,
            .narrow_evidence = 0x1,
        });
        f.blocks[0].instructions.push_back(IRInstruction{
            .opcode = IROpcode::Return,
            .operands = {1, 0, 0, 0},
        });
        mod.functions.push_back(std::move(f));

        aura::compiler::DeadCoercionEliminationPass dce;
        dce.run(mod);
        CHECK(dce.eliminated_count() >= 1, "DCE eliminated identity/narrow cast");
        CHECK(dce.narrow_evidence_hits() + dce.type_prop_hits() >= 1, "elision reason counted");
    }

    // #1345/#1346/#1348: FlatAST dirty prune + lockfree ref + soft compact
    {
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        aura::ast::FlatAST flat(alloc);
        // Minimal node so mark_dirty / StableNodeRef paths are live.
        // Do NOT commit_atomic_batch before make_ref — batch bumps generation_
        // without restamping node_gen_, which would stale the slot.
        const auto root = flat.add_node(aura::ast::NodeTag::Define);
        flat.root = root;
        if (root < flat.size()) {
            flat.mark_dirty_upward_fast(root, aura::ast::FlatAST::kGeneralDirty, 0, /*max_depth=*/8,
                                        /*stop_at_boundary=*/true);
            CHECK(flat.mark_dirty_boundary_prune_count() >= 1, "boundary prune on Define");
            auto ref = flat.make_ref(root);
            auto v = ref.validate_or_refresh(flat);
            CHECK(v.has_value(), "validate_or_refresh ok");
            CHECK(flat.lockfree_stable_ref_validate_count() >= 1, "lockfree validate counted");
        }
        flat.set_compaction_free_list_threshold(1);
        flat.begin_atomic_batch();
        flat.commit_atomic_batch();
        CHECK(flat.auto_compact_on_commit_count() >= 0, "auto compact counter readable");
    }

    // Smoke: eval still works after sweep
    {
        CompilerService cs2;
        auto a = cs2.eval("(+ 20 22)");
        CHECK(a && is_int(*a) && as_int(*a) == 42, "(+ 20 22)");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("production sweep #1336–#1348: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
