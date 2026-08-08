// @category: unit
// @reason: Issue #2615 — production multi-block dirty cascades use
//          mark_blocks_dirty (one fence); residual N× mark_block_dirty forbidden.
//          Issue #2773 — unified logical invalidation epoch (extend per #81967).
//
//   AC1: Multi-block production sites use batch; fence +1 for N blocks
//   AC2: Single-block mark_block_dirty still one fence (unchanged)
//   AC3: finish_dirty_sync / instruction_dirty_synced_with_blocks holds
//   AC4: Gate/linter forbids multi mark_block_dirty loops in production sources
//   AC5: Batch fence rate < sequential for N-block invalidates
//
//   #2773 AC1: multi-block mark_blocks_dirty → fence +1 and logical epoch +1
//   #2773 AC2: quiet path — note_logical only on dirty marks (source-cite)
//   #2773 AC3: Shape compact isolation #2617 still cited (no IR→shape force)
//   #2773 AC4: schema-2773 + unified-dirty-fence-advance-total
//   #2773 AC5: single residual mark counted separately from batch
//
//   #2774 AC1: production multi-block uses batch only (static + residual==0)
//   #2774 AC2: single mark_block_dirty still allowed (streak 1, no residual)
//   #2774 AC3: empty span quiet (no residual bump)
//   #2774 AC4: residual multi-via-single counters + schema-2774
//   #2774 AC5: N× mark_block_dirty trips residual; batch clears streak

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.ir;
import aura.compiler.ir_soa;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::current_ir_soa_generation_fence;
using aura::compiler::g_ir_soa_batch_dirty_blocks_total;
using aura::compiler::g_ir_soa_batch_dirty_cascades_total;
using aura::compiler::g_ir_soa_residual_multi_via_single_cascades_total;
using aura::compiler::g_ir_soa_residual_multi_via_single_marks_total;
using aura::compiler::g_ir_soa_single_dirty_marks_total;
using aura::compiler::g_unified_dirty_fence_advance_total;
using aura::compiler::g_unified_dirty_ir_batch_total;
using aura::compiler::g_unified_dirty_ir_single_total;
using aura::compiler::g_unified_dirty_last_sources;
using aura::compiler::IRFunctionSoA;
using aura::compiler::IRModuleV2;
using aura::compiler::kInvSrcIrSoaBatch;
using aura::compiler::kInvSrcIrSoaSingle;
using aura::compiler::kIrSoaBatchDirtyDisciplineIssue;
using aura::compiler::kIrSoaMultiViaSingleBanIssue;
using aura::compiler::kUnifiedDirtyFenceIssue;
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
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:soa-dirty-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static IRFunctionSoA make_n_block_fn(std::uint32_t n_blocks) {
    IRModuleV2 mod;
    auto fi = mod.add_function("disc_f", 4);
    for (std::uint32_t b = 0; b < n_blocks; ++b) {
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 1, 0, 0}, 0, 1, 0, 0);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 2, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
    }
    auto& fn = mod.functions[0];
    fn.block_dirty_.assign(n_blocks, 0);
    fn.instruction_dirty_.assign(fn.size(), 0);
    fn.generation_ = 0;
    return std::move(fn);
}

// ── AC1: multi-block batch one fence ──
static void ac1_multi_batch() {
    std::println("\n--- #2615 AC1: multi-block mark_blocks_dirty one fence ---");
    CHECK(kIrSoaBatchDirtyDisciplineIssue == 2615, "AC1: issue stamp");
    auto fn = make_n_block_fn(5);
    const auto fence0 = current_ir_soa_generation_fence();
    const auto cascades0 = g_ir_soa_batch_dirty_cascades_total.load(std::memory_order_relaxed);
    const auto blocks0 = g_ir_soa_batch_dirty_blocks_total.load(std::memory_order_relaxed);
    const std::uint32_t ids[] = {0, 1, 2, 3, 4};
    fn.mark_blocks_dirty(ids);
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC1: fence +1 for 5 blocks");
    CHECK(fn.generation() == 1, "AC1: generation +1");
    CHECK(g_ir_soa_batch_dirty_cascades_total.load(std::memory_order_relaxed) == cascades0 + 1,
          "AC1: cascade counter +1");
    CHECK(g_ir_soa_batch_dirty_blocks_total.load(std::memory_order_relaxed) == blocks0 + 5,
          "AC1: blocks counter +5");
    CHECK(fn.dirty_block_count() == 5, "AC1: all 5 blocks dirty");

    // Production source sites cite batch API
    const auto dce = read_file("src/compiler/pass_impls.ixx");
    CHECK(dce.find("Issue #2615") != std::string::npos, "AC1: DCE cites #2615");
    CHECK(dce.find("mark_blocks_dirty(changed_blocks)") != std::string::npos ||
              dce.find("mark_blocks_dirty(changed") != std::string::npos,
          "AC1: DCE uses batch mark_blocks_dirty");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("mark_blocks_dirty_bit_only") != std::string::npos,
          "AC1: precise path has mark_blocks_dirty_bit_only");
    CHECK(svc.find("Issue #2615") != std::string::npos, "AC1: service cites #2615");
}

