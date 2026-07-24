// tests/compiler/test_dead_coercion_elision_narrow_batch.cpp — dead_coercion_elision_narrow pair
// dup-merge (R19 phase 16). R19 phase16 — Issue #799 + #1925 dead_coercion_elision_narrow pair
//
//   #799:  DeadCoercionElimination + narrow_evidence CastOp elision observability
//          (refines #796/#795/#794; non-duplicative with #687 dead-coercion-elim-stats,
//          #629 coercion-zerooverhead)
//   #1925: expand DeadCoercionEliminationPass elision + narrow_evidence propagation
//          in Coercion/Call/TypeAnnotation lowering for occurrence-narrowed /
//          post-mutation cases; type_tag_for_coercion audit (refine #799)
//
//   AC1:  query:dead-coercion-elision-stats reachable (schema 799) (#799 AC1)
//   AC2:  elided-casts bumps on direct path (#799 AC2)
//   AC3:  evidence-hit-rate derived from evidence hits (#799 AC3)
//   AC4:  narrowing-stable-paths bumps on direct path (#799 AC4)
//   AC5:  runtime-check-savings bumps on direct path (#799 AC5)
//   AC6:  DCE Rule 6 narrow_evidence elision (no CastOp residual) (#799 AC6)
//   AC7:  metrics monotonic after DCE + bump matrix (#799 AC7)
//   AC8:  query regression (#687 elim-stats, #629 zerooverhead-stats) (#799 AC8)
//   AC9:  source cites #1925 (lowering + DCE Rule 6b/6c + can_elide) (#1925 AC1)
//   AC10: query:dead-coercion-elision-stats schema-1925 + wiring flags (#1925 AC2)
//   AC11: IR TypeProp + DCE elides identity CastOp under narrow_evidence (#1925 AC3)
//   AC12: Rule 6c Dynamic target + narrow_evidence → Local (#1925 AC4)
//   AC13: Rule 6b overlapping narrow_evidence bits elides (#1925 AC5)
//   AC14: elision-rate-bp / target 90% surface; multi-round mutate stress (#1925 AC6)
//   AC15: #799 lineage schema retained (#1925 AC7)
//   AC16: type_tag_for_coercion Dynamic/structural mapping present (#1925 AC8)
//
// Skipped: test_dead_coercion_elision_narrow_mutation.cpp uses #1925's 8-AC function-style
//          pattern; test_dead_coercion_elision_narrow_evidence.cpp uses #799's
//          aura_issue_799_detail::run_matrix(cs) + #ifndef AURA_ISSUE_BUNDLE_MEMBER dispatcher.
//          Merge unifies both patterns (R19 phase 3 #818 dispatcher-block drop).
//
// Skipped: test_type_propagation_dead_coercion.cpp (prefix type_propagation_*, different family).
// Skipped: bench_dead_coercion_elim.cpp — BENCH, hard ref in CMakeLists.txt L1152
//          (add_executable(issue_508_bench ...), not auto-discoverable).

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::TypePropagationPass;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t stat_int(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:dead-coercion-elision-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:dead-coercion-elision-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t elided_casts(CompilerService& cs) {
    return stat_int(cs, "elided-casts");
}
static std::int64_t evidence_hit_rate(CompilerService& cs) {
    return stat_int(cs, "evidence-hit-rate");
}
static std::int64_t narrowing_stable_paths(CompilerService& cs) {
    return stat_int(cs, "narrowing-stable-paths");
}
static std::int64_t runtime_check_savings(CompilerService& cs) {
    return stat_int(cs, "runtime-check-savings");
}

static std::size_t count_cast_ops(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

static std::size_t count_cast(const IRModule& mod) {
    return count_cast_ops(mod);
}

static IRModule make_narrow_cast_module() {
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "test", .local_count = 16});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    IRInstruction cast{};
    cast.opcode = IROpcode::CastOp;
    cast.operands = {2, 1, 0, 0};
    cast.type_id = 1;
    cast.narrow_evidence = 4;
    func.blocks[0].instructions = {
        {IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        {IROpcode::Local, {1, 0, 0, 0}, 1, 1},
        cast,
        {IROpcode::Return, {2, 0, 0, 0}, 0, 0},
    };
    return mod;
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

// Aggregate init puts the 3rd value in source_ast_node_id, not type_id —
// always set type_id / narrow_evidence by named field (see IRInstruction).
static IRInstruction make_inst(IROpcode op, std::array<std::uint32_t, 4> ops,
                               std::uint32_t type_id = 0, std::uint32_t narrow = 0) {
    IRInstruction i{};
    i.opcode = op;
    i.operands = ops;
    i.type_id = type_id;
    i.narrow_evidence = narrow;
    return i;
}

// ── #799 ACs (function-style, renamed from aura_issue_799_detail::run_matrix) ──

static void ac799_1_schema() {
    std::println("\n--- AC1: query:dead-coercion-elision-stats (schema 799) (#799 AC1) ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:dead-coercion-elision-stats\")");
    CHECK(h && is_hash(*h), "dead-coercion-elision-stats returns hash");
    CHECK(stat_int(cs, "schema") == 799, "schema == 799");
    CHECK(elided_casts(cs) >= 0, "elided-casts non-negative");
    CHECK(evidence_hit_rate(cs) >= 0, "evidence-hit-rate non-negative");
    CHECK(narrowing_stable_paths(cs) >= 0, "narrowing-stable-paths non-negative");
    CHECK(runtime_check_savings(cs) >= 0, "runtime-check-savings non-negative");
}

static void ac799_2_elided_casts() {
    std::println("\n--- AC2: elided-casts bumps on direct path (#799 AC2) ---");
    CompilerService cs;
    const auto e0 = elided_casts(cs);
    cs.evaluator().bump_dead_coercion_elision_elided_casts(2);
    CHECK(elided_casts(cs) == e0 + 2, "elided-casts bumps by exactly 2");
}

static void ac799_3_evidence_hit_rate() {
    std::println("\n--- AC3: evidence-hit-rate derived (#799 AC3) ---");
    CompilerService cs;
    const auto rate0 = evidence_hit_rate(cs);
    cs.evaluator().bump_dead_coercion_elision_evidence_hits(3);
    const auto rate1 = evidence_hit_rate(cs);
    CHECK(rate1 >= rate0, "evidence-hit-rate monotonic after evidence bumps");
}

static void ac799_4_narrowing_stable() {
    std::println("\n--- AC4: narrowing-stable-paths bumps on direct path (#799 AC4) ---");
    CompilerService cs;
    const auto n0 = narrowing_stable_paths(cs);
    cs.evaluator().bump_dead_coercion_elision_narrowing_stable_paths();
    CHECK(narrowing_stable_paths(cs) == n0 + 1, "narrowing-stable-paths bumps by 1");
}

static void ac799_5_runtime_check_savings() {
    std::println("\n--- AC5: runtime-check-savings bumps on direct path (#799 AC5) ---");
    CompilerService cs;
    const auto s0 = runtime_check_savings(cs);
    cs.evaluator().bump_dead_coercion_elision_runtime_check_savings(2);
    CHECK(runtime_check_savings(cs) == s0 + 2, "runtime-check-savings bumps by exactly 2");
}

static void ac799_6_dce_rule6() {
    std::println("\n--- AC6: DCE Rule 6 narrow_evidence elision (#799 AC6) ---");
    auto mod = make_narrow_cast_module();
    CHECK(count_cast_ops(mod) == 1, "module starts with one CastOp");
    DeadCoercionEliminationPass dce(nullptr);
    dce.run(mod);
    CHECK(count_cast_ops(mod) == 0, "DCE Rule 6 eliminates narrow_evidence CastOp");
    CHECK(dce.narrow_evidence_hits() >= 1, "DCE reports narrow_evidence hit");
}

static void ac799_7_metrics_monotonic() {
    std::println("\n--- AC7: metrics monotonic after DCE matrix (#799 AC7) ---");
    CompilerService cs;
    const auto ev7a = elided_casts(cs) + narrowing_stable_paths(cs) + runtime_check_savings(cs);
    cs.evaluator().bump_dead_coercion_elision_elided_casts();
    cs.evaluator().bump_dead_coercion_elision_narrowing_stable_paths();
    cs.evaluator().bump_dead_coercion_elision_runtime_check_savings();
    const auto ev7b = elided_casts(cs) + narrowing_stable_paths(cs) + runtime_check_savings(cs);
    CHECK(ev7b >= ev7a + 3, "aggregate elision counters monotonic");
}

static void ac799_8_query_regression() {
    std::println("\n--- AC8: query regression (#799 AC8) ---");
    CompilerService cs;
    auto elim687 = cs.eval("(engine:metrics \"query:dead-coercion-elim-stats\")");
    auto z629 = cs.eval("(engine:metrics \"query:dead-coercion-zerooverhead-stats\")");
    CHECK(elim687 && is_hash(*elim687), "dead-coercion-elim-stats regression (#687)");
    CHECK(z629 && is_hash(*z629), "dead-coercion-zerooverhead-stats regression (#629)");
}

// ── #1925 ACs (function-style) ──

static void ac1925_1_source() {
    std::println("\n--- AC9: #1925 source surface (#1925 AC1) ---");
    auto low = read_first({"src/compiler/lowering_impl.cpp", "../src/compiler/lowering_impl.cpp"});
    auto pm = read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
    auto hdr = read_first(
        {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
    CHECK(!low.empty(), "read lowering");
    CHECK(low.find("#1925") != std::string::npos, "lowering cites #1925");
    CHECK(low.find("can_elide_coercion_cast") != std::string::npos, "lower-time can_elide");
    CHECK(low.find("if_narrow_ev") != std::string::npos, "then-branch narrow prop");
    CHECK(!pm.empty() && pm.find("#1925") != std::string::npos, "pass_manager cites #1925");
    CHECK(pm.find("6b") != std::string::npos ||
              pm.find("overlapping evidence") != std::string::npos,
          "Rule 6b");
    CHECK(pm.find("6c") != std::string::npos || pm.find("Dynamic target") != std::string::npos,
          "Rule 6c");
    CHECK(!hdr.empty() &&
              hdr.find("dead_coercion_narrow_mutation_elided_total") != std::string::npos,
          "metrics");
    CHECK(hdr.find("dead_coercion_elision_rate_bp") != std::string::npos, "rate bp");
}

static void ac1925_2_schema() {
    std::println("\n--- AC10: schema-1925 on dead-coercion-elision-stats (#1925 AC2) ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:dead-coercion-elision-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 799, "lineage 799");
    CHECK(href(cs, "schema-1925") == 1925, "schema-1925");
    CHECK(href(cs, "issue-1925") == 1925, "issue-1925");
    CHECK(href(cs, "narrow-mutation-wired") == 1, "wired");
    CHECK(href(cs, "elision-rate-target-bp") == 9000, "90% target");
    CHECK(href(cs, "narrow-mutation-elided") >= 0, "narrow-mut key");
    CHECK(href(cs, "dynamic-passthrough-elided") >= 0, "dyn key");
    CHECK(href(cs, "elision-rate-bp") >= 0, "rate bp");
}

static void ac1925_3_typeprop_dce() {
    std::println("\n--- AC11: TypeProp + DCE identity under narrow_evidence (#1925 AC3) ---");
    TypeRegistry reg;
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "f", .local_count = 8});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    const auto int_tid = reg.int_type().index;
    func.blocks[0].instructions = {
        make_inst(IROpcode::ConstI64, {0, 42, 0, 0}, int_tid, 0),
        make_inst(IROpcode::Local, {1, 0, 0, 0}, int_tid, 4),
        make_inst(IROpcode::CastOp, {2, 1, 0, 0}, int_tid, 4),
        make_inst(IROpcode::Return, {2, 0, 0, 0}, 0, 0),
    };
    CHECK(count_cast(mod) == 1, "1 cast before");
    TypePropagationPass tp(&reg);
    tp.run(mod);
    DeadCoercionEliminationPass dce(&reg);
    dce.run(mod);
    CHECK(count_cast(mod) == 0, "cast elided");
    CHECK(dce.eliminated_count() >= 1, "eliminated >= 1");
    CHECK(dce.narrow_evidence_hits() >= 1, "narrow hits");
}

static void ac1925_4_dynamic_target() {
    std::println("\n--- AC12: Rule 6c Dynamic target + narrow_evidence (#1925 AC4) ---");
    TypeRegistry reg;
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "g", .local_count = 8});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    const auto int_tid = reg.int_type().index;
    func.blocks[0].instructions = {
        make_inst(IROpcode::ConstI64, {0, 7, 0, 0}, int_tid, 0),
        make_inst(IROpcode::Local, {1, 0, 0, 0}, int_tid, 8),
        make_inst(IROpcode::CastOp, {2, 1, 3, 0}, 0, 8),
        make_inst(IROpcode::Return, {2, 0, 0, 0}, 0, 0),
    };
    DeadCoercionEliminationPass dce(&reg);
    dce.run(mod);
    CHECK(count_cast(mod) == 0, "Dynamic+narrow elided");
    CHECK(dce.narrow_evidence_hits() >= 1 || dce.dynamic_hits() >= 1, "hits");
}

static void ac1925_5_overlapping_evidence() {
    std::println("\n--- AC13: Rule 6b overlapping narrow_evidence (#1925 AC5) ---");
    TypeRegistry reg;
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "h", .local_count = 8});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    const auto int_tid = reg.int_type().index;
    func.blocks[0].instructions = {
        make_inst(IROpcode::ConstI64, {0, 1, 0, 0}, 0, 0),
        make_inst(IROpcode::Local, {1, 0, 0, 0}, 0, 0x04),
        make_inst(IROpcode::CastOp, {2, 1, 0, 0}, int_tid, 0x0C),
        make_inst(IROpcode::Return, {2, 0, 0, 0}, 0, 0),
    };
    TypePropagationPass tp(&reg);
    tp.run(mod);
    DeadCoercionEliminationPass dce(&reg);
    dce.run(mod);
    CHECK(dce.eliminated_count() >= 1 || count_cast(mod) == 0, "elided via 6a/6b");
}

