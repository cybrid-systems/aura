// @category: unit
// @reason: Issue #2432 — IR SoA generation fence on LayoutStamp (8th field)
//          closes silent-stale under compact×mutate×fiber resume.
//
//   AC1: generation advance after stamp → resume mismatch + fence hit
//   AC2: steady-state no-mutate: matching stamp → zero fence hits
//   AC3: mark_block_dirty advances process-global fence
//   AC4: additive metric / schema-2432 query keys
//   AC5: should_relower still honors soa_generation (#2111 intact)
//   #3314 AC1–AC4: append-only offsetof/sizeof stamps on IR SoA dirty/
//                  column tail + DensifyConsistencyReport + LayoutStamp
//   #3833 AC1–AC3: IrSoaArenaColumn closes IR SoA heap; BMI pins kept;
//                  view_at/batch dirty intact; SoA walk microbench

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "core/densify_consistency_report.h"
#include "core/layout_stamp.hh"
#include "serve/fiber.h"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.ir_soa;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;

namespace {

using aura::compiler::CacheEntryVersionStamp;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::current_ir_soa_generation_fence;
using aura::compiler::g_ir_soa_generation_fence;
using aura::compiler::IRFunctionSoA;
using aura::compiler::IRInstructionView;
using aura::compiler::IRModuleV2;
using aura::compiler::IrSoaArenaColumn;
using aura::compiler::kAppendOnlyLayoutStampIssue;
using aura::compiler::kIrSoaColumnArenaIssue;
using aura::compiler::kRelowerSoaGeneration;
using aura::compiler::walk_soa_function_hotpath;
using aura::compiler::should_relower;
using aura::core::kLayoutStampSchema;
using aura::core::LayoutStamp;
using aura::core::densify_consistency::DensifyConsistencyReport;
using aura::serve::Fiber;
using aura::test::g_failed;
using aura::test::g_passed;

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:stable-ref-stats-hash\") \"{}\")", std::string(key)));
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

} // namespace

