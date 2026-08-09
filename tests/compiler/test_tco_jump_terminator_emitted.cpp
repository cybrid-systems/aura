// @category: unit
// @reason: Issue #2833 — TCOPass must emit a single Jump terminator for
// tail-call rewrite (not Branch; no duplicate opcode assignment).
//
//   AC1: source has one Jump assign in transform; cites #2833; no "need Branch"
//   AC2: runtime tail-call block ends with Jump, not Branch
//   AC3: Jump target is callee entry; Call+Return removed
//   AC4: schema-2833 query; this suite + linter; no docs/design/2833-*

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
using aura::compiler::TCOPass;
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

// Single-block self-tail: MakeClosure; Call; Return.
static IRModule make_self_tail() {
    IRModule mod;
    IRFunction fn;
    fn.name = "jump_term";
    fn.id = 0;
    fn.entry_block = 0;
    fn.local_count = 4;
    fn.blocks.resize(1);
    fn.blocks[0].id = 0;
    fn.blocks[0].instructions.push_back(insn(IROpcode::MakeClosure, 0, 0, 0));
    fn.blocks[0].instructions.push_back(insn(IROpcode::Call, 0, 0, 0, 1));
    fn.blocks[0].instructions.push_back(insn(IROpcode::Return, 1));
    mod.add_function(std::move(fn));
    return mod;
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

int run_test_tco_jump_terminator_emitted() {
    std::println("=== Issue #2833: TCO Jump terminator (no duplicate) ===");
    CHECK(true, "ac2833: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: single Jump assign; no mid-edit residue ---");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        CHECK(!impls.empty(), "AC1: pass_impls readable");
        CHECK(impls.find("Issue #2833") != std::string::npos, "AC1: cites #2833");
        CHECK(TCOPass::kTcoTerminatorIsJump, "AC1: kTcoTerminatorIsJump");
        CHECK(TCOPass::kTcoJumpTerminatorIssue == 2833, "AC1: issue stamp constexpr");

        // Transform window: from Issue #2833 single Jump comment to pop_back.
        auto pos = impls.find("Issue #2833: single Jump assignment");
        CHECK(pos != std::string::npos, "AC1: single Jump assignment comment");
        auto win = impls.substr(pos, 500);
        // Exactly one opcode=Jump assignment in the transform window.
        int jump_assigns = 0;
        std::size_t search = 0;
        const std::string needle = "opcode = aura::ir::IROpcode::Jump";
        while (true) {
            auto p = win.find(needle, search);
            if (p == std::string::npos)
                break;
            ++jump_assigns;
            search = p + needle.size();
        }
        CHECK(jump_assigns == 1,
              std::format("AC1: exactly one Jump assign in transform (got {})", jump_assigns));
        CHECK(win.find("need Branch") == std::string::npos, "AC1: no 'need Branch' residue");
        CHECK(win.find("Branch is the new terminator") == std::string::npos,
              "AC1: no stale Branch terminator comment");
        CHECK(win.find("Jump is the new terminator") != std::string::npos,
              "AC1: Jump is the new terminator");

        // Class-level design comment also says Jump, not Branch.
        auto design = impls.find("Replace the Call with a Jump");
        CHECK(design != std::string::npos, "AC1: design doc uses Jump");
        CHECK(impls.find("Remove the Return (the Branch is the new terminator)") ==
                  std::string::npos,
              "AC1: design doc Branch terminator removed");
    }

    // ── AC2/AC3: emitted terminator is Jump to entry ──
    {
        std::println("\n--- AC2/AC3: tail block terminator is Jump ---");
        auto mod = make_self_tail();
        TCOPass tco;
        tco.run(mod);
        CHECK(tco.tco_count() >= 1, "AC2: TCO applied");
        CHECK(count_op(mod, IROpcode::Call) == 0, "AC2: Call gone");
        CHECK(count_op(mod, IROpcode::Return) == 0, "AC2: Return gone");
        CHECK(count_op(mod, IROpcode::Branch) == 0, "AC2: no Branch emitted");
        CHECK(count_op(mod, IROpcode::Jump) >= 1, "AC2: Jump present");

        const auto& blk = mod.functions[0].blocks[0];
        CHECK(!blk.instructions.empty(), "AC3: block non-empty");
        const auto& last = blk.instructions.back();
        CHECK(last.opcode == IROpcode::Jump, "AC3: terminator is Jump");
        CHECK(last.operands[0] == 0, "AC3: Jump targets entry block 0");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2833 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2833") == 2833, "AC4: schema-2833");
        CHECK(href(cs, "issue-2833") == 2833, "AC4: issue-2833");
        CHECK(href(cs, "tco-jump-terminator-wired") == 1, "AC4: wired");
        CHECK(href(cs, "tco-jump-terminator-issue") == 2833, "AC4: issue key");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2833") != std::string::npos, "AC4: obs schema-2833");
        auto lint = read_file("scripts/coverage/checks/check_tco_jump_terminator_2833.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2833") != std::string::npos, "AC4: linter cites 2833");
    }

    std::println("\n=== #2833 TCO Jump terminator: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tco_jump_terminator_emitted();
}
#endif
