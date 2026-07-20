// @category: unit
// @reason: Issue #1875 — dirty-aware EscapeAnalysisPass + post-mutation
// linear ownership/escape validation (no miss) + hit-rate metric.
//
//   AC1: source cites #1875; EscapeAnalysis dirty-aware + hit_rate metric
//   AC2: EscapeAnalysisPass dirty filter analyzes only dirty blocks
//   AC3: post_mutation_invariant_check runs full validate + bumps hit_rate
//   AC4: LinearOwnershipWrap double_consume + use_after_move counters

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.pass_manager;
import aura.compiler.type_checker;
import aura.core.ast;
import aura.core.arena;
import aura.core.type;
import aura.core.mutation;

namespace {

using aura::ast::FlatAST;
using aura::ast::InvariantStatus;
using aura::ast::NodeTag;
using aura::ast::StringPool;
using aura::compiler::CompilerMetrics;
using aura::compiler::EscapeAnalysisPass;
using aura::compiler::LinearOwnershipWrap;
using aura::compiler::OwnershipEnv;
using aura::compiler::OwnershipNote;
using aura::compiler::post_mutation_invariant_check;
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

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1875 source surface ---");
        auto pm = read_first({"src/compiler/pass_manager.ixx", "../src/compiler/pass_manager.ixx"});
        auto impl = read_first(
            {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        auto ixx =
            read_first({"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"});
        CHECK(!pm.empty() && pm.find("#1875") != std::string::npos, "pass_manager cites #1875");
        CHECK(pm.find("dirty_blocks_analyzed") != std::string::npos, "dirty_blocks_analyzed");
        CHECK(pm.find("DirtyAwarePass<EscapeAnalysisPass>") != std::string::npos,
              "EscapeAnalysisPass DirtyAware static_assert");
        CHECK(pm.find("double_consume_count") != std::string::npos, "LinearOwnershipWrap double");
        CHECK(!impl.empty() && impl.find("#1875") != std::string::npos, "impl cites #1875");
        CHECK(impl.find("linear_post_mutation_validation_hit_rate") != std::string::npos,
              "hit_rate update in post_mutation");
        CHECK(impl.find("validate_ownership_full") != std::string::npos,
              "full validate in post_mutation");
        CHECK(!hdr.empty() &&
                  hdr.find("linear_post_mutation_validation_hit_rate") != std::string::npos,
              "hit_rate metric declared");
        CHECK(hdr.find("escape_analysis_dirty_reruns_total") != std::string::npos,
              "dirty reruns metric");
        CHECK(!ixx.empty() && ixx.find("OwnershipEscapeSummary") != std::string::npos,
              "OwnershipEscapeSummary export");
    }

    // ── AC2: dirty-aware EscapeAnalysis ──
    {
        std::println("\n--- AC2: EscapeAnalysis dirty block filter ---");
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "f", .local_count = 8});
        auto& func = mod.functions.back();
        // Block 0: Const + Local (no escape)
        {
            auto& b0 = func.blocks.emplace_back();
            b0.id = 0;
            b0.instructions = {
                IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
                IRInstruction{IROpcode::Local, {1, 0, 0, 0}, 0, 1},
            };
        }
        // Block 1: Return escapes slot 1
        {
            auto& b1 = func.blocks.emplace_back();
            b1.id = 1;
            b1.instructions = {
                IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
            };
        }
        // Full analysis first.
        EscapeAnalysisPass full;
        full.run(mod);
        CHECK(func.escape_map.size() == 8, "escape_map sized");
        CHECK(full.functions_analyzed() >= 1, "functions analyzed");
        // Dirty-only: only block 0 dirty.
        EscapeAnalysisPass dirty;
        std::unordered_set<std::uint32_t> dirty_set{0};
        dirty.set_block_dirty_fn([&](std::uint32_t bi) { return dirty_set.count(bi) > 0; });
        dirty.run(func); // first: seed map + mark dirty blocks only
        CHECK(dirty.dirty_blocks_analyzed() >= 1, "dirty blocks analyzed");
        CHECK(dirty.is_block_dirty(0), "block 0 dirty");
        CHECK(!dirty.is_block_dirty(1), "block 1 clean under filter");
        // Second pass: map sized → dirty_reruns increments.
        dirty.run(func);
        CHECK(dirty.dirty_reruns() >= 1, "dirty_reruns on re-analysis");
        std::println("  dirty_blocks={} dirty_reruns={} escaped_slots={}",
                     dirty.dirty_blocks_analyzed(), dirty.dirty_reruns(),
                     dirty.escaped_slots_total());
    }

    // ── AC3: post_mutation + hit_rate ──
    {
        std::println("\n--- AC3: post_mutation full validate + hit_rate ---");
        auto arena = std::make_unique<aura::ast::ASTArena>();
        auto alloc = arena->allocator();
        auto* flat = arena->create<FlatAST>(alloc);
        auto* pool = arena->create<StringPool>(alloc);

        // (let ((x (Linear 42))) (display x)) — classic leak for post_mutate.
        auto x_sym = pool->intern("x");
        auto inner = flat->add_literal(42);
        auto lin_node = flat->add_linear(inner);
        auto disp_sym = pool->intern("display");
        auto disp_var = flat->add_variable(disp_sym);
        auto x_var = flat->add_variable(x_sym);
        aura::ast::NodeId disp_args[] = {disp_var, x_var};
        auto disp_call = flat->add_call(disp_var, disp_args);
        auto root = flat->add_let(x_sym, lin_node, disp_call);
        flat->root = root;

        TypeRegistry reg;
        CompilerMetrics metrics;
        aura::ast::MutationRecord rec{};
        rec.mutation_id = 1875;
        rec.target_node = root;
        rec.operator_name = "test";
        std::vector<OwnershipNote> notes;
        auto st = post_mutation_invariant_check(*flat, *pool, reg, rec, notes, &metrics);
        CHECK(st == InvariantStatus::Ok || st == InvariantStatus::Warnings,
              "post_mutation returns Ok or Warnings");
        CHECK(metrics.linear_post_mutation_checks_total.load() >= 1, "checks bumped");
        CHECK(metrics.linear_post_mutation_full_validate_total.load() >= 1,
              "full validate path ran");
        CHECK(metrics.linear_post_mutation_hits_total.load() >= 1, "hits bumped (linear work)");
        const auto rate = metrics.linear_post_mutation_validation_hit_rate.load();
        CHECK(rate > 0 && rate <= 100, "hit_rate in (0,100]");
        // Leak / violation should be caught (no miss).
        CHECK(metrics.linear_violations_caught_total.load() >= 1 ||
                  metrics.linear_leak_prevented_total.load() >= 1 || !notes.empty(),
              "linear violation or leak detected");
        std::println("  checks={} hits={} full={} rate={} notes={}",
                     metrics.linear_post_mutation_checks_total.load(),
                     metrics.linear_post_mutation_hits_total.load(),
                     metrics.linear_post_mutation_full_validate_total.load(), rate, notes.size());
    }

    // ── AC4: LinearOwnershipWrap counters ──
    {
        std::println("\n--- AC4: LinearOwnershipWrap double_consume ---");
        IRModule mod;
        mod.functions.push_back(IRFunction{.name = "lo", .local_count = 8});
        auto& block = mod.functions.back().blocks.emplace_back();
        block.id = 0;
        // Move slot0 twice → double consume; then use → use-after-move.
        block.instructions = {
            IRInstruction{IROpcode::ConstI64, {0, 1, 0, 0}, 0, 1},
            IRInstruction{IROpcode::MoveOp, {1, 0, 0, 0}, 0, 1},
            IRInstruction{IROpcode::MoveOp, {2, 0, 0, 0}, 0, 1}, // double consume of 0
            IRInstruction{IROpcode::Add, {3, 0, 1, 0}, 0, 1},    // use-after-move on 0
        };
        LinearOwnershipWrap wrap;
        wrap.run(mod);
        std::println("  uam={} double={} funcs={} blocks={}", wrap.use_after_move_count(),
                     wrap.double_consume_count(), wrap.functions_scanned(), wrap.blocks_scanned());
        CHECK(wrap.double_consume_count() >= 1, "double consume detected");
        CHECK(wrap.use_after_move_count() >= 1, "use-after-move detected");
        CHECK(wrap.functions_scanned() >= 1, "functions scanned");
        CHECK(wrap.has_error(), "has_error when violations");
        CHECK(LinearOwnershipWrap::lifetime_use_after_move() >= 1, "lifetime uam");
    }

    std::println("\n=== test_linear_post_mutation_escape_1875: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
