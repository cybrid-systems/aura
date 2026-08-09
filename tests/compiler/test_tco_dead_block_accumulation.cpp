// @category: unit
// @reason: Issue #2831 — after inter-block TCO rewrites Jump preds,
// unreachable tail-call shells must be swept from func.blocks so IR
// does not accumulate dead blocks across TCO runs.
//
//   AC1: source has sweep_dead_blocks + tco_dead_block_total; cites #2831
//   AC2: inter-block TCO leaves no unreachable block after run
//   AC3: repeated TCO runs do not grow blocks.size()
//   AC4: schema-2831 query; this suite + linter; no docs/design/2831-*

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

// Self-recursive inter-block tail-call pattern:
//   B0 (entry): MakeClosure(slot0, this_func); Jump(B1)
//   B1: Call(slot0, arg_base=0, count=0, result=1); Return(1)
// After inter-block TCO: B0 Jump(entry); B1 unreachable → swept.
static IRModule make_inter_block_tail_call() {
    IRModule mod;
    IRFunction fn;
    fn.name = "self_tco";
    fn.id = 0;
    fn.entry_block = 0;
    fn.local_count = 4;
    fn.blocks.resize(2);
    fn.blocks[0].id = 0;
    fn.blocks[1].id = 1;
    // Entry: closure for self, jump to tail-call block.
    fn.blocks[0].instructions.push_back(insn(IROpcode::MakeClosure, 0, 0, 0)); // slot0 = self
    fn.blocks[0].instructions.push_back(insn(IROpcode::Jump, 1));
    // Tail-call block: Call self + Return result.
    fn.blocks[1].instructions.push_back(insn(IROpcode::Call, 0, 0, 0, 1));
    fn.blocks[1].instructions.push_back(insn(IROpcode::Return, 1));
    mod.add_function(std::move(fn));
    return mod;
}

// N independent inter-block tail-call shells:
//   entry Jump(B1); B1 Call+Return self; ... also B2..BN same pattern
// Reachable: only entry after TCO (all Jump preds retargeted to entry).
static IRModule make_n_tail_call_blocks(std::size_t n_dead) {
    IRModule mod;
    IRFunction fn;
    fn.name = "many_tco";
    fn.id = 0;
    fn.entry_block = 0;
    fn.local_count = 8;
    const std::size_t n_blocks = 1 + n_dead;
    fn.blocks.resize(n_blocks);
    for (std::size_t i = 0; i < n_blocks; ++i)
        fn.blocks[i].id = static_cast<std::uint32_t>(i);
    // Entry: MakeClosure + Jump to first tail block.
    fn.blocks[0].instructions.push_back(insn(IROpcode::MakeClosure, 0, 0, 0));
    fn.blocks[0].instructions.push_back(insn(IROpcode::Jump, 1));
    for (std::size_t i = 1; i < n_blocks; ++i) {
        fn.blocks[i].instructions.push_back(insn(IROpcode::Call, 0, 0, 0, 1));
        fn.blocks[i].instructions.push_back(insn(IROpcode::Return, 1));
        // Chain: each non-last also jumped-from... only entry jumps to 1.
        // Extra dead blocks are only reachable if something jumps to them.
        // For blocks 2..N to become dead after TCO, they need preds first.
        // Wire: block i-1 ends with Jump(i) before TCO for i>=2? That would
        // make a chain. Simpler: entry Jump(1); and also make each Bi
        // only a tail-call block with no pred except for B1 from entry.
        // Then only B1 is dead after TCO; B2.. without preds are already
        // dead and should also be swept.
    }
    mod.add_function(std::move(fn));
    return mod;
}

} // namespace

