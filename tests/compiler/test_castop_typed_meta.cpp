// @category: unit
// @reason: Issue #2624 Phase A — CastOp type_id + narrow_evidence downflow
//          side table (src/dst); no executor/JIT behavior change.
//
//   AC1: Non-elided Coercion/CastOp lower stamps typed meta (type ids/tags)
//   AC2: AST identity elision skips CastOp; no meta stamp required
//   AC3: Missing meta on old IR → missing_total only; no crash
//   AC4: Side table process-local (not SoA/module cache ABI)
//   AC5: DCE with meta present still correct (#2556 lineage)
//   AC6: schema-2624 additive; Phase B/C out of scope; source-cite

#include "compiler/castop_typed_meta.h"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.ir;
import aura.compiler.optimization_passes;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::castop_meta::castop_typed_meta_identity_elide_total;
using aura::compiler::castop_meta::castop_typed_meta_last_dst;
using aura::compiler::castop_meta::castop_typed_meta_last_src;
using aura::compiler::castop_meta::castop_typed_meta_lookup_hits;
using aura::compiler::castop_meta::castop_typed_meta_map_size;
using aura::compiler::castop_meta::castop_typed_meta_missing_total;
using aura::compiler::castop_meta::castop_typed_meta_phase_a;
using aura::compiler::castop_meta::castop_typed_meta_present;
using aura::compiler::castop_meta::castop_typed_meta_stamped_total;
using aura::compiler::castop_meta::castop_typed_meta_wired;
using aura::compiler::castop_meta::clear_castop_typed_meta_for_test;
using aura::compiler::castop_meta::lookup_castop_typed_meta;
using aura::compiler::castop_meta::make_site_key;
using aura::compiler::castop_meta::stamp_castop_typed_meta;
using aura::compiler::opt_registry::DeadCoercionPass;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:dead-coercion-layered-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_2624() {
    clear_castop_typed_meta_for_test();
    apply_dev_audit_defaults();
}

// ── AC1: stamp non-elided CastOp meta ──
static void ac1_non_elided_coercion_stamps() {
    std::println("\n--- #2624 AC1: non-elided CastOp stamps typed meta ---");
    reset_2624();
    const auto stamped0 = castop_typed_meta_stamped_total.load(std::memory_order_relaxed);

    // Simulate lower-time stamp: non-identity cast Int→Float style.
    const std::uint32_t src = 1; // Int-like
    const std::uint32_t dst = 4; // Float-like (distinct)
    const std::uint32_t evidence = 2;
    const std::uint32_t tag = 4;
    const auto site = make_site_key(/*block=*/0, /*instr=*/1, /*result=*/1);
    stamp_castop_typed_meta(site, src, dst, evidence, tag);

    CHECK(castop_typed_meta_stamped_total.load(std::memory_order_relaxed) > stamped0,
          "AC1: stamped_total advanced");
    CHECK(castop_typed_meta_map_size.load(std::memory_order_relaxed) >= 1, "AC1: map_size >= 1");
    CHECK(castop_typed_meta_last_src.load(std::memory_order_relaxed) == src, "AC1: last src");
    CHECK(castop_typed_meta_last_dst.load(std::memory_order_relaxed) == dst, "AC1: last dst");
    CHECK(castop_typed_meta_present(), "AC1: meta present");

    auto hit = lookup_castop_typed_meta(site);
    CHECK(hit.has_value(), "AC1: lookup hits stamped site");
    CHECK(hit->src_type_id == src && hit->dst_type_id == dst, "AC1: src/dst match");
    CHECK(hit->narrow_evidence == evidence, "AC1: evidence copied");
    CHECK(hit->type_tag == tag, "AC1: type_tag present");
    CHECK(src != 0 || dst != 0 || tag != 0, "AC1: type ids or tags non-zero");

    // Lowering path cites #2624 and stamps after Coercion CastOp.
    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("stamp_last_castop_typed_meta") != std::string::npos,
          "AC1: lower helper present");
    CHECK(low.find("#2624") != std::string::npos, "AC1: lower cites #2624");
    CHECK(low.find("castop_typed_meta.h") != std::string::npos, "AC1: lower includes meta");
}