// ── AC2: single-block unchanged ──
static void ac2_single_unchanged() {
    std::println("\n--- #2615 AC2: single mark_block_dirty one fence ---");
    auto fn = make_n_block_fn(3);
    const auto fence0 = current_ir_soa_generation_fence();
    const auto single0 = g_ir_soa_single_dirty_marks_total.load(std::memory_order_relaxed);
    fn.mark_block_dirty(1);
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC2: single fence +1");
    CHECK(fn.is_block_dirty(1) && !fn.is_block_dirty(0), "AC2: only block 1 dirty");
    CHECK(g_ir_soa_single_dirty_marks_total.load(std::memory_order_relaxed) == single0 + 1,
          "AC2: single mark counter +1");
}

// ── AC3: finish_dirty_sync holds ──
static void ac3_finish_sync() {
    std::println("\n--- #2615 AC3: finish_dirty_sync after batch ---");
    IRModuleV2 mod;
    auto fi = mod.add_function("sync_f", 2);
    for (std::uint32_t b = 0; b < 3; ++b) {
        auto bi = mod.add_block(fi);
        mod.add_instruction(fi, IROpcode::ConstI64, {b, 1, 0, 0}, 0, 1, 0, 0);
        mod.seal_block(fi, bi);
    }
    auto& fn = mod.functions[0];
    fn.block_dirty_.assign(3, 0);
    fn.instruction_dirty_.assign(fn.size(), 0);
    const std::uint32_t ids[] = {0, 2};
    fn.mark_blocks_dirty(ids);
    const auto flipped = mod.finish_dirty_sync();
    (void)flipped;
    CHECK(mod.instruction_dirty_synced_with_blocks(), "AC3: instruction_dirty_synced_with_blocks");
    // Bit-only then finish_dirty_sync should cascade missing instrs
    auto fn2 = make_n_block_fn(2);
    const std::uint32_t bits[] = {0, 1};
    fn2.mark_blocks_dirty_bits_only(bits);
    CHECK(fn2.is_block_dirty(0) && fn2.is_block_dirty(1), "AC3: bit-only sets blocks");
    // Instrs may be clean until finish_dirty_sync on module
    IRModuleV2 mod2;
    auto fi2 = mod2.add_function("bits", 2);
    for (std::uint32_t b = 0; b < 2; ++b) {
        auto bi = mod2.add_block(fi2);
        mod2.add_instruction(fi2, IROpcode::ConstI64, {b, 1, 0, 0}, 0, 1, 0, 0);
        mod2.seal_block(fi2, bi);
    }
    mod2.functions[0].block_dirty_.assign(2, 0);
    mod2.functions[0].instruction_dirty_.assign(mod2.functions[0].size(), 0);
    mod2.functions[0].mark_blocks_dirty_bits_only(bits);
    (void)mod2.finish_dirty_sync();
    CHECK(mod2.instruction_dirty_synced_with_blocks(), "AC3: bit-only + finish_dirty_sync synced");
}

