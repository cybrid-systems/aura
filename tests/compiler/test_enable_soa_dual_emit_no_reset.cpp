// @category: unit
// @reason: Issue #2821 — enable_soa_dual_emit must not wipe module_v2 when
// already enabled (silent AoS↔SoA divergence). Skip-reset metric + force path.
//
//   AC1: source cites #2821; skip-reset; force_reset param
//   AC2: second enable preserves SoA functions; metric advances
//   AC3: force_reset=true wipes; first enable still resets from cold
//   AC4: schema-2821 query; this suite + linter; no docs/design/2821-*

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core;
import aura.compiler.service;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.lowering;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:soa-adoption-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_enable_soa_dual_emit_no_reset() {
    std::println("=== Issue #2821: enable_soa_dual_emit no silent reset ===");
    CHECK(true, "ac2821: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: source cites skip-reset + force_reset ---");
        auto low = read_file("src/compiler/lowering.ixx");
        auto soa = read_file("src/compiler/ir_soa.ixx");
        CHECK(!low.empty(), "AC1: lowering.ixx readable");
        auto pos = low.find("void enable_soa_dual_emit");
        CHECK(pos != std::string::npos, "AC1: enable_soa_dual_emit present");
        auto win = low.substr(pos, 1200);
        CHECK(win.find("Issue #2821") != std::string::npos, "AC1: cites #2821");
        CHECK(win.find("force_reset") != std::string::npos, "AC1: force_reset param");
        CHECK(win.find("g_enable_soa_dual_emit_skip_reset_total_atomic") != std::string::npos ||
                  win.find("enable_soa_dual_emit_skip_reset") != std::string::npos,
              "AC1: skip-reset metric");
        CHECK(win.find("module_v2 = {}") != std::string::npos, "AC1: reset still on first/force");
        CHECK(soa.find("g_enable_soa_dual_emit_skip_reset_total_atomic") != std::string::npos,
              "AC1: metric in ir_soa.ixx");
    }

    // ── AC2: second enable preserves content ──
    {
        std::println("\n--- AC2: second enable preserves module_v2 ---");
        aura::ast::ASTArena arena;
        LoweringState st(arena);
        const auto skip0 = aura::compiler::g_enable_soa_dual_emit_skip_reset_total_atomic().load();

        st.enable_soa_dual_emit();
        CHECK(st.dual_emit_soa, "AC2: dual_emit on after first enable");
        CHECK(st.module_v2.functions.empty(), "AC2: fresh empty after first enable");

        // Simulate function A content after first enable.
        auto fi = st.module_v2.add_function("func_a", 2);
        auto bi = st.module_v2.add_block(fi);
        st.module_v2.add_instruction(fi, IROpcode::ConstI64, {0, 1, 0, 0});
        st.module_v2.seal_block(fi, bi);
        st.soa_instructions_emitted = 1;
        st.soa_functions_emitted = 1;
        st.cur_func_v2_idx = fi;
        CHECK(st.module_v2.functions.size() == 1, "AC2: one SoA function after A");
        CHECK(st.module_v2.functions[0].name == "func_a", "AC2: name func_a");

        // Second enable without force — must preserve.
        st.enable_soa_dual_emit();
        CHECK(st.dual_emit_soa, "AC2: still enabled");
        CHECK(st.module_v2.functions.size() == 1, "AC2: still one function (no wipe)");
        CHECK(st.module_v2.functions[0].name == "func_a", "AC2: func_a preserved");
        CHECK(st.soa_instructions_emitted == 1, "AC2: counters not zeroed");
        CHECK(st.soa_functions_emitted == 1, "AC2: func counter preserved");

        // Add function B on preserved SoA.
        auto fi2 = st.module_v2.add_function("func_b", 2);
        auto bi2 = st.module_v2.add_block(fi2);
        st.module_v2.add_instruction(fi2, IROpcode::ConstI64, {0, 2, 0, 0});
        st.module_v2.seal_block(fi2, bi2);
        CHECK(st.module_v2.functions.size() == 2, "AC2: two SoA functions after B");
        CHECK(st.module_v2.functions[0].name == "func_a", "AC2: A still present with B");

        // Third enable still no wipe.
        st.enable_soa_dual_emit(/*force_reset=*/false);
        CHECK(st.module_v2.functions.size() == 2, "AC2: third enable preserves both");

        const auto skip1 = aura::compiler::g_enable_soa_dual_emit_skip_reset_total_atomic().load();
        CHECK(skip1 >= skip0 + 2,
              std::format("AC2: skip-reset metric +2 (got Δ={})", skip1 - skip0));
    }

    // ── AC3: force_reset wipes; cold first enable resets ──
    {
        std::println("\n--- AC3: force_reset=true wipes; cold enable resets ---");
        aura::ast::ASTArena arena;
        LoweringState st(arena);
        st.enable_soa_dual_emit();
        (void)st.module_v2.add_function("keep", 1);
        CHECK(st.module_v2.functions.size() == 1, "AC3: content present");

        st.enable_soa_dual_emit(/*force_reset=*/true);
        CHECK(st.dual_emit_soa, "AC3: still enabled after force");
        CHECK(st.module_v2.functions.empty(), "AC3: force_reset wiped module_v2");
        CHECK(st.soa_instructions_emitted == 0, "AC3: counters zeroed on force");
        CHECK(st.soa_functions_emitted == 0, "AC3: func counter zeroed on force");

        // Cold path: new state, first enable clears (empty already).
        LoweringState st2(arena);
        CHECK(!st2.dual_emit_soa, "AC3: cold dual_emit off");
        st2.enable_soa_dual_emit();
        CHECK(st2.dual_emit_soa, "AC3: cold first enable turns on");
        CHECK(st2.module_v2.functions.empty(), "AC3: cold first starts empty");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2821 query keys ---");
        CompilerService cs;
        CHECK(href(cs, "schema-2821") == 2821, "AC4: schema-2821");
        CHECK(href(cs, "issue-2821") == 2821, "AC4: issue-2821");
        CHECK(href(cs, "enable-soa-dual-emit-skip-reset-wired") == 1, "AC4: wired");
        CHECK(href(cs, "enable-soa-dual-emit-skip-reset-total") >= 0, "AC4: skip total");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(obs.find("schema-2821") != std::string::npos, "AC4: obs schema-2821");
        CHECK(obs.find("enable-soa-dual-emit-skip-reset-total") != std::string::npos,
              "AC4: obs skip key");
    }

    std::println("\n=== #2821 enable_soa_dual_emit no reset: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_enable_soa_dual_emit_no_reset();
}
#endif