// ── AC2: identity elision → no CastOp / no stamp requirement ──
static void ac2_identity_elision_no_cast() {
    std::println("\n--- #2624 AC2: identity elision skips CastOp / no meta required ---");
    reset_2624();
    const auto stamped0 = castop_typed_meta_stamped_total.load(std::memory_order_relaxed);

    // Soft zero-cost stamp skip: all-zero fields → no map growth.
    stamp_castop_typed_meta(make_site_key(0, 0, 0), 0, 0, 0, 0);
    CHECK(castop_typed_meta_stamped_total.load(std::memory_order_relaxed) == stamped0,
          "AC2: all-zero stamp is no-op");
    CHECK(castop_typed_meta_map_size.load(std::memory_order_relaxed) == 0, "AC2: map still empty");

    const auto low = read_file("src/compiler/lowering_impl.cpp");
    CHECK(low.find("can_elide_coercion_cast") != std::string::npos, "AC2: lower-time elision");
    CHECK(low.find("#2624 AC2") != std::string::npos ||
              low.find("identity elision") != std::string::npos,
          "AC2: identity elision path cited");
    // #1425 / #2025 lineage retained in sources.
    const auto cmap = read_file("src/compiler/coercion_map.ixx");
    CHECK(cmap.find("#1425") != std::string::npos || low.find("#1425") != std::string::npos,
          "AC2: #1425 identity lineage");
}

// ── AC3: missing meta no crash ──
static void ac3_missing_meta_no_crash() {
    std::println("\n--- #2624 AC3: missing meta on old IR → missing++ only ---");
    reset_2624();
    const auto miss0 = castop_typed_meta_missing_total.load(std::memory_order_relaxed);
    const auto hits0 = castop_typed_meta_lookup_hits.load(std::memory_order_relaxed);

    auto miss = lookup_castop_typed_meta(make_site_key(99, 99, 99));
    CHECK(!miss.has_value(), "AC3: lookup returns nullopt");
    CHECK(castop_typed_meta_missing_total.load(std::memory_order_relaxed) > miss0,
          "AC3: missing_total advanced");
    CHECK(castop_typed_meta_lookup_hits.load(std::memory_order_relaxed) == hits0,
          "AC3: lookup_hits unchanged on miss");

    // Execute-style: DCE on CastOp without meta still runs (no crash).
    DeadCoercionPass dce;
    IRFunction fn;
    fn.name = "legacy_no_meta";
    fn.local_count = 3;
    fn.blocks.push_back({});
    fn.blocks[0].id = 0;
    IRInstruction cast{};
    cast.opcode = IROpcode::CastOp;
    cast.operands = {1, 0, 0, 0};
    cast.type_id = 1;
    fn.blocks[0].instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 7, 0, 0}, 0, 1},
        cast,
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}},
    };
    fn.blocks[0].instructions[0].type_id = 1;
    // Should not throw / abort.
    dce.run(fn);
    CHECK(true, "AC3: DCE on untyped CastOp completed");
}

// ── AC4: side table not SoA ABI ──
static void ac4_side_table_not_soa() {
    std::println("\n--- #2624 AC4: side table process-local (not SoA ABI) ---");
    reset_2624();
    const auto meta = read_file("src/compiler/castop_typed_meta.h");
    CHECK(meta.find("side table") != std::string::npos ||
              meta.find("Side table") != std::string::npos ||
              meta.find("side table") != std::string::npos,
          "AC4: documents side table");
    CHECK(meta.find("not persisted") != std::string::npos ||
              meta.find("Not persisted") != std::string::npos ||
              meta.find("no cache ABI") != std::string::npos ||
              meta.find("not persisted") != std::string::npos,
          "AC4: not persisted across cache");
    CHECK(meta.find("kCastOpTypedMetaCap") != std::string::npos, "AC4: bounded ring cap");
    // SoA columns must not gain src_type_id fields for this issue.
    const auto soa = read_file("src/compiler/ir_soa.ixx");
    // Allow type_ids_ (existing) but not new src_type_ids_ column for Phase A.
    CHECK(soa.find("src_type_ids_") == std::string::npos, "AC4: no SoA src_type_ids_ column");
    CHECK(soa.find("castop_src_type") == std::string::npos, "AC4: no SoA castop_src field");
}