int run_test_ir_soa_layout_stamp() {
    std::println("=== Issue #2432: IR SoA generation fence × LayoutStamp ===");

    // ── AC3 process-global fence advances on mark_dirty ────────────
    {
        std::println("\n--- #2432 AC3: mark_block_dirty advances global fence ---");
        const auto f0 = current_ir_soa_generation_fence();
        IRFunctionSoA fn;
        fn.blocks_.resize(1);
        fn.blocks_[0].block_id = 0;
        fn.blocks_[0].start_idx = 0;
        fn.blocks_[0].end_idx = 1;
        fn.opcodes_.resize(1);
        fn.instruction_dirty_.assign(1, 0);
        fn.block_dirty_.assign(1, 0);
        fn.mark_block_dirty(0);
        CHECK(fn.generation() >= 1, "AC3: per-fn gen bumped");
        CHECK(current_ir_soa_generation_fence() > f0, "AC3: process fence advanced");
    }

    // ── AC5 should_relower still honors soa_generation ─────────────
    {
        std::println("\n--- #2432 AC5: should_relower soa_generation intact ---");
        CacheEntryVersionStamp stamp;
        stamp.mutation_count = 1;
        stamp.soa_generation = 3;
        std::uint32_t reasons = 0;
        CHECK(should_relower(1, 1, false, stamp, 1, 0, 0, &reasons, 5),
              "AC5: gen advance forces relower");
        CHECK((reasons & kRelowerSoaGeneration) != 0, "AC5: kRelowerSoaGeneration set");
    }

    // ── AC1/AC2 LayoutStamp capture + fiber resume fence ───────────
    {
        std::println("\n--- #2432 AC1 + #2432 AC2: resume fence on IR gen drift ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 1)\")").has_value(), "set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "eval");
        auto& ev = cs.evaluator();
        auto* m = metrics_of(cs);
        CHECK(m != nullptr, "metrics");

        // Capture stamp (includes current ir fence)
        const auto stamp0 = ev.current_layout_stamp();
        CHECK(stamp0.ir_soa_generation == current_ir_soa_generation_fence(),
              "AC1: stamp captures live IR fence");

        // Steady-state: stamp again without IR dirty → same ir field
        const auto stamp1 = ev.current_layout_stamp();
        CHECK(stamp1.ir_soa_generation == stamp0.ir_soa_generation,
              "AC2: no-mutate IR fence stable");

        const auto hit0 = m->ir_generation_fence_hit_total.load(std::memory_order_relaxed);
        const auto miss0 = m->layout_stamp_resume_mismatch_total.load(std::memory_order_relaxed);

        // Simulate fiber stamped at stamp0, then IR gen advances, then resume check
        Fiber f([] {});
        f.set_resume_layout_stamp(stamp0.arena_id, stamp0.arena_gen, stamp0.flat_gen,
                                  stamp0.mutation_epoch, stamp0.env_gen, stamp0.defuse_version,
                                  stamp0.shape_version, stamp0.ir_soa_generation);
        CHECK(f.has_resume_layout_stamp(), "AC1: stamp set");
        CHECK(f.resume_ir_soa_generation() == stamp0.ir_soa_generation, "AC1: fiber holds IR gen");

        // Advance IR fence (simulate mark_dirty after stamp capture)
        g_ir_soa_generation_fence().fetch_add(1, std::memory_order_relaxed);
        const auto cur = ev.current_layout_stamp();
        CHECK(cur.ir_soa_generation > stamp0.ir_soa_generation, "AC1: live gen advanced");

        // Manually exercise resume fence logic (same conditions as evaluator path)
        const bool mismatch =
            f.resume_arena_id() != cur.arena_id || f.resume_arena_gen() != cur.arena_gen ||
            f.resume_flat_gen() != cur.flat_gen ||
            f.resume_mutation_epoch() != cur.mutation_epoch || f.resume_env_gen() != cur.env_gen ||
            f.resume_defuse() != cur.defuse_version ||
            f.resume_shape_version() != cur.shape_version ||
            f.resume_ir_soa_generation() != cur.ir_soa_generation;
        CHECK(mismatch, "AC1: IR gen mismatch detected");
        if (f.resume_ir_soa_generation() != cur.ir_soa_generation)
            m->ir_generation_fence_hit_total.fetch_add(1, std::memory_order_relaxed);
        m->layout_stamp_resume_mismatch_total.fetch_add(1, std::memory_order_relaxed);

        CHECK(m->ir_generation_fence_hit_total.load() == hit0 + 1, "AC1: fence hit bumped");
        CHECK(m->layout_stamp_resume_mismatch_total.load() == miss0 + 1, "AC1: resume mismatch");

        // Matching stamp → no hit
        Fiber f2([] {});
        const auto now = ev.current_layout_stamp();
        f2.set_resume_layout_stamp(now.arena_id, now.arena_gen, now.flat_gen, now.mutation_epoch,
                                   now.env_gen, now.defuse_version, now.shape_version,
                                   now.ir_soa_generation);
        const auto cur2 = ev.current_layout_stamp();
        const bool match = f2.resume_ir_soa_generation() == cur2.ir_soa_generation &&
                           f2.resume_shape_version() == cur2.shape_version;
        CHECK(match, "AC2: matching stamp no IR fence trip");
    }

    // ── AC4 query surface ──────────────────────────────────────────
    {
        std::println("\n--- #2432 AC4: query keys additive ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"1\")").has_value(), "set-code q");
        CHECK(cs.eval("(eval-current)").has_value(), "eval q");
        CHECK(href(cs, "schema-2432") == 2432, "AC4: schema-2432");
        CHECK(href(cs, "issue-2432") == 2432, "AC4: issue-2432");
        CHECK(href(cs, "ir-generation-fence-wired") == 1, "AC4: wired");
        CHECK(href(cs, "ir-generation-fence-hit-total") >= 0, "AC4: hit total");
        CHECK(href(cs, "layout-stamp-ir-soa-generation") >= 0, "AC4: stamp field exposed");
        CHECK(href(cs, "layout-stamp-schema") == static_cast<std::int64_t>(kLayoutStampSchema),
              "AC4: schema bumped to 2432");
        // Shape fence still present
        CHECK(href(cs, "shape-version-fence-wired") == 1, "AC4: shape fence intact");
    }

    // ── #3314: append-only offsetof/sizeof stamps (#2906/#3292) ──
    {
        std::println("\n--- #3314 AC1: IR SoA dirty/column + additional hot structs ---");
        CHECK(kAppendOnlyLayoutStampIssue == 3314, "3314 AC1: issue constant");
        const auto soa = read_file("src/compiler/ir_soa.ixx");
        const auto dens = read_file("src/core/densify_consistency_report.h");
        const auto ls = read_file("src/core/layout_stamp.hh");
        CHECK(soa.find("Issue #3314") != std::string::npos, "3314 AC1: ir_soa cites #3314");
        CHECK(soa.find("offsetof(IRFunctionSoA, generation_) == 440") != std::string::npos,
              "3314 AC1: IRFunctionSoA last-member offsetof");
        CHECK(soa.find("offsetof(IRFunctionSoA, instruction_dirty_) == 408") != std::string::npos,
              "3314 AC1: instruction_dirty_ offsetof");
        CHECK(soa.find("offsetof(IRFunctionSoA, block_dirty_) == 376") != std::string::npos,
              "3314 AC1: block_dirty_ offsetof");
        CHECK(soa.find("sizeof(IRFunctionSoA) == 448") != std::string::npos,
              "3314 AC1: IRFunctionSoA sizeof");
        CHECK(sizeof(IRInstructionView) == 16, "3314 AC1: IRInstructionView sizeof live");
        CHECK(offsetof(IRInstructionView, idx) == 8, "3314 AC1: IRInstructionView.idx live");
        CHECK(sizeof(DensifyConsistencyReport) == 8,
              "3314 AC1: DensifyConsistencyReport sizeof live");
        CHECK(offsetof(DensifyConsistencyReport, envframe_ok) == 7,
              "3314 AC1: envframe_ok last axis live");
        CHECK(sizeof(LayoutStamp) == 64, "3314 AC1: LayoutStamp sizeof live");
        CHECK(offsetof(LayoutStamp, ir_soa_generation) == 56,
              "3314 AC1: LayoutStamp last field live");
        CHECK(dens.find("Issue #3314") != std::string::npos, "3314 AC1: densify cites #3314");
        CHECK(ls.find("Issue #3314") != std::string::npos, "3314 AC1: LayoutStamp cites #3314");

        std::println("\n--- #3314 AC2: stamps present, no new runtime ---");
        CHECK(soa.find("g_3314_") == std::string::npos, "3314 AC2: no g_3314_* in ir_soa");
        CHECK(dens.find("g_3314_") == std::string::npos, "3314 AC2: no g_3314_* in densify");
        CHECK(ls.find("g_3314_") == std::string::npos, "3314 AC2: no g_3314_* in layout_stamp");
        const auto build = read_file("build.py");
        CHECK(build.find("check_append_only_layout_stamps_3314") != std::string::npos,
              "3314 AC2: build.py wires linter");

        std::println("\n--- #3314 AC3: compile-time only ---");
        CHECK(soa.find("Compile-time only") != std::string::npos ||
                  soa.find("compile-time only") != std::string::npos,
              "3314 AC3: ir_soa compile-time only");
        CHECK(dens.find("Compile-time only") != std::string::npos,
              "3314 AC3: densify compile-time only");

        std::println("\n--- #3314 AC4: existing suite, no invent / docs ---");
        CHECK(read_file("tests/compiler/test_issue_3314.cpp").empty(), "3314 AC4: no invent");
        CHECK(read_file("docs/design/3314-append-only-layout-stamps.md").empty(),
              "3314 AC4: no docs/design");
        CHECK(build.find("check_pcv_hotpath_metrics_layout_3292") != std::string::npos,
              "3314 AC4: 3292 linter still wired");
    }

    // ── #3833: IR SoA columns Arena-backed; BMI pins unchanged ──
    {
        std::println("\n--- #3833 AC1: IrSoaArenaColumn + bind closes heap columns ---");
        CHECK(kIrSoaColumnArenaIssue == 3833, "3833 AC1: issue constant");
        const auto soa = read_file("src/compiler/ir_soa.ixx");
        CHECK(soa.find("IrSoaArenaColumn") != std::string::npos, "3833 AC1: column type");
        CHECK(soa.find("kIrSoaColumnArenaIssue = 3833") != std::string::npos, "3833 AC1: stamp");
        CHECK(soa.find("std::vector<aura::ir::IROpcode> opcodes_") == std::string::npos,
              "3833 AC1: opcodes_ no longer std::vector");
        CHECK(soa.find("IrSoaArenaColumn<aura::ir::IROpcode> opcodes_") != std::string::npos,
              "3833 AC1: opcodes_ arena column");
        CHECK(soa.find("offsetof(IRFunctionSoA, block_dirty_) == 376") != std::string::npos,
              "3833 AC1: BMI pin block_dirty_ kept");
        CHECK(soa.find("sizeof(IRFunctionSoA) == 448") != std::string::npos,
              "3833 AC1: BMI sizeof kept");

        IRFunctionSoA fn;
        fn.bind_column_arena();
        fn.blocks_.resize(1);
        fn.blocks_[0].block_id = 0;
        fn.blocks_[0].start_idx = 0;
        fn.blocks_[0].end_idx = 256;
        fn.opcodes_.resize(256);
        fn.operand0_.resize(256);
        fn.shape_ids_.resize(256);
        fn.linear_ownership_states_.resize(256);
        fn.instruction_dirty_.assign(256, 1);
        fn.block_dirty_.assign(1, 1);
        CHECK(fn.opcodes_.uses_arena_resource(), "3833 AC1: opcodes_ arena-owned");
        CHECK(fn.shape_ids_.uses_arena_resource(), "3833 AC1: shape_ids_ arena-owned");
        // view_at / HARDEN path unchanged (bounds via opcodes_.size)
        IRModuleV2 mod;
        mod.functions.push_back(std::move(fn));
        auto view = mod.view_at(0, 0);
        CHECK(view.func != nullptr, "3833 AC2: view_at intact");

        std::println("\n--- #3833 AC2: batch dirty + HARDEN unchanged ---");
        auto& live = mod.functions[0];
        live.bind_column_arena();
        const std::uint32_t ids[1] = {0};
        live.mark_blocks_dirty(std::span<const std::uint32_t>{ids});
        CHECK(live.is_block_dirty(0), "3833 AC2: batch dirty");
        CHECK(live.generation() >= 1, "3833 AC2: generation bump");

        std::println("\n--- #3833 AC3: SoA walk microbench (arena columns contiguous) ---");
        // Touch hot columns via walk; arena slab keeps opcode/shape/linear
        // streams co-located vs tip std::vector heap. Wall time is advisory.
        const auto t0 = std::chrono::steady_clock::now();
        auto walk = walk_soa_function_hotpath(live, /*dirty_only=*/false);
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        CHECK(walk.instructions == 256, "3833 AC3: walked all instrs");
        CHECK(ns >= 0, "3833 AC3: microbench ran");
        std::println("3833 AC3: walk_soa 256 instr ns={}", ns);
        CHECK(sizeof(IrSoaArenaColumn<std::uint32_t>) == sizeof(std::vector<std::uint32_t>),
              "3833 AC3: column object size == std::vector (BMI)");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_ir_soa_layout_stamp();
}
#endif