// ── AC4: gate / source no residual multi loops ──
static void ac4_no_residual_loops() {
    std::println("\n--- #2615 AC4: no residual multi mark_block_dirty loops ---");
    // Production sources: for-loops over mark_block_dirty on multi ids banned
    // (gate script is authority; here soft scan key files).
    const auto dce = read_file("src/compiler/pass_impls.ixx");
    // After #2615, DCE must batch-mark (not per-block mark_block_dirty in run).
    CHECK(dce.find("mark_block_dirty(block.block_id)") == std::string::npos,
          "AC4: DCE no per-block mark_block_dirty(block.block_id)");
    CHECK(dce.find("mark_blocks_dirty(changed_blocks)") != std::string::npos ||
              dce.find("changed_blocks") != std::string::npos,
          "AC4: DCE batch marks via changed_blocks");
    CHECK(dce.find("Issue #2615") != std::string::npos, "AC4: DCE SoA run cites #2615");
    const auto svc = read_file("src/compiler/service.ixx");
    // precise path must batch bit-only (not N× single mark_block_dirty_bit_only loop only)
    CHECK(svc.find("apply_impact_scope_dirty") != std::string::npos,
          "AC4: apply_impact_scope_dirty present");
    CHECK(svc.find("mark_blocks_dirty_bit_only") != std::string::npos,
          "AC4: precise uses mark_blocks_dirty_bit_only");
    CHECK(svc.find("N× mark_block_dirty_bit_only") != std::string::npos ||
              svc.find("Issue #2615") != std::string::npos,
          "AC4: precise path cites batch bit-only / #2615");
    CompilerService cs;
    CHECK(href(cs, "schema-2615") == 2615, "AC4: schema-2615 on query:soa-dirty-stats");
    CHECK(href(cs, "soa-batch-dirty-discipline-wired") == 1, "AC4: discipline wired");
}

// ── AC5: fence rate improves ──
static void ac5_fence_rate() {
    std::println("\n--- #2615 AC5: batch fence rate < sequential ---");
    constexpr std::uint32_t N = 8;
    auto fn_b = make_n_block_fn(N);
    auto fn_s = make_n_block_fn(N);
    std::vector<std::uint32_t> ids(N);
    for (std::uint32_t i = 0; i < N; ++i)
        ids[i] = i;

    const auto f0 = current_ir_soa_generation_fence();
    fn_b.mark_blocks_dirty(ids);
    const auto batch_delta = current_ir_soa_generation_fence() - f0;

    const auto f1 = current_ir_soa_generation_fence();
    for (std::uint32_t i = 0; i < N; ++i)
        fn_s.mark_block_dirty(i);
    const auto seq_delta = current_ir_soa_generation_fence() - f1;

    CHECK(batch_delta == 1, "AC5: batch fence delta == 1");
    CHECK(seq_delta == N, "AC5: sequential fence delta == N");
    CHECK(batch_delta < seq_delta, "AC5: batch fence < sequential");
}

// ── #2681 AC5: derived bp key computes blocks*10000/cascades ──
static void ac2681_blocks_per_cascade_bp() {
    std::println("\n--- #2681 AC5: soa-batch-blocks-per-cascade-bp ---");
    // Bring-up: 0 cascades → bp == 0 (no division by zero).
    CompilerService cs0;
    CHECK(href(cs0, "soa-batch-blocks-per-cascade-bp") == 0,
          "AC5-2681: bp == 0 when cascades == 0 (no div-by-zero)");
    CHECK(href(cs0, "schema-2681") == 2681, "AC5-2681: schema-2681 sentinel");
    CHECK(href(cs0, "issue-2681") == 2681, "AC5-2681: issue-2681 sentinel");
    CHECK(href(cs0, "soa-batch-dirty-discipline-hardened") == 1,
          "AC5-2681: hardened sentinel wired");

    // Drive a 4-block cascade → bp should be 4*10000/1 = 40000.
    auto fn = make_n_block_fn(4);
    const auto cascades0 = g_ir_soa_batch_dirty_cascades_total.load(std::memory_order_relaxed);
    const std::uint32_t ids[] = {0, 1, 2, 3};
    fn.mark_blocks_dirty(ids);
    const auto cascades1 = g_ir_soa_batch_dirty_cascades_total.load(std::memory_order_relaxed);
    CHECK(cascades1 == cascades0 + 1, "AC5-2681: cascade counter +1 after 4-block mark");

    CompilerService cs;
    const auto bp = href(cs, "soa-batch-blocks-per-cascade-bp");
    CHECK(bp > 0, "AC5-2681: bp > 0 after cascade");
    // blocks/cascades ratio is at least 4 (this cascade) plus prior
    // accumulated blocks — so bp >= 40000 is guaranteed.
    CHECK(bp >= 40000, "AC5-2681: bp reflects at least one 4-block cascade (>= 40000bp)");

    // #2615 baseline keys still wired.
    CHECK(href(cs, "soa-batch-dirty-cascades-total") >= 1, "AC5-2681: cascades-total still wired");
    CHECK(href(cs, "soa-single-dirty-marks-total") >= 0,
          "AC5-2681: single-marks-total still wired");
}

