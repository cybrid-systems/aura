// test_type_propagation_dead_coercion.cpp — Issue #1874 (#1978 renamed):
// expand TypePropagationPass type_id / narrow_evidence stamping so
// DeadCoercionEliminationPass eliminates more CastOps (fixpoint rounds
// + cast_eliminated_after_propagation). Issue# moved from filename to
// header per #1978.
//
// @category: unit
// @reason: Issue #1874 — expand TypePropagationPass type_id /
// narrow_evidence stamping so DeadCoercionEliminationPass eliminates
// more CastOps (fixpoint rounds + cast_eliminated_after_propagation).
//
//   AC1: source cites #1874; expanded ops + kMaxRounds 16 + metrics
//   AC2: Const ground stamp + Local/Add/Cast chain → type_id match
//   AC3: TypeProp then DCE elides identity CastOp (eliminated + narrow)
//   AC4: fixpoint_rounds > 0; metrics fields present

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::DeadCoercionEliminationPass;
using aura::compiler::TypePropagationPass;
using aura::core::TypeRegistry;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
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

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1874 source surface ---");
        auto pm = read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        auto svc = read_first({"src/compiler/service.ixx", "../src/compiler/service.ixx"});
        CHECK(!pm.empty(), "read pass_manager.ixx");
        CHECK(pm.find("#1874") != std::string::npos, "pass_manager cites #1874");
        CHECK(pm.find("kMaxRounds = 16") != std::string::npos, "fixpoint cap raised to 16");
        CHECK(pm.find("fixpoint_rounds") != std::string::npos, "fixpoint_rounds accessor");
        CHECK(pm.find("MutBorrowOp") != std::string::npos, "MutBorrowOp in should_propagate");
        CHECK(pm.find("ConstI64") != std::string::npos, "ConstI64 ground stamp");
        CHECK(pm.find("const_ground_stamped") != std::string::npos, "const ground counter");
        CHECK(!hdr.empty() && hdr.find("type_propagation_fixpoint_rounds") != std::string::npos,
              "fixpoint_rounds metric");
        CHECK(hdr.find("cast_eliminated_after_propagation") != std::string::npos,
              "cast_eliminated_after_propagation metric");
        CHECK(!svc.empty() && svc.find("type_propagation_fixpoint_rounds") != std::string::npos,
              "service wires fixpoint_rounds");
        CHECK(svc.find("cast_eliminated_after_propagation") != std::string::npos,
              "service wires cast_eliminated_after_propagation");
    }

    // ── AC2: const ground + chain ──
    {
        std::println("\n--- AC2: Const ground stamp + Local/Add chain ---");
        TypeRegistry reg;
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "t", .local_count = 16});
        auto& block = mod.functions.back().blocks.emplace_back();
        block.id = 0;
        // ConstI64 without type_id — #1874 stamps Int from reg.
        block.instructions = {
            IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 0},
            IRInstruction{IROpcode::ConstI64, {1, 2, 0, 0}, 0, 0},
            IRInstruction{IROpcode::Add, {2, 0, 1, 0}, 0, 0},
            IRInstruction{IROpcode::Local, {3, 2, 0, 0}, 0, 0},
            IRInstruction{IROpcode::Return, {3, 0, 0, 0}, 0, 0},
        };
        TypePropagationPass tp(&reg);
        tp.run(mod);
        std::println("  propagated={} const_ground={} rounds={} ext={}", tp.propagated_count(),
                     tp.const_ground_stamped(), tp.fixpoint_rounds(), tp.extended_ops_propagated());
        CHECK(tp.const_ground_stamped() >= 2, "ConstI64 ground stamps");
        CHECK(block.instructions[0].type_id == reg.int_type().index, "const0 Int");
        CHECK(block.instructions[1].type_id == reg.int_type().index, "const1 Int");
        CHECK(block.instructions[2].type_id == reg.int_type().index, "Add Int");
        CHECK(block.instructions[3].type_id == reg.int_type().index, "Local Int");
        CHECK(tp.fixpoint_rounds() >= 1, "fixpoint rounds counted");
        CHECK(tp.propagated_count() >= 3, "propagated consts+add+local");
    }

    // ── AC3: TypeProp → DCE identity / narrow CastOp ──
    {
        std::println("\n--- AC3: TypeProp then DCE elides CastOp ---");
        TypeRegistry reg;
        const auto int_id = static_cast<std::uint32_t>(reg.int_type().index);

        // Identity cast with type_id match after prop.
        {
            IRModule mod;
            mod.functions.push_back(IRFunction{.name = "id_cast", .local_count = 16});
            auto& block = mod.functions.back().blocks.emplace_back();
            block.id = 0;
            // ConstI64 typed Int; CastOp to same type (tag 0 / type_id Int).
            block.instructions = {
                IRInstruction{IROpcode::ConstI64, {0, 42, 0, 0}, 0, int_id},
                IRInstruction{IROpcode::CastOp, {1, 0, 0, 0}, 0, int_id}, // identity
                IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            };
            CHECK(count_cast(mod) == 1, "one CastOp before");
            TypePropagationPass tp(&reg);
            tp.run(mod);
            DeadCoercionEliminationPass dce(&reg);
            dce.run(mod);
            std::println("  identity: eliminated={} type_prop_hits={} casts_left={}",
                         dce.eliminated_count(), dce.type_prop_hits(), count_cast(mod));
            CHECK(dce.eliminated_count() >= 1, "identity cast eliminated");
            CHECK(count_cast(mod) == 0, "no CastOp residual");
        }

        // Rule 6: narrow_evidence-proved cast.
        {
            IRModule mod;
            mod.functions.push_back(IRFunction{.name = "narrow_cast", .local_count = 16});
            auto& block = mod.functions.back().blocks.emplace_back();
            block.id = 0;
            IRInstruction cast{};
            cast.opcode = IROpcode::CastOp;
            cast.operands = {1, 0, 0, 0};
            cast.type_id = int_id;
            cast.narrow_evidence = 4; // kNarrowNumber-ish
            block.instructions = {
                IRInstruction{IROpcode::ConstI64, {0, 7, 0, 0}, 0, int_id},
                cast,
                IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            };
            TypePropagationPass tp(&reg);
            tp.run(mod);
            DeadCoercionEliminationPass dce(&reg);
            dce.run(mod);
            std::println("  narrow: eliminated={} narrow_hits={}", dce.eliminated_count(),
                         dce.narrow_evidence_hits());
            CHECK(dce.eliminated_count() >= 1, "narrow cast eliminated");
            CHECK(dce.narrow_evidence_hits() >= 1 || dce.type_prop_hits() >= 1,
                  "narrow or type_prop hit");
            CHECK(count_cast(mod) == 0, "no CastOp residual after narrow DCE");
        }

        // MutBorrow unary prop (#1874 expansion).
        {
            IRModule mod;
            mod.functions.push_back(IRFunction{.name = "mut_borrow", .local_count = 16});
            auto& block = mod.functions.back().blocks.emplace_back();
            block.id = 0;
            block.instructions = {
                IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, int_id},
                IRInstruction{IROpcode::MutBorrowOp, {1, 0, 0, 0}, 0, 0},
                IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            };
            TypePropagationPass tp(&reg);
            tp.run(mod);
            CHECK(block.instructions[1].type_id == int_id, "MutBorrowOp type_id stamped");
            CHECK(tp.extended_ops_propagated() >= 1, "extended op counted");
        }
    }

    // ── AC4: metrics fields ──
    {
        std::println("\n--- AC4: metrics fields ---");
        CompilerMetrics m;
        CHECK(m.type_propagation_fixpoint_rounds.load() == 0, "fixpoint starts 0");
        CHECK(m.cast_eliminated_after_propagation.load() == 0, "cast_elim starts 0");
        m.type_propagation_fixpoint_rounds.fetch_add(3, std::memory_order_relaxed);
        m.cast_eliminated_after_propagation.fetch_add(2, std::memory_order_relaxed);
        CHECK(m.type_propagation_fixpoint_rounds.load() == 3, "fixpoint bump");
        CHECK(m.cast_eliminated_after_propagation.load() == 2, "cast_elim bump");
    }

    std::println("\n=== test_type_propagation_dead_coercion_1874: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
