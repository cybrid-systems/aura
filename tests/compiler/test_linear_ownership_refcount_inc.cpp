// @category: unit
// @reason: Issue #2829 — RefCountOp-inc (ops[2]==1) must not mark source
// as moved; only RefCountOp-dec (ops[2]==0) consumes. Prior is_consuming
// always returned true for RefCountOp → false-positive use-after-move.
//
//   AC1: source inspects operands[2]; cites #2829; inc metric
//   AC2: RefCountOp-inc + CellGet → use_after_move == 0
//   AC3: RefCountOp-dec + CellGet → use_after_move >= 1
//   AC4: schema-2829 query; this suite + linter; no docs/design/2829-*

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

// LinearWrap slot0 from const; RefCountOp(result, src, flag); then CellGet.
// flag: 1 = inc (share), 0 = dec (consume).
static IRModule make_refcount_then_cellget(std::uint32_t flag) {
    IRModule mod;
    IRFunction fn;
    fn.name = "rc";
    fn.entry_block = 0;
    fn.local_count = 4;
    fn.blocks.push_back({0, {}, {}});
    // slot 0 := const
    fn.blocks[0].instructions.push_back(insn(IROpcode::ConstI64, 0));
    // slot 1 := LinearWrap(slot 0)
    fn.blocks[0].instructions.push_back(insn(IROpcode::LinearWrap, 1, 0));
    // slot 2 := RefCountOp(inner=1, flag)
    fn.blocks[0].instructions.push_back(insn(IROpcode::RefCountOp, 2, 1, flag));
    // slot 3 := CellGet(cell=1) — read the original linear slot after refcount
    fn.blocks[0].instructions.push_back(insn(IROpcode::CellGet, 3, 1));
    mod.add_function(std::move(fn));
    return mod;
}

} // namespace

int run_test_linear_ownership_refcount_inc() {
    std::println("=== Issue #2829: LinearOwnership RefCountOp-inc non-consume ===");
    CHECK(true, "ac2829: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: is_consuming inspects operands[2] ---");
        auto impls = read_file("src/compiler/pass_impls.ixx");
        auto ir = read_file("src/compiler/ir.ixx");
        CHECK(!impls.empty(), "AC1: pass_impls readable");
        CHECK(impls.find("Issue #2829") != std::string::npos, "AC1: cites #2829");
        CHECK(impls.find("operands[2]") != std::string::npos, "AC1: operands[2] inspected");
        CHECK(impls.find("note_refcount_inc_non_consume") != std::string::npos ||
                  impls.find("refcount_inc_false_positive") != std::string::npos,
              "AC1: inc metric");
        // Must not unconditionally return true on RefCountOp alone.
        auto wrap = impls.find("static bool is_consuming_(const aura::ir::IRInstruction");
        CHECK(wrap != std::string::npos, "AC1: Wrap is_consuming_ takes instruction");
        auto pass = impls.rfind("static bool is_consuming(const aura::ir::IRInstruction");
        CHECK(pass != std::string::npos, "AC1: Pass is_consuming takes instruction");
        CHECK(ir.find("Issue #2829") != std::string::npos ||
                  ir.find("inc(1)/dec(0)") != std::string::npos,
              "AC1: ir.ixx documents inc/dec encoding");
    }

    // ── AC2: inc + read is clean ──
    {
        std::println("\n--- AC2: RefCountOp-inc + CellGet → no UaM ---");
        const auto fp0 = LinearOwnershipWrap::refcount_inc_false_positive_total();
        auto mod = make_refcount_then_cellget(/*flag=*/1);
        LinearOwnershipWrap wrap;
        wrap.run(mod);
        CHECK(wrap.use_after_move_count() == 0,
              std::format("AC2: Wrap UaM after inc (got {})", wrap.use_after_move_count()));
        CHECK(!wrap.has_error(), "AC2: Wrap has_error false");
        LinearOwnershipPass pass;
        pass.run(mod);
        CHECK(pass.use_after_move_count() == 0,
              std::format("AC2: Pass UaM after inc (got {})", pass.use_after_move_count()));
        CHECK(LinearOwnershipWrap::refcount_inc_false_positive_total() > fp0,
              "AC2: inc non-consume metric advanced");
    }

    // ── AC3: dec + read is UaM ──
    {
        std::println("\n--- AC3: RefCountOp-dec + CellGet → UaM ---");
        auto mod = make_refcount_then_cellget(/*flag=*/0);
        LinearOwnershipWrap wrap;
        wrap.run(mod);
        CHECK(wrap.use_after_move_count() >= 1,
              std::format("AC3: Wrap UaM after dec (got {})", wrap.use_after_move_count()));
        CHECK(wrap.has_error(), "AC3: Wrap has_error true");
        LinearOwnershipPass pass;
        pass.run(mod);
        CHECK(pass.use_after_move_count() >= 1,
              std::format("AC3: Pass UaM after dec (got {})", pass.use_after_move_count()));
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2829 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2829") == 2829, "AC4: schema-2829");
        CHECK(href(cs, "issue-2829") == 2829, "AC4: issue-2829");
        CHECK(href(cs, "linear-ownership-refcount-inc-wired") == 1, "AC4: wired");
        CHECK(href(cs, "linear-ownership-refcount-inc-false-positive-total") >= 0,
              "AC4: false-positive total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2829") != std::string::npos, "AC4: obs schema-2829");
        auto lint =
            read_file("scripts/coverage/checks/check_linear_ownership_refcount_inc_2829.py");
        CHECK(!lint.empty(), "AC4: linter present");
        CHECK(lint.find("2829") != std::string::npos, "AC4: linter cites 2829");
    }

    std::println("\n=== #2829 RefCountOp-inc: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_ownership_refcount_inc();
}
#endif