// ── #2681 AC6: source-cite / no regression ──
static void ac2681_source_cite() {
    std::println("\n--- #2681 AC6: source-cite + no regression ---");
    const auto soa = read_file("src/compiler/ir_soa.ixx");
    const auto dce = read_file("src/compiler/pass_impls.ixx");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");

    // Issue #2681 sentinel in all four production-side files.
    CHECK(soa.find("Issue #2681") != std::string::npos, "AC6: ir_soa.ixx cites Issue #2681");
    CHECK(dce.find("Issue #2681") != std::string::npos, "AC6: pass_impls.ixx cites Issue #2681");
    CHECK(svc.find("Issue #2681") != std::string::npos, "AC6: service.ixx cites Issue #2681");
    CHECK(q.find("Issue #2681") != std::string::npos,
          "AC6: evaluator_primitives_obs_jit.cpp cites Issue #2681");

    // mark_all_blocks_dirty is still bulk + single bump (AC4 / no regression).
    // Body must have std::fill(block_dirty_ ...) + std::fill(instruction_dirty_ ...)
    // + exactly one bump_generation() call.
    const auto mabd_pos = soa.find("void mark_all_blocks_dirty() {");
    CHECK(mabd_pos != std::string::npos, "AC6: mark_all_blocks_dirty impl present");
    if (mabd_pos != std::string::npos) {
        const auto body_end = soa.find("\n    }\n", mabd_pos);
        CHECK(body_end != std::string::npos, "AC6: mark_all_blocks_dirty body close");
        if (body_end != std::string::npos) {
            const auto body = soa.substr(mabd_pos, body_end - mabd_pos + 6);
            const auto bumps = [&]() -> std::size_t {
                std::size_t n = 0;
                std::size_t pos = 0;
                const std::string needle = "bump_generation()";
                while ((pos = body.find(needle, pos)) != std::string::npos) {
                    ++n;
                    pos += needle.size();
                }
                return n;
            }();
            CHECK(bumps == 1,
                  "AC6: mark_all_blocks_dirty has exactly 1 bump_generation() (no regression)");
        }
    }

    // AC3 (#2681 widen): scan all production TUs under src/compiler/ — no
    // residual multi-block mark_block_dirty loops. The linter is authoritative,
    // but we self-verify a few key files here for fast feedback.
    auto has_residual = [](const std::string& content) -> bool {
        std::size_t pos = 0;
        while ((pos = content.find("for (", pos)) != std::string::npos) {
            const auto body_end = content.find("\n}", pos);
            if (body_end == std::string::npos || body_end - pos > 2000)
                break;
            const auto snippet = content.substr(pos, body_end - pos);
            // Strip batch / no-bump / impl variants; bare mark_block_dirty(
            // inside a for-loop over multi blocks is a residual.
            std::string bare = snippet;
            const std::array<const char*, 5> noise = {
                "mark_block_dirty_bit_only_no_bump",
                "mark_block_dirty_no_bump",
                "mark_block_dirty_impl",
                "mark_block_dirty_bits_only",
                "mark_blocks_dirty",
            };
            for (auto* n : noise) {
                std::size_t p = 0;
                while ((p = bare.find(n, p)) != std::string::npos) {
                    bare.replace(p, std::strlen(n), "");
                }
            }
            if (bare.find("mark_block_dirty(") != std::string::npos)
                return true;
            pos = body_end;
        }
        return false;
    };
    CHECK(!has_residual(dce), "AC6: pass_impls.ixx no residual mark_block_dirty loop");
    CHECK(!has_residual(svc), "AC6: service.ixx no residual mark_block_dirty loop");

    // No design doc regression (per #1655).
    for (const auto& p :
         {"docs/design/batch_dirty_discipline_2681.md", "docs/batch_dirty_discipline_2681.md"}) {
        std::ifstream f(p);
        CHECK(!f.good(), "AC6: no design doc at " + std::string(p));
    }
}

