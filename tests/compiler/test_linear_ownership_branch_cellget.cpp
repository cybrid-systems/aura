// @category: unit
// @reason: Issue #2828 — LinearOwnershipPass/Wrap must detect use-after-move
// on Branch / Return / CellGet / MakePair input slots (previously false-
// listed as "no input slots").
//
//   AC1: source removes Branch/Return/CellGet/MakePair from false list;
//        input_slot_range + recovered metric; cites #2828
//   AC2: Move then Branch/Return/CellGet/MakePair → use_after_move >= 1
//   AC3: Jump/Nop remain input-free (no false positive on block ids)
//   AC4: schema-2828 query; this suite + linter; no docs/design/2828-*

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
using aura::compiler::LinearOwnershipPass;
using aura::compiler::LinearOwnershipWrap;
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

// Const slot0, MoveOp(result=1, src=0), then tail instruction.
static IRModule make_move_then(IRInstruction tail, std::uint32_t locals = 4) {
    IRModule mod;
    IRFunction fn;
    fn.name = "uam";
    fn.entry_block = 0;
    fn.local_count = locals;
    fn.blocks.push_back({0, {}, {}});
    fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 0));
    fn.blocks[0].instructions.push_back(insn(IROpcode::MoveOp, 1, 0));
    fn.blocks[0].instructions.push_back(std::move(tail));
    mod.add_function(std::move(fn));
    return mod;
}

} // namespace

int run_test_linear_ownership_branch_cellget() {
    std::println("=== Issue #2828: linear ownership Branch/Return/CellGet/MakePair ===");
    CHECK(true, "ac2828: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source removes false-list + input ranges ---");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        CHECK(!impls.empty(), "AC1: pass_impls readable");
        CHECK(impls.find("Issue #2828") != std::string::npos, "AC1: cites #2828");
        CHECK(impls.find("input_slot_range") != std::string::npos, "AC1: input_slot_range");
        CHECK(impls.find("input_scan_missed") != std::string::npos ||
                  impls.find("input_scan_missed_total") != std::string::npos,
              "AC1: recovered metric");
        auto wrap_pos = impls.find("static bool reads_input_(aura::ir::IROpcode op)");
        CHECK(wrap_pos != std::string::npos, "AC1: Wrap reads_input_ present");
        auto wrap_body = impls.substr(wrap_pos, 500);
        CHECK(wrap_body.find("CellGet") == std::string::npos,
              "AC1: CellGet not in Wrap false list");
        CHECK(wrap_body.find("MakePair") == std::string::npos,
              "AC1: MakePair not in Wrap false list");
        CHECK(wrap_body.find("case aura::ir::IROpcode::Branch:") == std::string::npos,
              "AC1: Branch not in Wrap false list");
        CHECK(wrap_body.find("case aura::ir::IROpcode::Return:") == std::string::npos,
              "AC1: Return not in Wrap false list");
        auto pass_pos2 = impls.rfind("static bool reads_input(aura::ir::IROpcode op)");
        CHECK(pass_pos2 != std::string::npos, "AC1: Pass reads_input present");
        auto pass_body = impls.substr(pass_pos2, 450);
        CHECK(pass_body.find("CellGet") == std::string::npos,
              "AC1: CellGet not in Pass false list");
        CHECK(pass_body.find("MakePair") == std::string::npos,
              "AC1: MakePair not in Pass false list");
        CHECK(pass_body.find("case aura::ir::IROpcode::Branch:") == std::string::npos,
              "AC1: Branch not in Pass false list");
        CHECK(pass_body.find("case aura::ir::IROpcode::Return:") == std::string::npos,
              "AC1: Return not in Pass false list");
    }

    // ── AC2: recovered ops detect UaM ──
    {
        std::println("\n--- AC2: Move then Branch/Return/CellGet/MakePair ---");

        // Return value after move of slot 0.
        {
            auto mod = make_move_then(insn(IROpcode::Return, 0));
            LinearOwnershipWrap wrap;
            wrap.run(mod);
            CHECK(wrap.use_after_move_count() >= 1,
                  std::format("AC2: Return UaM (got {})", wrap.use_after_move_count()));
            CHECK(wrap.has_error(), "AC2: Return has_error");
            LinearOwnershipPass pass;
            pass.run(mod);
            CHECK(pass.use_after_move_count() >= 1, "AC2: Pass Return UaM");
        }

        // Branch condition after move of slot 0.
        {
            auto mod = make_move_then(insn(IROpcode::Branch, 0, 0, 0));
            LinearOwnershipWrap wrap;
            wrap.run(mod);
            CHECK(wrap.use_after_move_count() >= 1,
                  std::format("AC2: Branch UaM (got {})", wrap.use_after_move_count()));
        }

        // CellGet cell_id after move of slot 0 (result=2, cell=0).
        {
            auto mod = make_move_then(insn(IROpcode::CellGet, 2, 0));
            LinearOwnershipWrap wrap;
            wrap.run(mod);
            CHECK(wrap.use_after_move_count() >= 1,
                  std::format("AC2: CellGet UaM (got {})", wrap.use_after_move_count()));
        }

        // MakePair car after move of slot 0 (result=2, car=0, cdr=1).
        {
            auto mod = make_move_then(insn(IROpcode::MakePair, 2, 0, 1));
            LinearOwnershipWrap wrap;
            wrap.run(mod);
            CHECK(wrap.use_after_move_count() >= 1,
                  std::format("AC2: MakePair UaM (got {})", wrap.use_after_move_count()));
        }

        CHECK(LinearOwnershipWrap::input_scan_missed_total() > 0, "AC2: recovered metric advanced");
    }

    // ── AC3: Jump does not false-positive on block id 0 ──
    {
        std::println("\n--- AC3: Jump remains input-free ---");
        // Move slot 0; Jump to block 0. Block id 0 must not count as slot read.
        auto mod = make_move_then(insn(IROpcode::Jump, 0));
        LinearOwnershipWrap wrap;
        wrap.run(mod);
        CHECK(wrap.use_after_move_count() == 0,
              std::format("AC3: Jump no UaM (got {})", wrap.use_after_move_count()));
        // Clean dual path: move then return the *new* binding (slot 1).
        auto mod2 = make_move_then(insn(IROpcode::Return, 1));
        LinearOwnershipWrap wrap2;
        wrap2.run(mod2);
        CHECK(
            wrap2.use_after_move_count() == 0,
            std::format("AC3: Return of move result clean (got {})", wrap2.use_after_move_count()));
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2828 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2828") == 2828, "AC4: schema-2828");
        CHECK(href(cs, "issue-2828") == 2828, "AC4: issue-2828");
        CHECK(href(cs, "linear-ownership-input-scan-recovered-wired") == 1, "AC4: wired");
        CHECK(href(cs, "linear-ownership-input-scan-missed-total") >= 0, "AC4: missed total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2828") != std::string::npos, "AC4: obs schema-2828");
        auto lint =
            read_file("scripts/coverage/checks/check_linear_ownership_branch_cellget_2828.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2828") != std::string::npos, "AC4: linter cites 2828");
    }

    std::println("\n=== #2828 linear ownership input scan: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_ownership_branch_cellget();
}
#endif
