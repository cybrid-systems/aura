// @category: unit
// @reason: Issue #2830 — DCEPass must mark Call/Apply/PrimCall arg-range
// slots as used (not only the fixed 4-operand header). Pure producers of
// args beyond the header must not be DCE'd.
//
//   AC1: source has mark_used_slots + Call/Apply expanded ranges; cites #2830
//   AC2: 5-arg Call keeps all pure arg producers
//   AC3: unused pure ops still DCE'd; MakePair car/cdr preserved
//   AC4: schema-2830 query; this suite + linter; no docs/design/2830-*

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

static int count_opcode(const IRModule& mod, IROpcode op) {
    int n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == op)
                    ++n;
    return n;
}

} // namespace

int run_test_dce_pass_variable_args() {
    std::println("=== Issue #2830: DCEPass variable-arg Call scan ===");
    CHECK(true, "ac2830: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: mark_used_slots + expanded Call range ---");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        CHECK(!impls.empty(), "AC1: pass_impls readable");
        CHECK(impls.find("Issue #2830") != std::string::npos, "AC1: cites #2830");
        CHECK(impls.find("mark_used_slots") != std::string::npos, "AC1: mark_used_slots");
        CHECK(impls.find("operand_under_count") != std::string::npos, "AC1: under-count metric");
        // Expanded Call path must scan base + i (definition, not comment).
        CHECK(impls.find("case O::Call:") != std::string::npos ||
                  impls.find("case aura::ir::IROpcode::Call:") != std::string::npos,
              "AC1: Call case in mark_used_slots");
        auto pos = impls.find("static void mark_used_slots");
        CHECK(pos != std::string::npos, "AC1: mark_used_slots definition");
        auto win = impls.substr(pos, 1500);
        CHECK(win.find("base + i") != std::string::npos, "AC1: Call arg range expansion");
        CHECK(win.find("Apply") != std::string::npos, "AC1: Apply expanded");
        CHECK(win.find("PrimCall") != std::string::npos, "AC1: PrimCall expanded");
    }

    // ── AC2: 5-arg Call keeps pure arg producers ──
    {
        std::println("\n--- AC2: 5-arg Call preserves pure arg consts ---");
        // slots 0..4 = ConstI64 args; slot 5 = Arg callee (non-pure);
        // Call(callee=5, arg_base=0, arg_count=5, result=6); Return(6)
        IRModule mod;
        IRFunction fn;
        fn.name = "five_arg";
        fn.entry_block = 0;
        fn.local_count = 8;
        fn.blocks.push_back({0, {}, {}});
        for (std::uint32_t s = 0; s < 5; ++s)
            fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, s, s + 10));
        fn.blocks[0].instructions.push_back(insn(IROpcode::Arg, 5, 0)); // callee slot
        fn.blocks[0].instructions.push_back(insn(IROpcode::Call, 5, 0, 5, 6));
        fn.blocks[0].instructions.push_back(insn(IROpcode::Return, 6));
        mod.add_function(std::move(fn));

        const auto under0 = DCEPass::operand_under_count_total();
        DCEPass dce;
        dce.run(mod);
        CHECK(count_opcode(mod, IROpcode::ConstI64) == 5,
              std::format("AC2: all 5 arg consts kept (got {})",
                          count_opcode(mod, IROpcode::ConstI64)));
        CHECK(count_opcode(mod, IROpcode::Call) == 1, "AC2: Call preserved");
        CHECK(DCEPass::operand_under_count_total() > under0,
              "AC2: under-count metric advanced on expanded args");
    }

    // ── AC3: unused pure still dies; MakePair car/cdr kept ──
    {
        std::println("\n--- AC3: unused pure DCE + MakePair uses ---");
        // Dead const + used MakePair components.
        IRModule mod;
        IRFunction fn;
        fn.name = "pair";
        fn.entry_block = 0;
        fn.local_count = 8;
        fn.blocks.push_back({0, {}, {}});
        fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 0, 1)); // dead
        fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 1, 2)); // car
        fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 2, 3)); // cdr
        fn.blocks[0].instructions.push_back(insn(IROpcode::MakePair, 3, 1, 2));
        fn.blocks[0].instructions.push_back(insn(IROpcode::Return, 3));
        mod.add_function(std::move(fn));

        DCEPass dce;
        dce.run(mod);
        // slot 0 ConstI64 unused → Nop; slots 1,2 used by MakePair → kept
        CHECK(
            count_opcode(mod, IROpcode::ConstI64) == 2,
            std::format("AC3: 2 used consts kept (got {})", count_opcode(mod, IROpcode::ConstI64)));
        CHECK(count_opcode(mod, IROpcode::MakePair) == 1, "AC3: MakePair kept");
        CHECK(count_opcode(mod, IROpcode::Nop) >= 1, "AC3: dead const → Nop");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2830 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2830") == 2830, "AC4: schema-2830");
        CHECK(href(cs, "issue-2830") == 2830, "AC4: issue-2830");
        CHECK(href(cs, "dce-variable-arg-scan-wired") == 1, "AC4: wired");
        CHECK(href(cs, "dce-operand-under-count-total") >= 0, "AC4: under-count total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2830") != std::string::npos, "AC4: obs schema-2830");
        auto lint = read_file("scripts/coverage/checks/check_dce_pass_variable_args_2830.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2830") != std::string::npos, "AC4: linter cites 2830");
    }

    std::println("\n=== #2830 DCE variable-arg: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dce_pass_variable_args();
}
#endif
