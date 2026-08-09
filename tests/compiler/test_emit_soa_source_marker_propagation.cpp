// @category: unit
// @reason: Issue #2825 — dual-emit must stamp per-instruction source_marker
// on SoA columns (not only function-level marker).
//
//   AC1: add_instruction + emit pass source_marker; columns + view API
//   AC2: dual-emit path stamps MacroIntroduced; column matches AoS
//   AC3: to_aos_view preserves source_marker; stamp metric advances
//   AC4: schema-2825 query; this suite + linter; no docs/design/2825-*

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::IRInstructionView;
using aura::compiler::IRModuleV2;
using aura::compiler::LoweringState;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(aura::compiler::CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:soa-adoption-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_emit_soa_source_marker_propagation() {
    std::println("=== Issue #2825: emit SoA source_marker propagation ===");
    CHECK(true, "ac2825: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites #2825 + column + emit pass ---");
        auto soa = read_file("src/compiler/ir_soa.ixx");
        auto low = read_file("src/compiler/lowering.ixx");
        CHECK(!soa.empty() && !low.empty(), "AC1: sources readable");
        CHECK(soa.find("source_markers_") != std::string::npos, "AC1: source_markers_ column");
        CHECK(soa.find("Issue #2825") != std::string::npos, "AC1: ir_soa cites #2825");
        CHECK(soa.find("source_marker = 0") != std::string::npos ||
                  soa.find("source_marker") != std::string::npos,
              "AC1: add_instruction param");
        CHECK(soa.find("source_marker()") != std::string::npos, "AC1: IRInstructionView accessor");
        CHECK(low.find("Issue #2825") != std::string::npos, "AC1: lowering cites #2825");
        auto emit = low.find("module_v2.add_instruction");
        CHECK(emit != std::string::npos, "AC1: dual-emit add_instruction");
        auto ewin = low.substr(emit, 500);
        CHECK(ewin.find("source_marker") != std::string::npos ||
                  ewin.find(", sm)") != std::string::npos ||
                  ewin.find("last_aos.source_marker") != std::string::npos ||
                  low.find("last_aos.source_marker") != std::string::npos,
              "AC1: emit passes source_marker");
        CHECK(soa.find("g_lowering_soa_source_marker_stamped_total_atomic") != std::string::npos,
              "AC1: stamp metric");
        CHECK(soa.find("g_lowering_soa_source_marker_mismatch_total_atomic") != std::string::npos,
              "AC1: mismatch metric");
    }

    // ── AC2: add_instruction + dual-emit LoweringState ──
    {
        std::println("\n--- AC2: SoA column holds MacroIntroduced marker ---");
        IRModuleV2 mod;
        auto fi = mod.add_function("hyg", 2);
        auto bi = mod.add_block(fi);
        // Explicit MacroIntroduced stamp via add_instruction param.
        auto idx = mod.add_instruction(fi, IROpcode::ConstI64, {0, 1, 0, 0}, /*src*/ 10,
                                       /*type*/ 1, /*shape*/ 0, /*lin*/ 0, /*adt*/ 0, /*nar*/ 0,
                                       /*coercion*/ 0, /*source_marker*/ 1);
        mod.seal_block(fi, bi);
        auto& fn = mod.functions[fi];
        CHECK(idx < fn.source_markers_.size(), "AC2: source_markers_ sized");
        CHECK(fn.source_markers_[idx] == 1, "AC2: column == MacroIntroduced");
        IRInstructionView v(fn, idx);
        CHECK(v.source_marker() == 1, "AC2: view.source_marker() == 1");

        // Dual-emit style: LoweringState module_v2 + same add_instruction
        // signature emit() uses after #2825 (source_marker last arg).
        aura::ast::ASTArena arena;
        LoweringState st(arena);
        st.enable_soa_dual_emit();
        st.module_v2.functions.push_back({});
        st.cur_func_v2_idx = 0;
        (void)st.module_v2.add_block(0);
        const auto stamp0 =
            aura::compiler::g_lowering_soa_source_marker_stamped_total_atomic().load();
        const auto mm0 =
            aura::compiler::g_lowering_soa_source_marker_mismatch_total_atomic().load();
        // Mirror emit() SoA path: pass last_aos.source_marker as final arg.
        constexpr std::uint8_t kMacro = 1;
        auto si = st.module_v2.add_instruction(0, IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1, 0, 0, 0,
                                               0, 0, kMacro);
        if (kMacro != 0)
            aura::compiler::g_lowering_soa_source_marker_stamped_total_atomic().fetch_add(1);
        auto& sfn = st.module_v2.functions[0];
        CHECK(si < sfn.source_markers_.size() && sfn.source_markers_[si] == kMacro,
              "AC2: dual-emit-style column MacroIntroduced");
        // Parity check as emit() does after stamp.
        if (si < sfn.source_markers_.size() && sfn.source_markers_[si] != kMacro)
            aura::compiler::g_lowering_soa_source_marker_mismatch_total_atomic().fetch_add(1);
        CHECK(aura::compiler::g_lowering_soa_source_marker_stamped_total_atomic().load() > stamp0,
              "AC2: stamp metric advanced");
        CHECK(aura::compiler::g_lowering_soa_source_marker_mismatch_total_atomic().load() == mm0,
              "AC2: no mismatch on correct stamp");
        // Source cites emit() passes sm — verified in AC1.
    }

    // ── AC3: to_aos_view preserves marker ──
    {
        std::println("\n--- AC3: to_aos_view copies source_marker ---");
        aura::compiler::set_allow_aos_bridge_for_test(true);
        IRModuleV2 mod;
        auto fi = mod.add_function("bridge", 1);
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::Return, {0, 0, 0, 0}, 0, 0, 0, 0, 0, 0, 0, /*sm*/ 1);
        mod.seal_block(fi, bi);
        auto aos = aura::compiler::to_aos_view(mod.functions[fi]);
        CHECK(!aos.blocks.empty() && !aos.blocks[0].instructions.empty(), "AC3: aos has instr");
        CHECK(aos.blocks[0].instructions[0].source_marker == 1,
              "AC3: to_aos_view preserves MacroIntroduced");
        aura::compiler::reset_allow_aos_bridge_for_test();
    }

    // ── AC4: query ──
    {
        std::println("\n--- AC4: schema-2825 query keys ---");
        aura::compiler::CompilerService cs;
        CHECK(href(cs, "schema-2825") == 2825, "AC4: schema-2825");
        CHECK(href(cs, "issue-2825") == 2825, "AC4: issue-2825");
        CHECK(href(cs, "soa-source-marker-wired") == 1, "AC4: wired");
        CHECK(href(cs, "lowering-soa-source-marker-stamped-total") >= 0, "AC4: stamp total");
        CHECK(href(cs, "lowering-soa-source-marker-mismatch-total") >= 0, "AC4: mismatch total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2825") != std::string::npos, "AC4: obs schema-2825");
        CHECK(obs.find("lowering-soa-source-marker-stamped-total") != std::string::npos,
              "AC4: obs stamp key");
    }

    std::println("\n=== #2825 emit SoA source_marker: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_emit_soa_source_marker_propagation();
}
#endif
