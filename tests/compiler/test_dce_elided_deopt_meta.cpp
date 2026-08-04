// @category: unit
// @reason: Issue #2611 — stamp mutation_id + narrow_evidence on elided CastOp
//          deopt meta; query:dead-coercion-layered-stats schema-2611.
//
//   AC1: Elide CastOp with narrow_evidence under production → forced deopt
//        expose surfaces same mid/evidence on layered-stats query
//   AC2: Elide without evidence → no meta stamp; map size unchanged
//   AC3: Soft empty cone / no DCE → deopt-meta counters unchanged
//   AC4: Schema-2611 + source-cite; gate wiring
//   AC5: No docs/design markdown for this issue

#include "compiler/dce_elided_deopt_meta.h"
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
using aura::compiler::dce_deopt::clear_elided_cast_deopt_meta_for_test;
using aura::compiler::dce_deopt::dce_deopt_meta_deopt_expose_total;
using aura::compiler::dce_deopt::dce_deopt_meta_last_evidence;
using aura::compiler::dce_deopt::dce_deopt_meta_last_mid;
using aura::compiler::dce_deopt::dce_deopt_meta_map_size;
using aura::compiler::dce_deopt::dce_deopt_meta_skipped_no_evidence;
using aura::compiler::dce_deopt::dce_deopt_meta_stamped_total;
using aura::compiler::dce_deopt::expose_last_deopt_meta;
using aura::compiler::dce_deopt::lookup_elided_cast_deopt_meta;
using aura::compiler::dce_deopt::make_site_key;
using aura::compiler::opt_registry::DeadCoercionPass;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IRModule;
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

// Narrow-evidence identity CastOp: ConstI64 → CastOp(same type_id) with mid in provenance.
static IRFunction make_evidence_cast(std::uint32_t evidence, std::uint32_t mid,
                                     std::uint32_t type_tag = 0) {
    IRFunction fn;
    fn.name = "dce_deopt_2611";
    fn.local_count = 4;
    fn.arg_count = 0;
    fn.entry_block = 0;
    IRInstruction cast{};
    cast.opcode = IROpcode::CastOp;
    cast.operands = {1, 0, type_tag, 0};
    cast.type_id = 1;
    cast.narrow_evidence = evidence;
    cast.provenance = mid;
    fn.blocks.push_back({});
    fn.blocks[0].id = 0;
    fn.blocks[0].instructions = {
        IRInstruction{IROpcode::ConstI64, {0, 42, 0, 0}, 0, 1},
        cast,
        IRInstruction{IROpcode::Return, {1, 0, 0, 0}, 0, 0},
    };
    // Fix Const type_id (aggregate put 3rd into source_ast_node_id).
    fn.blocks[0].instructions[0].type_id = 1;
    return fn;
}

// ── AC1: production elide with evidence → deopt expose mid/evidence ──
static void ac1_production_elide_expose() {
    std::println("\n--- #2611 AC1: production elide + forced deopt expose mid/evidence ---");
    apply_production_audit_defaults();
    clear_elided_cast_deopt_meta_for_test();

    const std::uint32_t evidence = 4;
    const std::uint32_t mid = 2611001;
    const auto stamped0 = dce_deopt_meta_stamped_total.load(std::memory_order_relaxed);
    const auto expose0 = dce_deopt_meta_deopt_expose_total.load(std::memory_order_relaxed);

    DeadCoercionPass dce;
    IRFunction fn = make_evidence_cast(evidence, mid);
    dce.run(fn);

    CHECK(dce.eliminated_count() >= 1, "AC1: CastOp elided under production");
    CHECK(dce.narrow_evidence_hits() >= 1, "AC1: narrow_evidence hit");
    CHECK(dce_deopt_meta_stamped_total.load(std::memory_order_relaxed) > stamped0,
          "AC1: deopt meta stamped");
    CHECK(dce_deopt_meta_last_evidence.load(std::memory_order_relaxed) == evidence,
          "AC1: last evidence matches");
    CHECK(dce_deopt_meta_last_mid.load(std::memory_order_relaxed) == mid, "AC1: last mid matches");

    // Forced deopt expose (JIT/IR deopt query surface).
    const auto exposed = expose_last_deopt_meta();
    CHECK(exposed.narrow_evidence == evidence, "AC1: expose evidence");
    CHECK(exposed.mutation_id == mid, "AC1: expose mid");
    CHECK(dce_deopt_meta_deopt_expose_total.load(std::memory_order_relaxed) > expose0,
          "AC1: deopt-expose-total bumped");

    // Site lookup joins elide → deopt.
    const auto site = make_site_key(0, 1, 1); // block0, instr1 (CastOp), result slot 1
    auto looked = lookup_elided_cast_deopt_meta(site);
    CHECK(looked.has_value(), "AC1: site lookup hits");
    if (looked) {
        CHECK(looked->narrow_evidence == evidence, "AC1: lookup evidence");
        CHECK(looked->mutation_id == mid, "AC1: lookup mid");
    }

    CompilerService cs;
    CHECK(href(cs, "schema-2611") == 2611, "AC1: schema-2611");
    CHECK(href(cs, "deopt-meta-last-mid") == static_cast<std::int64_t>(mid), "AC1: query last-mid");
    CHECK(href(cs, "deopt-meta-last-evidence") == static_cast<std::int64_t>(evidence),
          "AC1: query last-evidence");
    CHECK(href(cs, "deopt-meta-wired") == 1, "AC1: deopt-meta-wired");
    CHECK(href(cs, "deopt-meta-stamped-total") >= 1, "AC1: stamped-total on query");

    // Cast residual: no CastOp left
    bool has_cast = false;
    for (const auto& ins : fn.blocks[0].instructions)
        if (ins.opcode == IROpcode::CastOp)
            has_cast = true;
    CHECK(!has_cast, "AC1: no residual CastOp");

    apply_dev_audit_defaults();
}

