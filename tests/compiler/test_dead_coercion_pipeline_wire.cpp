// @category: unit
// @reason: Issue #2025 — explicit DeadCoercionEliminationPass in the default
// optimization pipeline + narrow_evidence / AST elision synergy.
//
//   AC1: source cites #2025; PassKind::DeadCoercion + DeadCoercionPass +
//        run_default_optimization_pipeline includes DCE after TypeProp
//   AC2: run_pass_kind(DeadCoercion) elides identity CastOp
//   AC3: run_default_optimization_pipeline notes dead-coercion-runs
//   AC4: AST identity elision bumps g_dead_coercion_ast_elided_total
//   AC5: query:optimization-passes-stats schema-2025 + layered keys
//   AC6: IR narrow_evidence CastOp elided; metrics ir-elided grow
//   AC7: existing service coerce path still works (no regression)

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.ir;
import aura.compiler.optimization_passes;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::compiler::apply_coercion_map;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::g_dead_coercion_ast_elided_total;
using aura::compiler::opt_registry::dead_coercion_ir_elided_total;
using aura::compiler::opt_registry::dead_coercion_pipeline_runs_total;
using aura::compiler::opt_registry::default_pass_count;
using aura::compiler::opt_registry::find_descriptor;
using aura::compiler::opt_registry::PassKind;
using aura::compiler::opt_registry::run_default_optimization_pipeline;
using aura::compiler::opt_registry::run_pass_kind;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
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

static std::size_t count_cast(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

// Minimal IR: ConstI64 → CastOp (identity type_id) → Return.
// IRInstruction layout: opcode, operands, shape_id, type_id, ... narrow_evidence.
static IRModule make_identity_cast_module() {
    IRModule mod;
    IRFunction fn;
    fn.name = "dce_test";
    fn.local_count = 2;
    fn.arg_count = 0;
    fn.entry_block = 0;
    aura::ir::BasicBlock blk;
    blk.id = 0;
    // Match test_type_propagation_dead_coercion positional init style.
    blk.instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
        IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1}, // identity
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    fn.blocks.push_back(std::move(blk));
    mod.functions.push_back(std::move(fn));
    mod.entry_function_id = 0;
    return mod;
}

// CastOp with narrow_evidence + matching source type (Rule 6a).
static IRModule make_narrow_cast_module() {
    IRModule mod;
    IRFunction fn;
    fn.name = "dce_narrow";
    fn.local_count = 2;
    fn.arg_count = 0;
    fn.entry_block = 0;
    aura::ir::BasicBlock blk;
    blk.id = 0;
    IRInstruction c{IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1};
    c.narrow_evidence = 0x3;
    IRInstruction cast{IROpcode::CastOp, {1, 0, 0, 0}, 0, 1};
    cast.narrow_evidence = 0x3;
    blk.instructions = {
        c,
        cast,
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    fn.blocks.push_back(std::move(blk));
    mod.functions.push_back(std::move(fn));
    mod.entry_function_id = 0;
    return mod;
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2025 ---");
    auto opt = read_file("src/compiler/optimization_passes.ixx");
    auto cm = read_file("src/compiler/coercion_map.ixx");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!opt.empty(), "optimization_passes.ixx readable");
    CHECK(opt.find("Issue #2025") != std::string::npos, "opt_passes cites #2025");
    CHECK(opt.find("DeadCoercionPass") != std::string::npos, "DeadCoercionPass wrapper");
    CHECK(opt.find("run_default_optimization_pipeline") != std::string::npos, "default pipeline");
    CHECK(opt.find("case PassKind::DeadCoercion") != std::string::npos, "run_pass_kind case");
    CHECK(find_descriptor(PassKind::DeadCoercion) != nullptr, "descriptor registered");
    CHECK(find_descriptor(PassKind::DeadCoercion)->name == "dead-coercion-elim", "name");
    CHECK(default_pass_count() >= 10, "table includes DeadCoercion");
    CHECK(!cm.empty() && cm.find("g_dead_coercion_ast_elided_total") != std::string::npos,
          "AST elision counter");
    CHECK(!q.empty() && q.find("schema-2025") != std::string::npos, "query schema-2025");
}

