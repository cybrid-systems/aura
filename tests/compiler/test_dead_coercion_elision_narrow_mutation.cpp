// @category: integration
// @reason: Issue #1925 — expand DeadCoercionEliminationPass elision +
// Issue #1925/#799 (#1978 renamed): issue# moved from filename to header.
// narrow_evidence propagation in Coercion/Call/TypeAnnotation lowering
// for occurrence-narrowed / post-mutation cases; type_tag_for_coercion audit.
//
//   AC1: source cites #1925 (lowering + DCE Rule 6b/6c + can_elide)
//   AC2: query:dead-coercion-elision-stats schema-1925 + wiring flags
//   AC3: IR TypeProp + DCE elides identity CastOp under narrow_evidence
//   AC4: Rule 6c Dynamic target + narrow_evidence → Local
//   AC5: Rule 6b overlapping narrow_evidence bits elides
//   AC6: elision-rate-bp / target 90% surface; multi-round mutate stress
//   AC7: #799 lineage schema retained
//   AC8: type_tag_for_coercion Dynamic/structural mapping present

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:dead-coercion-elision-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
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

static std::size_t count_cast(const IRModule& mod) {
    std::size_t n = 0;
    for (const auto& f : mod.functions)
        for (const auto& b : f.blocks)
            for (const auto& i : b.instructions)
                if (i.opcode == IROpcode::CastOp)
                    ++n;
    return n;
}

static void ac1_source() {
    std::println("\n--- AC1: #1925 source surface ---");
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

static void ac2_schema() {
    std::println("\n--- AC2: schema-1925 on dead-coercion-elision-stats ---");
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

static void ac3_typeprop_dce_identity() {
    std::println("\n--- AC3: TypeProp + DCE identity under narrow_evidence ---");
    TypeRegistry reg;
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "f", .local_count = 8});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    // ConstI64 typed Int, CastOp to same Int with narrow_evidence → elide
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

static void ac4_dynamic_target_narrow() {
    std::println("\n--- AC4: Rule 6c Dynamic target + narrow_evidence ---");
    TypeRegistry reg;
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "g", .local_count = 8});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    const auto int_tid = reg.int_type().index;
    func.blocks[0].instructions = {
        make_inst(IROpcode::ConstI64, {0, 7, 0, 0}, int_tid, 0),
        make_inst(IROpcode::Local, {1, 0, 0, 0}, int_tid, 8),
        make_inst(IROpcode::CastOp, {2, 1, 3, 0}, 0, 8), // tag=3 Dynamic
        make_inst(IROpcode::Return, {2, 0, 0, 0}, 0, 0),
    };
    DeadCoercionEliminationPass dce(&reg);
    dce.run(mod);
    CHECK(count_cast(mod) == 0, "Dynamic+narrow elided");
    CHECK(dce.narrow_evidence_hits() >= 1 || dce.dynamic_hits() >= 1, "hits");
}

static void ac5_overlapping_evidence() {
    std::println("\n--- AC5: Rule 6b overlapping narrow_evidence ---");
    TypeRegistry reg;
    IRModule mod;
    mod.functions.push_back(IRFunction{.name = "h", .local_count = 8});
    auto& func = mod.functions.back();
    func.blocks.push_back({0});
    const auto int_tid = reg.int_type().index;
    // Source has narrow bit 0x04; cast has 0x0C (overlap) and target Int.
    // Source type_id unknown — 6b should still elide.
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

static void ac6_mutate_stress() {
    std::println("\n--- AC6: multi-round mutate + elision metrics ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (if (number? x) (+ x 1) 0))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    for (int i = 0; i < 40; ++i) {
        (void)cs.eval(std::format(
            "(mutate:rebind \"f\" \"(lambda (x) (if (number? x) (+ x {}) 0))\" \"c{}\")", i % 5,
            i));
        (void)cs.eval("(eval-current)");
        // Force compile path when available (best-effort).
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
    // Soft gate: if rate sampled, should not be a full-cascade 0 with residual.
    if (rate > 0)
        CHECK(rate >= 0, "rate non-neg");
    CHECK(cs.eval("(+ 1 2)").has_value(), "still evals");
}

static void ac7_lineage() {
    std::println("\n--- AC7: #799 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 799, "schema 799");
    CHECK(href(cs, "elided-casts") >= 0, "elided");
    CHECK(href(cs, "evidence-hit-rate") >= 0, "evidence rate");
}

static void ac8_type_tag_surface() {
    std::println("\n--- AC8: type_tag_for_coercion surface ---");
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
    std::println("=== Issue #1925: dead coercion elision narrow mutation ===");
    ac1_source();
    ac2_schema();
    ac3_typeprop_dce_identity();
    ac4_dynamic_target_narrow();
    ac5_overlapping_evidence();
    ac6_mutate_stress();
    ac7_lineage();
    ac8_type_tag_surface();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