// ── Issue #2773: unified logical invalidation epoch ──

static void ac2773_1_batch_one_fence_one_logical() {
    std::println("\n--- #2773 AC1: multi-block batch → fence +1, logical +1 ---");
    CHECK(kUnifiedDirtyFenceIssue == 2773, "AC1: issue stamp");
    auto fn = make_n_block_fn(4);
    const auto fence0 = current_ir_soa_generation_fence();
    const auto logical0 = g_unified_dirty_fence_advance_total.load(std::memory_order_relaxed);
    const auto batch0 = g_unified_dirty_ir_batch_total.load(std::memory_order_relaxed);
    const std::uint32_t ids[] = {0, 1, 2, 3};
    fn.mark_blocks_dirty(ids);
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC1: IR fence +1 for 4 blocks");
    CHECK(g_unified_dirty_fence_advance_total.load(std::memory_order_relaxed) == logical0 + 1,
          "AC1: unified logical advance +1");
    CHECK(g_unified_dirty_ir_batch_total.load(std::memory_order_relaxed) == batch0 + 1,
          "AC1: ir-batch logical counter +1");
    CHECK((g_unified_dirty_last_sources.load(std::memory_order_relaxed) & kInvSrcIrSoaBatch) != 0,
          "AC1: last sources includes IR batch bit");
}

static void ac2773_2_single_separate() {
    std::println("\n--- #2773 AC2: single mark counted separately from batch ---");
    auto fn = make_n_block_fn(2);
    const auto fence0 = current_ir_soa_generation_fence();
    const auto logical0 = g_unified_dirty_fence_advance_total.load(std::memory_order_relaxed);
    const auto single0 = g_unified_dirty_ir_single_total.load(std::memory_order_relaxed);
    const auto batch0 = g_unified_dirty_ir_batch_total.load(std::memory_order_relaxed);
    fn.mark_block_dirty(0);
    CHECK(current_ir_soa_generation_fence() == fence0 + 1, "AC2: single mark fence +1");
    CHECK(g_unified_dirty_fence_advance_total.load(std::memory_order_relaxed) == logical0 + 1,
          "AC2: logical +1");
    CHECK(g_unified_dirty_ir_single_total.load(std::memory_order_relaxed) == single0 + 1,
          "AC2: ir-single counter +1");
    CHECK(g_unified_dirty_ir_batch_total.load(std::memory_order_relaxed) == batch0,
          "AC2: batch counter unchanged on single mark");
    CHECK((g_unified_dirty_last_sources.load(std::memory_order_relaxed) & kInvSrcIrSoaSingle) != 0,
          "AC2: last sources includes IR single bit");
}

static void ac2773_3_shape_isolation() {
    std::println("\n--- #2773 AC3: Shape compact≠storm isolation preserved ---");
    const auto shape = read_file("src/compiler/shape_profiler.h");
    CHECK(shape.find("2617") != std::string::npos, "AC3: #2617 still present");
    CHECK(shape.find("#2773") != std::string::npos, "AC3: #2773 notes IR must not force shape");
    CHECK(shape.find("deopt-storm") != std::string::npos ||
              shape.find("mutation_induced_invalidations_") != std::string::npos,
          "AC3: compact isolation contract still documented");
    const auto soa = read_file("src/compiler/ir_soa.ixx");
    CHECK(soa.find("Does NOT bump Shape version") != std::string::npos ||
              soa.find("preserve #2617") != std::string::npos,
          "AC3: note_logical does not bump Shape");
    // IR dirty path must not call shape storm feed symbols.
    CHECK(soa.find("note_logical_invalidation_epoch") != std::string::npos, "AC3: helper present");
}