static void ac2_run_pass_kind() {
    std::println("\n--- AC2: run_pass_kind(DeadCoercion) elides identity Cast ---");
    auto mod = make_identity_cast_module();
    const auto casts0 = count_cast(mod);
    CHECK(casts0 == 1, "one CastOp before");
    const auto ir0 = dead_coercion_ir_elided_total.load();
    CHECK(run_pass_kind(mod, PassKind::DeadCoercion), "run_pass_kind ok");
    const auto casts1 = count_cast(mod);
    CHECK(casts1 == 0, "CastOp elided");
    CHECK(dead_coercion_ir_elided_total.load() > ir0, "IR elided counter grew");
}

static void ac3_default_pipeline() {
    std::println("\n--- AC3: default pipeline runs DeadCoercion ---");
    auto mod = make_identity_cast_module();
    const auto pipe0 = dead_coercion_pipeline_runs_total.load();
    CHECK(run_default_optimization_pipeline(mod), "default pipeline ok");
    CHECK(dead_coercion_pipeline_runs_total.load() > pipe0, "pipeline DCE ran");
    CHECK(count_cast(mod) == 0, "default pipeline elided CastOp");
}

static void ac4_ast_elision() {
    std::println("\n--- AC4: AST identity elision counter ---");
    FlatAST flat;
    StringPool pool;
    auto lit = flat.add_literal(3);
    flat.set_type(lit, 1);
    CoercionMap map;
    map.add(NULL_NODE, 0, lit, 1, 1, 0, 0);
    const auto a0 = g_dead_coercion_ast_elided_total.load();
    const auto applied = apply_coercion_map(flat, map);
    CHECK(applied == 0, "AST identity elided");
    CHECK(g_dead_coercion_ast_elided_total.load() > a0, "AST elided counter grew");
}

static void ac5_query_keys() {
    std::println("\n--- AC5: query:optimization-passes-stats schema-2025 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:optimization-passes-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 2025 || href(cs, "schema") == 1576, "schema 2025|1576");
    CHECK(href(cs, "schema-2025") == 2025, "schema-2025");
    CHECK(href(cs, "issue-2025") == 2025, "issue-2025");
    CHECK(href(cs, "dead-coercion-wired") == 1, "wired");
    CHECK(href(cs, "dead-coercion-runs") >= 0, "runs");
    CHECK(href(cs, "dead-coercion-ir-elided") >= 0, "ir-elided");
    CHECK(href(cs, "dead-coercion-ast-elided") >= 0, "ast-elided");
    CHECK(href(cs, "dead-coercion-layered-total") >= 0, "layered");
    CHECK(href(cs, "default-table-count") >= 10, "table count");
}

static void ac6_narrow_evidence() {
    std::println("\n--- AC6: narrow_evidence CastOp elision ---");
    auto mod = make_narrow_cast_module();
    const auto ir0 = dead_coercion_ir_elided_total.load();
    CHECK(count_cast(mod) == 1, "one cast before");
    CHECK(run_pass_kind(mod, PassKind::DeadCoercion), "DCE ok");
    CHECK(count_cast(mod) == 0, "narrow cast elided");
    CHECK(dead_coercion_ir_elided_total.load() > ir0, "IR elided grew");
}

static void ac7_service_no_regression() {
    std::println("\n--- AC7: service coerce path no regression ---");
    CompilerService cs;
    CHECK(cs.eval("(let ((x 5)) (+ x 3))").has_value(), "let + arith");
    CHECK(cs.eval("(if (number? 5) 1 0)").has_value(), "occurrence predicate");
    auto h = cs.eval("(engine:metrics \"query:optimization-passes-stats\")");
    CHECK(h && is_hash(*h), "stats still hash after eval");
    CHECK(href(cs, "dead-coercion-wired") == 1, "wired after eval");
}

} // namespace

int main() {
    ac1_source();
    ac2_run_pass_kind();
    ac3_default_pipeline();
    ac4_ast_elision();
    ac5_query_keys();
    ac6_narrow_evidence();
    ac7_service_no_regression();
    if (g_failed)
        return 1;
    std::println("DeadCoercion pipeline wire (#2025): OK ({} passed)", g_passed);
    return 0;
}