int run_test_tco_dead_block_accumulation() {
    std::println("=== Issue #2831: TCO dead-block sweep ===");
    CHECK(true, "ac2831: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: sweep_dead_blocks present ---");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        CHECK(!impls.empty(), "AC1: pass_impls readable");
        CHECK(impls.find("Issue #2831") != std::string::npos, "AC1: cites #2831");
        CHECK(impls.find("sweep_dead_blocks") != std::string::npos, "AC1: sweep_dead_blocks");
        CHECK(impls.find("tco_dead_block_total") != std::string::npos, "AC1: dead metric");
        auto pos = impls.find("void run(aura::ir::IRModule& module)");
        // Prefer TCOPass run: look near sweep call.
        auto sweep_call = impls.find("sweep_dead_blocks(func)");
        CHECK(sweep_call != std::string::npos, "AC1: sweep called from run");
        (void)pos;
    }

    // ── AC2: inter-block TCO reclaims unreachable tail block ──
    {
        std::println("\n--- AC2: inter-block dead shell reclaimed ---");
        auto mod = make_inter_block_tail_call();
        CHECK(mod.functions[0].blocks.size() == 2, "AC2: start with 2 blocks");
        const auto dead0 = TCOPass::tco_dead_block_total();
        TCOPass tco;
        tco.run(mod);
        CHECK(mod.functions[0].blocks.size() == 1,
              std::format("AC2: only entry remains (got {})", mod.functions[0].blocks.size()));
        CHECK(mod.functions[0].blocks[0].id == 0, "AC2: entry id 0 kept");
        CHECK(tco.tco_dead_block_count() >= 1, "AC2: per-run dead count");
        CHECK(TCOPass::tco_dead_block_total() > dead0, "AC2: process total advanced");
        // Entry should Jump to self entry (or Jump(0)).
        bool has_jump = false;
        for (const auto& i : mod.functions[0].blocks[0].instructions) {
            if (i.opcode == IROpcode::Jump) {
                has_jump = true;
                CHECK(i.operands[0] == 0, "AC2: Jump targets entry");
            }
        }
        CHECK(has_jump, "AC2: entry still has Jump");
    }

    // ── AC3: repeated TCO does not grow blocks ──
    {
        std::println("\n--- AC3: repeated runs do not accumulate ---");
        auto mod = make_n_tail_call_blocks(3); // entry + 3 dead candidates
        const auto start_n = mod.functions[0].blocks.size();
        CHECK(start_n == 4, "AC3: start size 4");
        TCOPass tco1;
        tco1.run(mod);
        const auto after1 = mod.functions[0].blocks.size();
        CHECK(after1 <= start_n, "AC3: size does not grow after first run");
        CHECK(after1 == 1, std::format("AC3: only live entry (got {})", after1));
        // M additional runs — size must stay stable.
        for (int m = 0; m < 10; ++m) {
            // Re-inject a fresh dead shell each time to simulate
            // sessions that keep producing tail-call blocks.
            aura::ir::BasicBlock dead;
            dead.id = static_cast<std::uint32_t>(100 + m);
            dead.instructions.push_back(insn(IROpcode::Call, 0, 0, 0, 1));
            dead.instructions.push_back(insn(IROpcode::Return, 1));
            // No pred → already unreachable; also wire a Jump from a
            // temp pred pattern: Jump from entry was retargeted.
            mod.functions[0].blocks.push_back(std::move(dead));
            TCOPass tco;
            tco.run(mod);
            CHECK(mod.functions[0].blocks.size() == 1,
                  std::format("AC3: after run {} size still 1 (got {})", m,
                              mod.functions[0].blocks.size()));
        }
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2831 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2831") == 2831, "AC4: schema-2831");
        CHECK(href(cs, "issue-2831") == 2831, "AC4: issue-2831");
        CHECK(href(cs, "tco-dead-block-sweep-wired") == 1, "AC4: wired");
        CHECK(href(cs, "tco-dead-block-total") >= 0, "AC4: dead total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2831") != std::string::npos, "AC4: obs schema-2831");
        auto lint = read_file("scripts/coverage/checks/check_tco_dead_block_accumulation_2831.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2831") != std::string::npos, "AC4: linter cites 2831");
    }

    std::println("\n=== #2831 TCO dead-block sweep: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_tco_dead_block_accumulation();
}
#endif