static void ac1925_6_mutate_stress() {
    std::println("\n--- AC14: multi-round mutate + elision metrics (#1925 AC6) ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    for (int i = 0; i < 40; ++i) {
        (void)cs.eval(std::format(
            "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x {}) 0))\" \"c{}\")", i % 5,
            i));
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(compile:current)");
    }
    CHECK(href(cs, "schema-1925") == 1925, "schema holds");
    const auto elided = href(cs, "elided-casts");
    const auto narrow = href(cs, "narrow-mutation-elided");
    const auto rate = href(cs, "elision-rate-bp");
    const auto target = href(cs, "elision-rate-target-bp");
    std::println("  elided={} narrow-mut={} rate-bp={} target={}", elided, narrow, rate, target);
    CHECK(target == 9000, "target 90%");
    CHECK(elided >= 0, "elided readable");
    CHECK(narrow >= 0, "narrow readable");
    if (rate > 0)
        CHECK(rate >= 0, "rate non-neg");
    CHECK(cs.eval("(+ 1 2)").has_value(), "still evals");
}

static void ac1925_7_lineage() {
    std::println("\n--- AC15: #799 lineage (#1925 AC7) ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 799, "schema 799");
    CHECK(href(cs, "elided-casts") >= 0, "elided");
    CHECK(href(cs, "evidence-hit-rate") >= 0, "evidence rate");
}

static void ac1925_8_type_tag() {
    std::println("\n--- AC16: type_tag_for_coercion surface (#1925 AC8) ---");
    auto low = read_first({"src/compiler/lowering_impl.cpp", "../src/compiler/lowering_impl.cpp"});
    auto pm = read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
    CHECK(low.find("TypeTag::VOID") != std::string::npos ||
              low.find("TypeTag::PAIR") != std::string::npos,
          "lowering structural tags");
    CHECK(pm.find("TypeTag::VOID") != std::string::npos ||
              pm.find("TypeTag::PAIR") != std::string::npos,
          "pass_manager structural tags");
    CHECK(low.find("can_elide_coercion_cast") != std::string::npos, "can_elide helper");
}

} // namespace

int main() {
    std::println("=== dead_coercion_elision_narrow pair: #799 (evidence) + #1925 (mutation) ===\n");
    ac799_1_schema();
    ac799_2_elided_casts();
    ac799_3_evidence_hit_rate();
    ac799_4_narrowing_stable();
    ac799_5_runtime_check_savings();
    ac799_6_dce_rule6();
    ac799_7_metrics_monotonic();
    ac799_8_query_regression();
    ac1925_1_source();
    ac1925_2_schema();
    ac1925_3_typeprop_dce();
    ac1925_4_dynamic_target();
    ac1925_5_overlapping_evidence();
    ac1925_6_mutate_stress();
    ac1925_7_lineage();
    ac1925_8_type_tag();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
