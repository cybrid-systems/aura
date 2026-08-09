// @category: unit
// @reason: Issue #2832 — TCOPass non-zero arg_base must not emit Local
// with source slot >= local_count. Malformed IR is refused (Call+Return
// left intact) and tco_arg_base_oob_skipped_total advances.
//
//   AC1: source bounds-checks arg_base+count vs local_count; cites #2832
//   AC2: OOB Call (arg_base=3, arg_count=4, local_count=4) → no OOB Local
//   AC3: in-bounds non-zero arg_base still TCO's; oob metric only on skip
//   AC4: schema-2832 query; this suite + linter; no docs/design/2832-*

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

// Single-block tail call: MakeClosure; Call(callee=0, arg_base, arg_count, result);
// Return(result). local_count controls the valid slot window.
static IRModule make_tail_call(std::uint32_t local_count, std::uint32_t arg_base,
                               std::uint32_t arg_count) {
    IRModule mod;
    IRFunction fn;
    fn.name = "tco_oob";
    fn.id = 0;
    fn.entry_block = 0;
    fn.local_count = local_count;
    fn.blocks.resize(1);
    fn.blocks[0].id = 0;
    // result slot = local_count > 0 ? local_count - 1 : 0 (may be unused for OOB case)
    const std::uint32_t result = local_count > 0 ? local_count - 1 : 0;
    fn.blocks[0].instructions.push_back(insn(IROpcode::MakeClosure, 0, 0, 0));
    fn.blocks[0].instructions.push_back(insn(IROpcode::Call, 0, arg_base, arg_count, result));
    fn.blocks[0].instructions.push_back(insn(IROpcode::Return, result));
    mod.add_function(std::move(fn));
    return mod;
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

static bool any_local_src_oob(const IRModule& mod) {
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::Local && i.operands[1] >= f.local_count)
                    return true;
    return false;
}

} // namespace

int run_test_tco_arg_base_oob() {
    std::println("=== Issue #2832: TCOPass arg_base OOB guard ===");
    CHECK(true, "ac2832: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: bounds check vs local_count ---");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        CHECK(!impls.empty(), "AC1: pass_impls readable");
        CHECK(impls.find("Issue #2832") != std::string::npos, "AC1: cites #2832");
        CHECK(impls.find("tco_arg_base_oob_skipped") != std::string::npos, "AC1: oob metric");
        CHECK(impls.find("local_count") != std::string::npos, "AC1: local_count");
        // Prefer #2832 contract comment window (not inter-block continue).
        auto pos = impls.find("Issue #2832 contract");
        CHECK(pos != std::string::npos, "AC1: non-zero arg_base path");
        auto win = impls.substr(pos, 900);
        CHECK(win.find("tco_arg_base_oob_skipped") != std::string::npos,
              "AC1: OOB skip in arg_base path");
        CHECK(win.find("we don't re-validate") == std::string::npos,
              "AC1: removed 'don't re-validate' claim");
    }

    // ── AC2: malformed IR refused ──
    {
        std::println("\n--- AC2: OOB arg_base refused ---");
        // local_count=4, arg_base=3, arg_count=4 → slots 3,4,5,6 — 4..6 OOB
        auto mod = make_tail_call(/*local_count=*/4, /*arg_base=*/3, /*arg_count=*/4);
        const auto oob0 = TCOPass::tco_arg_base_oob_skipped_total();
        const auto tco0 = [&] {
            TCOPass t;
            // capture tco_count after run via instance
            return t;
        };
        (void)tco0;
        TCOPass tco;
        tco.run(mod);
        CHECK(TCOPass::tco_arg_base_oob_skipped_total() > oob0, "AC2: oob-skipped advanced");
        CHECK(tco.tco_count() == 0, "AC2: no TCO transform on OOB");
        CHECK(count_opcode(mod, IROpcode::Call) == 1, "AC2: Call left intact");
        CHECK(count_opcode(mod, IROpcode::Return) == 1, "AC2: Return left intact");
        CHECK(!any_local_src_oob(mod), "AC2: no Local with src >= local_count");
    }

    // ── AC3: in-bounds non-zero arg_base still works ──
    {
        std::println("\n--- AC3: in-bounds arg_base still TCO ---");
        // local_count=8, arg_base=2, arg_count=3 → slots 2,3,4 all valid
        auto mod = make_tail_call(/*local_count=*/8, /*arg_base=*/2, /*arg_count=*/3);
        const auto oob0 = TCOPass::tco_arg_base_oob_skipped_total();
        TCOPass tco;
        tco.run(mod);
        CHECK(tco.tco_count() >= 1, "AC3: TCO applied");
        CHECK(count_opcode(mod, IROpcode::Call) == 0, "AC3: Call rewritten away");
        CHECK(count_opcode(mod, IROpcode::Jump) >= 1, "AC3: Jump present");
        CHECK(!any_local_src_oob(mod), "AC3: all Local src in bounds");
        // Locals emitted for arg copies: operands[1] in {2,3,4}
        int local_n = 0;
        for (const auto& i : mod.functions[0].blocks[0].instructions) {
            if (i.opcode == IROpcode::Local) {
                ++local_n;
                CHECK(i.operands[1] < 8, "AC3: Local src < 8");
                CHECK(i.operands[1] >= 2 && i.operands[1] <= 4, "AC3: Local src in [2,4]");
            }
        }
        CHECK(local_n == 3, std::format("AC3: 3 Local copies (got {})", local_n));
        CHECK(TCOPass::tco_arg_base_oob_skipped_total() == oob0, "AC3: no oob bump on good IR");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2832 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2832") == 2832, "AC4: schema-2832");
        CHECK(href(cs, "issue-2832") == 2832, "AC4: issue-2832");
        CHECK(href(cs, "tco-arg-base-oob-wired") == 1, "AC4: wired");
        CHECK(href(cs, "tco-arg-base-oob-skipped-total") >= 0, "AC4: oob total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2832") != std::string::npos, "AC4: obs schema-2832");
        auto lint = read_file("scripts/coverage/checks/check_tco_arg_base_oob_2832.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2832") != std::string::npos, "AC4: linter cites 2832");
    }

    std::println("\n=== #2832 TCO arg_base OOB: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tco_arg_base_oob();
}
#endif