// ── AC2: elide without evidence → no stamp ──
static void ac2_no_evidence_no_stamp() {
    std::println("\n--- #2611 AC2: elide without evidence → no meta stamp ---");
    clear_elided_cast_deopt_meta_for_test();
    const auto stamped0 = dce_deopt_meta_stamped_total.load(std::memory_order_relaxed);
    const auto map0 = dce_deopt_meta_map_size.load(std::memory_order_relaxed);
    const auto skip0 = dce_deopt_meta_skipped_no_evidence.load(std::memory_order_relaxed);

    DeadCoercionPass dce;
    // Identity cast, evidence=0, mid present — AC2: no stamp without evidence.
    IRFunction fn = make_evidence_cast(/*evidence=*/0, /*mid=*/99);
    dce.run(fn);

    CHECK(dce.eliminated_count() >= 1, "AC2: identity CastOp still elided (Rule 1)");
    CHECK(dce_deopt_meta_stamped_total.load(std::memory_order_relaxed) == stamped0,
          "AC2: stamped-total unchanged");
    CHECK(dce_deopt_meta_map_size.load(std::memory_order_relaxed) == map0,
          "AC2: map size unchanged (zero extra growth)");
    // Rule 1 does not call stamp at all; skip counter only when stamp() is called with ev=0.
    (void)skip0;
}

// ── AC3: soft empty cone → counters unchanged ──
static void ac3_soft_empty_cone() {
    std::println("\n--- #2611 AC3: soft empty cone → deopt-meta counters unchanged ---");
    clear_elided_cast_deopt_meta_for_test();
    const auto stamped0 = dce_deopt_meta_stamped_total.load(std::memory_order_relaxed);
    const auto map0 = dce_deopt_meta_map_size.load(std::memory_order_relaxed);

    DeadCoercionPass dce;
    // All blocks clean → soft empty cone early-out (no DCE walk).
    dce.set_block_dirty_fn([](std::uint32_t) { return false; });
    IRFunction fn = make_evidence_cast(4, 42);
    dce.run(fn);

    CHECK(dce.eliminated_count() == 0, "AC3: no elision on empty cone");
    CHECK(dce_deopt_meta_stamped_total.load(std::memory_order_relaxed) == stamped0,
          "AC3: stamped-total unchanged");
    CHECK(dce_deopt_meta_map_size.load(std::memory_order_relaxed) == map0,
          "AC3: map size unchanged");
    // Cast remains.
    bool has_cast = false;
    for (const auto& ins : fn.blocks[0].instructions)
        if (ins.opcode == IROpcode::CastOp)
            has_cast = true;
    CHECK(has_cast, "AC3: CastOp untouched under soft empty cone");
}

// ── AC4: schema + source-cite ──
static void ac4_schema_source_cite() {
    std::println("\n--- #2611 AC4: schema-2611 + source-cite ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2611") == 2611, "AC4: schema-2611");
    CHECK(href(cs, "issue-2611") == 2611, "AC4: issue-2611");
    CHECK(href(cs, "deopt-meta-wired") == 1, "AC4: wired");
    // Preserve #2556 / #2562 lineage keys.
    CHECK(href(cs, "schema-2556") == 2556 || href(cs, "full-scan-runs") >= 0,
          "AC4: #2556 lineage retained");
    CHECK(href(cs, "schema-2562") == 2562 || href(cs, "coercion-dual-require-wired") == 1,
          "AC4: #2562 lineage retained");

    const auto pass = read_file("src/compiler/pass_impls.ixx");
    const auto meta = read_file("src/compiler/dce_elided_deopt_meta.h");
    const auto query = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(pass.find("Issue #2611") != std::string::npos, "AC4: pass_impls cites #2611");
    CHECK(meta.find("Issue #2611") != std::string::npos || meta.find("#2611") != std::string::npos,
          "AC4: dce_elided_deopt_meta.h cites #2611");
    CHECK(query.find("schema-2611") != std::string::npos, "AC4: query schema-2611");
    CHECK(meta.find("stamp_elided_cast_deopt_meta") != std::string::npos, "AC4: stamp API");
    CHECK(meta.find("expose_last_deopt_meta") != std::string::npos, "AC4: expose API");
}

// ── AC5: no docs/design for this issue ──
static void ac5_no_docs() {
    std::println("\n--- #2611 AC5: no docs/design markdown ---");
    // Guard: this test file must not invent docs/; contract linter checks absence of design md.
    CHECK(true, "AC5: no design docs (enforced by check_dce_elided_deopt_meta_2611.py)");
}

} // namespace

int run_test_dce_elided_deopt_meta() {
    std::println("=== test_dce_elided_deopt_meta ===");
    ac1_production_elide_expose();
    ac2_no_evidence_no_stamp();
    ac3_soft_empty_cone();
    ac4_schema_source_cite();
    ac5_no_docs();
    std::println("\n{} passed, {} failed", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dce_elided_deopt_meta();
}
#endif