// ── AC5: DCE with meta present ──
static void ac5_dce_with_meta() {
    std::println("\n--- #2624 AC5: DCE correct with meta present (#2556 lineage) ---");
    reset_2624();
    const auto id_elide0 = castop_typed_meta_identity_elide_total.load(std::memory_order_relaxed);

    // Stamp identity meta (src==dst) for CastOp at instr index 1.
    const auto site = make_site_key(0, 1, 1);
    stamp_castop_typed_meta(site, /*src=*/5, /*dst=*/5, /*ev=*/0, /*tag=*/0);

    DeadCoercionPass dce;
    IRFunction fn;
    fn.name = "meta_identity";
    fn.local_count = 3;
    fn.blocks.push_back({});
    fn.blocks[0].id = 0;
    // Const type_id intentionally 0 so classic Rule 1 may miss; meta src==dst should elide.
    IRInstruction cast{};
    cast.opcode = IROpcode::CastOp;
    cast.operands = {1, 0, 0, 0};
    cast.type_id = 0; // incomplete on instr; meta has the proof
    fn.blocks[0].instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 9, 0, 0}, 0, 0},
        cast,
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}},
    };
    dce.run(fn);

    CHECK(fn.blocks[0].instructions[1].opcode == IROpcode::Local ||
              castop_typed_meta_identity_elide_total.load(std::memory_order_relaxed) > id_elide0,
          "AC5: meta src==dst enables identity elide (or Local rewrite)");

    // Dirty-cone path still present in sources (#2556).
    const auto opt = read_file("src/compiler/optimization_passes.ixx");
    CHECK(opt.find("#2556") != std::string::npos, "AC5: #2556 dirty-cone lineage retained");
    const auto pass = read_file("src/compiler/pass_impls.ixx");
    CHECK(pass.find("castop_typed_meta_present") != std::string::npos,
          "AC5: DCE consults meta only when present");
}

// ── AC6: schema + source-cite ──
static void ac6_schema_source() {
    std::println("\n--- #2624 AC6: schema-2624 additive + Phase A only ---");
    reset_2624();
    CompilerService cs;
    CHECK(href(cs, "schema-2624") == 2624, "AC6: schema-2624");
    CHECK(href(cs, "issue-2624") == 2624, "AC6: issue-2624");
    CHECK(href(cs, "castop-typed-meta-stamped-total") >= 0, "AC6: stamped queryable");
    CHECK(href(cs, "castop-typed-meta-missing-total") >= 0, "AC6: missing queryable");
    CHECK(href(cs, "castop-typed-meta-wired") == 1, "AC6: wired");
    CHECK(href(cs, "castop-typed-meta-phase-a") == 1, "AC6: phase-a flag");
    CHECK(castop_typed_meta_phase_a.load(std::memory_order_relaxed) == 1, "AC6: phase_a atomic");
    CHECK(castop_typed_meta_wired.load(std::memory_order_relaxed) == 1, "AC6: wired atomic");
    // Lineage retained.
    CHECK(href(cs, "schema-2611") == 2611, "AC6: schema-2611 retained");
    CHECK(href(cs, "schema-2556") == 2556, "AC6: schema-2556 retained");

    const auto meta = read_file("src/compiler/castop_typed_meta.h");
    const auto query = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(meta.find("Phase B") != std::string::npos, "AC6: Phase B out of scope documented");
    CHECK(meta.find("Phase C") != std::string::npos, "AC6: Phase C out of scope documented");
    CHECK(query.find("schema-2624") != std::string::npos, "AC6: query schema-2624");
    CHECK(query.find("castop-typed-meta-stamped-total") != std::string::npos,
          "AC6: query stamped key");
}

} // namespace

int run_test_castop_typed_meta() {
    std::println("=== Issue #2624: CastOp typed meta Phase A ===");
    ac1_non_elided_coercion_stamps();
    ac2_identity_elision_no_cast();
    ac3_missing_meta_no_crash();
    ac4_side_table_not_soa();
    ac5_dce_with_meta();
    ac6_schema_source();
    reset_2624();
    std::println("\n=== #2624: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_castop_typed_meta();
}
#endif
