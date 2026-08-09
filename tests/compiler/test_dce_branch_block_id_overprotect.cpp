// @category: unit
// @reason: Issue #2834 — DCEPass must not treat Branch/Jump block-id
// operands as local slot uses (over-protects pure ops at those indices).
//
//   AC1: mark_used_slots has Branch/Jump cases; cites #2834; metric
//   AC2: Branch(cond, 1, 2) does not protect pure dead slots 1 and 2
//   AC3: Jump(0) does not protect pure dead slot 0 when unused
//   AC4: schema-2834 query; this suite + linter; no docs/design/2834-*

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.pass_manager;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::DCEPass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:optimization-passes-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static IRInstruction insn(IROpcode op, std::uint32_t a = 0, std::uint32_t b = 0,
                          std::uint32_t c = 0, std::uint32_t d = 0) {
    IRInstruction i;
    i.opcode = op;
    i.operands = {a, b, c, d};
    return i;
}

static int count_op(const IRModule& mod, IROpcode op) {
    int n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == op)
                    ++n;
    return n;
}

} // namespace

int run_test_dce_branch_block_id_overprotect() {
    std::println("=== Issue #2834: DCE Branch block-id overprotect ===");
    CHECK(true, "ac2834: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: Branch/Jump special-cased in mark_used_slots ---");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        CHECK(!impls.empty(), "AC1: pass_impls readable");
        CHECK(impls.find("Issue #2834") != std::string::npos, "AC1: cites #2834");
        CHECK(impls.find("branch_block_id_overprotect") != std::string::npos, "AC1: metric");
        auto pos = impls.find("static void mark_used_slots");
        CHECK(pos != std::string::npos, "AC1: mark_used_slots");
        auto win = impls.substr(pos, 2200);
        CHECK(win.find("case O::Branch:") != std::string::npos, "AC1: Branch case");
        CHECK(win.find("case O::Jump:") != std::string::npos, "AC1: Jump case");
        CHECK(win.find("block ids") != std::string::npos ||
                  win.find("block id") != std::string::npos,
              "AC1: documents block ids");
    }

    // ── AC2: Branch targets do not protect dead pure slots ──
    {
        std::println("\n--- AC2: Branch(cond,1,2) allows DCE of slots 1 and 2 ---");
        // slot0 = cond (live); slot1, slot2 = pure dead consts;
        // Branch(0, 1, 2) — targets look like slots under old scan.
        IRModule mod;
        IRFunction fn;
        fn.name = "br";
        fn.local_count = 8;
        fn.entry_block = 0;
        fn.blocks.push_back({0, {}, {}});
        fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 0, 1)); // cond live
        fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 1, 2)); // dead pure
        fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 2, 3)); // dead pure
        fn.blocks[0].instructions.push_back(insn(IROpcode::Branch, 0, 1, 2));
        mod.add_function(std::move(fn));

        const auto ov0 = DCEPass::branch_block_id_overprotect_total();
        DCEPass dce;
        dce.run(mod);
        CHECK(dce.eliminated_count() >= 2,
              std::format("AC2: eliminated pure slots 1+2 (got {})", dce.eliminated_count()));
        // Cond producer kept; dead pure Nop'd.
        CHECK(count_op(mod, IROpcode::ConstI64) == 1,
              std::format("AC2: only cond Const remains (got {})",
                          count_op(mod, IROpcode::ConstI64)));
        CHECK(count_op(mod, IROpcode::Nop) >= 2, "AC2: at least 2 Nops");
        CHECK(count_op(mod, IROpcode::Branch) == 1, "AC2: Branch preserved");
        CHECK(DCEPass::branch_block_id_overprotect_total() > ov0,
              "AC2: overprotect-prevented metric advanced");
    }

    // ── AC3: Jump does not protect slot 0 ──
    {
        std::println("\n--- AC3: Jump(0) does not protect unused pure at slot 0 ---");
        // Pure dead at slot 0; Jump targets block 0 (same numeric id).
        // Cond path: also Return of a different live slot so Jump is not
        // the only use — actually Jump uses no slots, so all pure dead.
        IRModule mod;
        IRFunction fn;
        fn.name = "jmp";
        fn.local_count = 8;
        fn.entry_block = 0;
        fn.blocks.push_back({0, {}, {}});
        fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 0, 42)); // dead
        fn.blocks[0].instructions.push_back(insn(IROpcode::Jump, 0));
        mod.add_function(std::move(fn));

        DCEPass dce;
        dce.run(mod);
        CHECK(dce.eliminated_count() >= 1, "AC3: pure at slot 0 eliminated");
        CHECK(count_op(mod, IROpcode::ConstI64) == 0, "AC3: Const Nop'd");
        CHECK(count_op(mod, IROpcode::Jump) == 1, "AC3: Jump preserved");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2834 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2834") == 2834, "AC4: schema-2834");
        CHECK(href(cs, "issue-2834") == 2834, "AC4: issue-2834");
        CHECK(href(cs, "dce-branch-block-id-wired") == 1, "AC4: wired");
        CHECK(href(cs, "dce-branch-block-id-overprotect-total") >= 0, "AC4: total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2834") != std::string::npos, "AC4: obs schema-2834");
        auto lint =
            read_file("scripts/coverage/checks/check_dce_branch_block_id_overprotect_2834.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2834") != std::string::npos, "AC4: linter cites 2834");
    }

    std::println("\n=== #2834 DCE Branch overprotect: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dce_branch_block_id_overprotect();
}
#endif