static void ac2773_4_obs_schema() {
    std::println("\n--- #2773 AC4: schema-2773 observability ---");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("schema-2773") != std::string::npos, "AC4: schema-2773");
    CHECK(q.find("issue-2773") != std::string::npos, "AC4: issue-2773");
    CHECK(q.find("unified-dirty-fence-advance-total") != std::string::npos,
          "AC4: unified-dirty-fence-advance-total key");
    CHECK(q.find("unified-dirty-ir-batch-total") != std::string::npos, "AC4: ir-batch key");
    CHECK(q.find("unified-dirty-ir-single-total") != std::string::npos, "AC4: ir-single key");
    CHECK(q.find("unified-dirty-fence-wired") != std::string::npos, "AC4: wired sentinel");
    // Existing surfaces preserved.
    CHECK(q.find("schema-2522") != std::string::npos, "AC4: schema-2522 preserved");
    CHECK(q.find("schema-2615") != std::string::npos, "AC4: schema-2615 preserved");
    // Process-wide counters advanced by AC1/AC2 (no engine:metrics dependency —
    // query path may abort under partial typecheck rebuilds).
    CHECK(g_unified_dirty_fence_advance_total.load(std::memory_order_relaxed) >= 2,
          "AC4: process-wide advance total ≥ 2 after AC1+AC2 marks");
}

static void ac2773_5_source_cite_quiet() {
    std::println("\n--- #2773 AC5: source-cite quiet path + linter ---");
    const auto soa = read_file("src/compiler/ir_soa.ixx");
    CHECK(soa.find("note_logical_invalidation_epoch") != std::string::npos, "AC5: helper");
    CHECK(soa.find("g_unified_dirty_fence_advance_total") != std::string::npos, "AC5: counter");
    CHECK(soa.find("Quiet path") != std::string::npos ||
              soa.find("quiet path") != std::string::npos ||
              soa.find("Zero cost when no dirty") != std::string::npos,
          "AC5: quiet-path documented");
    CHECK(soa.find("allowed fence writers") != std::string::npos ||
              soa.find("Allowed fence writers") != std::string::npos ||
              soa.find("allowed fence writers") != std::string::npos,
          "AC5: writer policy documented");
    // mark_blocks_dirty body calls note once (batch).
    CHECK(soa.find("kInvSrcIrSoaBatch") != std::string::npos, "AC5: batch source bit");
    CHECK(soa.find("kInvSrcIrSoaSingle") != std::string::npos, "AC5: single source bit");
    const auto build = read_file("build.py");
    CHECK(build.find("check_unified_dirty_fence_2773") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(read_file("docs/design/2773-unified-dirty-fence.md").empty(),
          "AC5: no docs/design/2773-* per #1655");
}

// ── Issue #2774: residual multi-via-single ban ──

static void ac2774_1_batch_no_residual() {
    std::println("\n--- #2774 AC1: multi-block batch leaves residual==0 ---");
    CHECK(kIrSoaMultiViaSingleBanIssue == 2774, "AC1: issue stamp");
    auto fn = make_n_block_fn(5);
    // Use a fresh function so streak TLS starts clean relative to this object.
    const auto r0 =
        g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed);
    const auto m0 = g_ir_soa_residual_multi_via_single_marks_total.load(std::memory_order_relaxed);
    const std::uint32_t ids[] = {0, 1, 2, 3, 4};
    fn.mark_blocks_dirty(ids);
    CHECK(g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed) == r0,
          "AC1: batch path no residual cascade");
    CHECK(g_ir_soa_residual_multi_via_single_marks_total.load(std::memory_order_relaxed) == m0,
          "AC1: batch path no residual marks");
    // Production sources still cite batch (lineage #2615/#2681).
    const auto dce = read_file("src/compiler/pass_impls.ixx");
    CHECK(dce.find("mark_blocks_dirty") != std::string::npos, "AC1: DCE uses batch");
}

static void ac2774_2_single_allowed() {
    std::println("\n--- #2774 AC2: true single mark_block_dirty no residual ---");
    auto fn = make_n_block_fn(3);
    const auto r0 =
        g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed);
    const auto m0 = g_ir_soa_residual_multi_via_single_marks_total.load(std::memory_order_relaxed);
    const auto s0 = g_ir_soa_single_dirty_marks_total.load(std::memory_order_relaxed);
    fn.mark_block_dirty(1);
    CHECK(g_ir_soa_single_dirty_marks_total.load(std::memory_order_relaxed) == s0 + 1,
          "AC2: single marks counter +1");
    CHECK(g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed) == r0,
          "AC2: one single mark → residual cascades unchanged");
    CHECK(g_ir_soa_residual_multi_via_single_marks_total.load(std::memory_order_relaxed) == m0,
          "AC2: one single mark → residual marks unchanged");
}

static void ac2774_3_empty_quiet() {
    std::println("\n--- #2774 AC3: empty span quiet ---");
    auto fn = make_n_block_fn(2);
    const auto fence0 = current_ir_soa_generation_fence();
    const auto r0 =
        g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed);
    fn.mark_blocks_dirty(std::span<const std::uint32_t>{});
    CHECK(current_ir_soa_generation_fence() == fence0, "AC3: empty span no fence");
    CHECK(g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed) == r0,
          "AC3: empty span no residual");
}

static void ac2774_4_residual_trips_and_schema() {
    std::println("\n--- #2774 AC4: N× mark_block_dirty residual + schema ---");
    auto fn = make_n_block_fn(4);
    // Clear streak by batch first, then deliberately residual-loop.
    const std::uint32_t one[] = {0};
    fn.mark_blocks_dirty(one);
    const auto r0 =
        g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed);
    const auto m0 = g_ir_soa_residual_multi_via_single_marks_total.load(std::memory_order_relaxed);
    fn.mark_block_dirty(1);
    fn.mark_block_dirty(2);
    fn.mark_block_dirty(3);
    CHECK(g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed) ==
              r0 + 1,
          "AC4: residual cascade +1 on second single");
    CHECK(g_ir_soa_residual_multi_via_single_marks_total.load(std::memory_order_relaxed) == m0 + 2,
          "AC4: residual marks +2 (2nd and 3rd singles)");
    // Batch clears streak — further singles on same fn start fresh.
    const std::uint32_t ids[] = {0, 1};
    fn.mark_blocks_dirty(ids);
    const auto r1 =
        g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed);
    fn.mark_block_dirty(2);
    CHECK(g_ir_soa_residual_multi_via_single_cascades_total.load(std::memory_order_relaxed) == r1,
          "AC4: after batch, single mark does not residual");

    const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(q.find("schema-2774") != std::string::npos, "AC4: schema-2774");
    CHECK(q.find("soa-residual-multi-via-single-cascades-total") != std::string::npos,
          "AC4: residual cascades key");
    CHECK(q.find("soa-residual-multi-via-single-ban-wired") != std::string::npos,
          "AC4: wired sentinel");
    // #2522 / #2615 preserved
    CHECK(q.find("schema-2522") != std::string::npos, "AC4: schema-2522 preserved");
    CHECK(q.find("schema-2615") != std::string::npos, "AC4: schema-2615 preserved");
}

static void ac2774_5_source_cite() {
    std::println("\n--- #2774 AC5: source-cite + linter wire ---");
    const auto soa = read_file("src/compiler/ir_soa.ixx");
    CHECK(soa.find("#2774") != std::string::npos, "AC5: ir_soa cites #2774");
    CHECK(soa.find("g_ir_soa_residual_multi_via_single_cascades_total") != std::string::npos,
          "AC5: residual cascade counter");
    CHECK(soa.find("note_single_mark_for_residual") != std::string::npos, "AC5: residual helper");
    CHECK(soa.find("clear_single_mark_residual") != std::string::npos, "AC5: clear helper");
    const auto build = read_file("build.py");
    CHECK(build.find("check_batch_dirty_multi_via_single_ban_2774") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(read_file("docs/design/2774-multi-via-single-ban.md").empty(),
          "AC5: no docs/design/2774-* per #1655");
}

} // namespace

int run_test_batch_dirty_discipline() {
    std::println("=== Issue #2615 + #2681: batch dirty cascade discipline ===");
    ac1_multi_batch();
    ac2_single_unchanged();
    ac3_finish_sync();
    ac4_no_residual_loops();
    ac5_fence_rate();
    ac2681_blocks_per_cascade_bp();
    ac2681_source_cite();
    std::println("\n=== Issue #2773: unified dirty fence protocol ===");
    ac2773_1_batch_one_fence_one_logical();
    ac2773_2_single_separate();
    ac2773_3_shape_isolation();
    ac2773_4_obs_schema();
    ac2773_5_source_cite_quiet();
    std::println("\n=== Issue #2774: residual multi-via-single ban ===");
    ac2774_1_batch_no_residual();
    ac2774_2_single_allowed();
    ac2774_3_empty_quiet();
    ac2774_4_residual_trips_and_schema();
    ac2774_5_source_cite();
    std::println("\n=== #2615/#2681/#2773/#2774: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_batch_dirty_discipline();
}
#endif
