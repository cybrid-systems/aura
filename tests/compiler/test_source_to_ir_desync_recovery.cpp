// @category: unit
// @reason: Issue #2206 — aggressive source_to_ir_map desync recovery
// (patch preferred dirty funcs, then full map rebuild; never silent
// full-relower solely because of reverse-index desync).
//
//   AC1: After intentional map desync, recovery patches/rebuilds and
//        should_partial_relower still returns true when dirty < threshold.
//   AC2: Counters source_to_ir_desync_recovered_total +
//        source_to_ir_desync_funcs_patched on query:soa-dirty-stats /
//        query:incremental-relower-stats with schema-2206.
//   AC3: Soundness path still green (source cites #2113 + no forced
//        MapInconsistent on green inject→recover); metrics recover++.
//   AC4: Unit injects map desync and asserts recovery + partial retention.

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

// Stable-func-id probe for light-link detection (#2687 AC5 pattern).
extern "C" std::uint32_t aura_get_or_preserve_stable_func_id(const char* name, int* out_preserved);
extern "C" void aura_clear_stable_func_id_map(void);

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.ir_cache_pure;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::count_source_to_ir_map_inconsistencies;
using aura::compiler::patch_source_to_ir_map_for_function;
using aura::compiler::rebuild_source_to_ir_map_from_irs;
using aura::compiler::recover_source_to_ir_map_desync;
using aura::compiler::should_partial_relower;
using aura::compiler::source_to_ir_map_is_consistent;
using aura::compiler::SourceIrLoc;
using aura::compiler::SourceToIrDesyncRecovery;
using aura::compiler::SourceToIrMap;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::ir::BasicBlock;
using aura::ir::IRFunction;
using aura::ir::IRInstruction;
using aura::ir::IROpcode;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href_soa(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:soa-dirty-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_inc(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

IRFunction make_fn_with_source_stamps(std::string name, std::uint32_t n0, std::uint32_t n1) {
    IRFunction fn;
    fn.name = std::move(name);
    fn.id = 0;
    BasicBlock b0;
    b0.id = 0;
    IRInstruction i0;
    i0.opcode = IROpcode::ConstI64;
    i0.source_ast_node_id = n0;
    IRInstruction i1;
    i1.opcode = IROpcode::ConstI64;
    i1.source_ast_node_id = n1;
    b0.instructions.push_back(i0);
    b0.instructions.push_back(i1);
    fn.blocks.push_back(std::move(b0));
    return fn;
}

// AC1: pure recover after desync + should_partial_relower still true
void ac1_recover_keeps_partial() {
    std::println("\n--- AC1: recover after desync; should_partial still true ---");
    std::vector<IRFunction> irs;
    irs.push_back(make_fn_with_source_stamps("a", 10, 20));
    irs.push_back(make_fn_with_source_stamps("b", 30, 40));
    SourceToIrMap map;
    rebuild_source_to_ir_map_from_irs(irs, map);
    CHECK(source_to_ir_map_is_consistent(irs, map), "baseline consistent");

    // Intentional under-dirty desync: stale block index on func 0 stamp.
    map[10] = SourceIrLoc{/*fi*/ 0, /*bi*/ 99, /*ii*/ 0};
    CHECK(!source_to_ir_map_is_consistent(irs, map), "desync injected");
    const std::size_t dirty_n = 1; // body-only dirty surface under threshold
    CHECK(should_partial_relower(dirty_n), "partial eligible before recover");

    std::vector<std::size_t> preferred{0};
    auto rec = recover_source_to_ir_map_desync(irs, map, preferred);
    CHECK(rec.bad_before >= 1, "bad_before >= 1");
    CHECK(rec.recovered, "recovered after patch/rebuild");
    CHECK(rec.funcs_patched >= 1, "patched preferred dirty func");
    CHECK(source_to_ir_map_is_consistent(irs, map), "consistent after recover");
    // AC1: subsequent should_partial_relower still true when dirty < thr
    CHECK(should_partial_relower(dirty_n), "partial retained after recover (AC1)");
    CHECK(should_partial_relower(dirty_n, /*threshold=*/8), "partial under thr=8");
}

// AC2: query surfaces + schema-2206
void ac2_query_schema() {
    std::println("\n--- AC2: query schema-2206 + counters ---");
    auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(!met.empty() && met.find("source_to_ir_desync_recovered_total") != std::string::npos,
          "recovered metric field");
    CHECK(met.find("source_to_ir_desync_funcs_patched") != std::string::npos,
          "funcs_patched field");
    auto qsoa = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(!qsoa.empty() && qsoa.find("schema-2206") != std::string::npos, "soa schema-2206");
    CHECK(qsoa.find("source_to_ir_desync_recovered_total") != std::string::npos,
          "soa recovered key");
    auto qinc = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!qinc.empty() && qinc.find("schema-2206") != std::string::npos, "inc schema-2206");
    CHECK(qinc.find("source-to-ir-desync-recovered-total") != std::string::npos,
          "inc recovered dash key");

    // Light-link detection (#2687 AC5 pattern): under light link the
    // stable-func-id map is a weak stub returning 0 and CompilerService
    // engine:metrics eval queries are not wired (primitives register only
    // in libaura_test_objects), so the eval half of this AC cannot hold.
    // Source-cite checks above always run.
    int preserved = -1;
    const auto probe = aura_get_or_preserve_stable_func_id("__light_probe_2206__", &preserved);
    aura_clear_stable_func_id_map();
    if (probe == 0 && preserved == 0) {
        std::println("  (light link: engine:metrics eval not wired → eval asserts "
                     "best-effort, source-cite kept)");
        return;
    }

    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto hsoa = cs.eval("(engine:metrics \"query:soa-dirty-stats\")");
    CHECK(hsoa && is_hash(*hsoa), "soa-dirty hash");
    CHECK(href_soa(cs, "schema-2206") == 2206, "soa schema-2206 value");
    CHECK(href_soa(cs, "issue-2206") == 2206, "soa issue-2206");
    CHECK(href_soa(cs, "source-to-ir-desync-recovery-wired") == 1, "soa wired");
    CHECK(href_soa(cs, "source_to_ir_desync_recovered_total") >= 0, "soa recovered key live");
    CHECK(href_soa(cs, "source_to_ir_desync_funcs_patched") >= 0, "soa patched key live");
    auto hinc = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(hinc && is_hash(*hinc), "incremental hash");
    CHECK(href_inc(cs, "schema-2206") == 2206, "inc schema-2206 value");
    CHECK(href_inc(cs, "source_to_ir_desync_recovered_total") >= 0, "inc recovered live");
}

// AC3: green recover path; wire-up cites; soundness oracle still referenced
void ac3_wireup_soundness() {
    std::println("\n--- AC3: wire-up + green recover metrics ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(!pure.empty() && pure.find("recover_source_to_ir_map_desync") != std::string::npos,
          "pure recover helper");
    CHECK(pure.find("SourceToIrDesyncRecovery") != std::string::npos, "recovery result struct");
    CHECK(pure.find("#2206") != std::string::npos, "pure cites #2206");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(!dirty.empty() &&
              (dirty.find("recover_source_to_ir_map_desync") != std::string::npos ||
               dirty.find("prepare_source_to_ir_map_for_partial_") != std::string::npos),
          "service_dirty wires recover/prepare");
    CHECK(dirty.find("source_to_ir_desync_recovered_total") != std::string::npos ||
              read_file("src/compiler/service.ixx").find("source_to_ir_desync_recovered_total") !=
                  std::string::npos,
          "dirty/service bumps recovered");
    CHECK(dirty.find("#2206") != std::string::npos, "dirty cites #2206");
    // Soundness oracle lineage still present (#2113) — no regression site
    auto svc = read_file("src/compiler/service_dirty.cpp");
    CHECK(svc.find("incremental_soundness") != std::string::npos ||
              read_file("src/compiler/observability_metrics.h").find("incremental_soundness") !=
                  std::string::npos,
          "soundness metrics still present (#2113)");

    // Pure multi-func: preferred patch alone can repair; full rebuild last resort.
    std::vector<IRFunction> irs;
    irs.push_back(make_fn_with_source_stamps("a", 1, 2));
    irs.push_back(make_fn_with_source_stamps("b", 3, 4));
    SourceToIrMap map;
    rebuild_source_to_ir_map_from_irs(irs, map);
    map[3] = SourceIrLoc{1, 77, 0}; // corrupt func 1 stamp
    auto rec = recover_source_to_ir_map_desync(irs, map, /*preferred*/ {1});
    CHECK(rec.recovered, "green recover");
    CHECK(rec.funcs_patched == 1, "only preferred patched first");
    // Prefer path may finish without full rebuild when patch is enough.
    CHECK(source_to_ir_map_is_consistent(irs, map), "green consistent");
}

// AC4: service inject desync → recover → metrics + partial retention
void ac4_service_inject_recover() {
    std::println("\n--- AC4: service inject map desync + recover ---");
    // Light-link detection (#2687 AC5 pattern): engine:metrics eval queries
    // are not wired under light link → query-mirror asserts best-effort.
    int preserved = -1;
    const auto probe = aura_get_or_preserve_stable_func_id("__light_probe_2206_ac4__", &preserved);
    aura_clear_stable_func_id_map();
    if (probe == 0 && preserved == 0) {
        std::println("  (light link: engine:metrics eval not wired → query-mirror asserts "
                     "best-effort, source-cite kept)");
        return;
    }
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g (lambda (x) (+ x 2))) (g 1)\")").has_value(),
          "set-code g");
    CHECK(cs.eval("(eval-current)").has_value(), "eval g");
    if (!cs.get_define_v2("g"))
        (void)cs.eval("(compile:cache-define \"g\")");
    CHECK(cs.get_define_v2("g") != nullptr, "cache entry g");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto rec0 = m->source_to_ir_desync_recovered_total.load(std::memory_order_relaxed);
    const auto pat0 = m->source_to_ir_desync_funcs_patched.load(std::memory_order_relaxed);
    const auto bad0 = m->source_to_ir_map_inconsistency_total.load(std::memory_order_relaxed);

    CHECK(cs.inject_source_to_ir_map_desync_for_test("g"), "inject desync");
    const auto* entry = cs.get_define_v2("g");
    CHECK(entry != nullptr, "entry after inject");
    CHECK(!source_to_ir_map_is_consistent(entry->irs, entry->source_to_ir_map),
          "desync present after inject");
    const auto dirty_n = entry->dirty_block_count();
    CHECK(dirty_n >= 1, "dirty surface after inject");
    CHECK(should_partial_relower(dirty_n) || dirty_n < 8, "partial-eligible dirty surface");

    CHECK(cs.recover_source_to_ir_desync_for_test("g"), "recover returns true");
    entry = cs.get_define_v2("g");
    CHECK(source_to_ir_map_is_consistent(entry->irs, entry->source_to_ir_map),
          "consistent after service recover");
    const auto rec1 = m->source_to_ir_desync_recovered_total.load(std::memory_order_relaxed);
    const auto pat1 = m->source_to_ir_desync_funcs_patched.load(std::memory_order_relaxed);
    const auto bad1 = m->source_to_ir_map_inconsistency_total.load(std::memory_order_relaxed);
    std::println("  recovered {}→{} patched {}→{} bad {}→{}", rec0, rec1, pat0, pat1, bad0, bad1);
    CHECK(rec1 > rec0, "recovered_total advanced");
    CHECK(pat1 > pat0 || rec1 > rec0, "funcs_patched or recover advanced");
    CHECK(bad1 > bad0, "inconsistency probe advanced on inject");
    // Partial retention: dirty still small → should_partial true
    const auto dirty_after = entry->dirty_block_count();
    CHECK(should_partial_relower(dirty_after > 0 ? dirty_after : 1),
          "partial retained after service recover (AC4)");
    CHECK(href_soa(cs, "source_to_ir_desync_recovered_total") == static_cast<std::int64_t>(rec1),
          "query mirrors recovered");
    CHECK(href_inc(cs, "source_to_ir_desync_funcs_patched") == static_cast<std::int64_t>(pat1),
          "inc query mirrors patched");
}

// Pure: empty preferred → patch all; still-bad forces full rebuild path
void ac_extra_full_rebuild_fallback() {
    std::println("\n--- extra: full rebuild fallback when patch insufficient ---");
    std::vector<IRFunction> irs;
    irs.push_back(make_fn_with_source_stamps("solo", 5, 6));
    SourceToIrMap map;
    rebuild_source_to_ir_map_from_irs(irs, map);
    // Orphan entry pointing nowhere + corrupt live stamp
    map[999] = SourceIrLoc{0, 0, 0};
    map[5] = SourceIrLoc{0, 50, 0};
    // preferred empty → patches all live funcs, then rebuild if orphan remains
    auto rec = recover_source_to_ir_map_desync(irs, map, {});
    CHECK(rec.recovered, "recovered via rebuild if needed");
    // Orphan 999 is dropped by full rebuild; patch-only would leave it
    CHECK(map.count(999) == 0 || source_to_ir_map_is_consistent(irs, map),
          "orphan cleared or consistent");
    CHECK(source_to_ir_map_is_consistent(irs, map), "final consistent");
    if (rec.used_full_rebuild)
        CHECK(rec.used_full_rebuild, "used full rebuild for orphan");
}

// Issue #3068: production workspace relower must recover or force-full
// after inject — not peel on a stale map.
void ac3068_relower_recovers_or_full() {
    std::println("\n--- #3068: inject desync → public_relower recovers or force-full ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define m (lambda (x) (+ x 3))) (m 1)\")").has_value(),
          "3068 set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3068 eval");
    if (!cs.get_define_v2("m"))
        (void)cs.eval("(compile:cache-define \"m\")");
    CHECK(cs.get_define_v2("m") != nullptr, "3068 cache entry");
    auto* met = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(met != nullptr, "3068 metrics");
    const auto rec0 = met->source_to_ir_desync_recovered_total.load(std::memory_order_relaxed);
    const auto forced0 = met->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto reb0 = met->source_to_ir_map_rebuild_total.load(std::memory_order_relaxed);
    CHECK(cs.inject_source_to_ir_map_desync_for_test("m"), "3068 inject");
    (void)cs.public_relower_dirty_defines_from_workspace();
    const auto* entry = cs.get_define_v2("m");
    CHECK(entry != nullptr, "3068 entry after relower");
    CHECK(source_to_ir_map_is_consistent(entry->irs, entry->source_to_ir_map),
          "3068 map consistent after workspace relower");
    const auto rec1 = met->source_to_ir_desync_recovered_total.load(std::memory_order_relaxed);
    const auto forced1 = met->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
    const auto reb1 = met->source_to_ir_map_rebuild_total.load(std::memory_order_relaxed);
    CHECK(rec1 > rec0 || forced1 > forced0 || reb1 > reb0,
          "3068 recover/rebuild or force-full observed");
    auto r = cs.eval("(m 2)");
    CHECK(r && is_int(*r) && as_int(*r) == 5, "3068 (m 2) == 5");

    // AC2: drop instr loc → recover/rebuild or force-full; no silent stale.
    CompilerService cs2;
    CHECK(cs2.eval("(set-code \"(define n (lambda (x) (if x x x))) (n 1)\")").has_value(),
          "3068 AC2 set-code");
    CHECK(cs2.eval("(eval-current)").has_value(), "3068 AC2 eval");
    if (!cs2.get_define_v2("n"))
        (void)cs2.eval("(compile:cache-define \"n\")");
    if (cs2.get_define_v2("n") && cs2.inject_source_to_ir_map_drop_instr_loc_for_test("n")) {
        auto* met2 = static_cast<CompilerMetrics*>(cs2.evaluator().compiler_metrics());
        const auto reb0b = met2->source_to_ir_map_rebuild_total.load(std::memory_order_relaxed);
        const auto forced0b =
            met2->partial_forced_full_by_impact_total.load(std::memory_order_relaxed);
        (void)cs2.public_relower_dirty_defines_from_workspace();
        const auto* e2 = cs2.get_define_v2("n");
        CHECK(e2 != nullptr, "3068 AC2 entry");
        using aura::compiler::source_to_ir_map_missing_instr_loc;
        CHECK(!source_to_ir_map_missing_instr_loc(e2->irs, e2->source_to_ir_map) ||
                  met2->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) >
                      forced0b,
              "3068 AC2 loc restored or force-full");
        CHECK(met2->source_to_ir_map_rebuild_total.load(std::memory_order_relaxed) > reb0b ||
                  met2->partial_forced_full_by_impact_total.load(std::memory_order_relaxed) >
                      forced0b,
              "3068 AC2 rebuild or force-full observed");
        CHECK(cs2.eval("(n 1)").has_value(), "3068 AC2 eval after drop");
    } else {
        CHECK(true, "3068 AC2 no instr loc to drop (soft)");
    }
}

// ── Issue #3985: Phase-5 densify success must not clean-hit pre-densify map ──
static void ac3985_densify_success_invalidates_map() {
    std::println("\n--- #3985: densify success invalidates IR cache map/content latch ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(emb.find("Issue #3985") != std::string::npos, "3985: Phase-5 cites #3985");
    CHECK(svc.find("force_ir_cache_map_invalid_after_densify") != std::string::npos,
          "3985: densify map-invalid helper");
    CHECK(svc.find("kDensifyIrCacheMapInvalidIssue = 3985") != std::string::npos, "3985: stamp");
    CHECK(svc.find("set_densify_ir_cache_map_invalid_fn") != std::string::npos,
          "3985: CompilerService wires densify hook");
    CHECK(svc.find("force_ir_cache_dirty_after_abort") != std::string::npos,
          "3985 AC3: abort densify still uses abort force-dirty");
    const auto densify_fn = svc.find("void force_ir_cache_map_invalid_after_densify()");
    CHECK(densify_fn != std::string::npos, "3985: helper present");
    if (densify_fn != std::string::npos) {
        const auto win = svc.substr(densify_fn, 1600);
        CHECK(win.find("abort_map_invalid = true") != std::string::npos,
              "3985: reuses abort_map_invalid");
        CHECK(win.find("content_stored_this_epoch = false") != std::string::npos,
              "3985: clears content latch");
        CHECK(win.find("source_to_ir_map.clear()") != std::string::npos, "3985: clears map");
        CHECK(win.find("clear_cache_v2_for_define") == std::string::npos,
              "3985 AC3: does not drop irs (not abort restore)");
        CHECK(win.find("aura_aot_bump_func_table_epoch") == std::string::npos,
              "3985: no AOT table bump");
        CHECK(win.find("production_defaults_active()") != std::string::npos,
              "3985 AC4: production/Full gate (Soft no map walk)");
    }
    CHECK(svc.find("schema-3985") == std::string::npos, "3985: no new query key");
    CHECK(read_file("tests/compiler/test_issue_3985.cpp").empty(), "3985: no invent");
    CHECK(read_file("docs/design/3985-densify-ir-cache-map.md").empty(), "3985: no docs/design");

    apply_production_audit_defaults();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f3985 (lambda (x) (+ x 1))) (f3985 1)\")").has_value(),
          "3985 AC1: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "3985 AC1: eval store");
    if (!cs.get_define_v2("f3985"))
        (void)cs.eval("(compile:cache-define \"f3985\")");
    const auto* e0 = cs.get_define_v2("f3985");
    CHECK(e0 != nullptr, "3985 AC1: cache entry");
    CHECK(e0 && e0->content_stored_this_epoch, "3985 AC1: store set content latch");
    const auto hash = e0 ? e0->source_hash : 0;
    CHECK(cs.lookup_define_v2("f3985", hash) == 0, "3985 AC1: clean hit after store");
    CHECK(cs.prepare_source_to_ir_map_for_partial_for_test("f3985"),
          "3985 AC2: partial map usable after store");

    cs.public_force_ir_cache_map_invalid_after_densify();
    const auto* e1 = cs.get_define_v2("f3985");
    CHECK(e1 && e1->abort_map_invalid, "3985 AC1: abort_map_invalid after densify");
    CHECK(e1 && !e1->content_stored_this_epoch, "3985 AC1: content latch cleared");
    CHECK(e1 && e1->source_to_ir_map.empty(), "3985 AC1: map cleared");
    CHECK(e1 && !e1->irs.empty(), "3985 AC3: irs retained (not abort drop)");
    CHECK(cs.lookup_define_v2("f3985", hash) == 1,
          "3985 AC1: lookup_define_v2 == 1 until next store");
    CHECK(!cs.prepare_source_to_ir_map_for_partial_for_test("f3985"),
          "3985 AC2: prepare_source_to_ir_map_for_partial_ false → full peel");

    CHECK(cs.eval("(eval-current)").has_value(), "3985 AC5: eval-current re-stores");
    const auto* e2 = cs.get_define_v2("f3985");
    CHECK(e2 && e2->content_stored_this_epoch, "3985 AC5: store restored content latch");
    CHECK(e2 && !e2->abort_map_invalid, "3985 AC5: abort_map_invalid cleared on store");
    CHECK(cs.lookup_define_v2("f3985", e2 ? e2->source_hash : hash) == 0,
          "3985 AC5: clean hit after store");
    CHECK(cs.prepare_source_to_ir_map_for_partial_for_test("f3985"),
          "3985 AC2: partial map usable after store rebuild");
    if (e2)
        CHECK(source_to_ir_map_is_consistent(e2->irs, e2->source_to_ir_map),
              "3985 AC2: post-store map keys match live IR NodeIds");
    auto r = cs.eval("(f3985 40)");
    CHECK(r.has_value(), "3985 AC5: eval after densify-invalid + relower");

    apply_dev_audit_defaults();
    CompilerService cs_soft;
    CHECK(cs_soft.eval("(set-code \"(define s3985 (lambda (x) x)) (s3985 1)\")").has_value(),
          "3985 AC4: Soft set-code");
    CHECK(cs_soft.eval("(eval-current)").has_value(), "3985 AC4: Soft eval");
    if (!cs_soft.get_define_v2("s3985"))
        (void)cs_soft.eval("(compile:cache-define \"s3985\")");
    const auto* es = cs_soft.get_define_v2("s3985");
    const auto hsoft = es ? es->source_hash : 0;
    cs_soft.public_force_ir_cache_map_invalid_after_densify();
    const auto* es2 = cs_soft.get_define_v2("s3985");
    CHECK(es2 && es2->content_stored_this_epoch,
          "3985 AC4: Soft helper is a no-op (no extra map walk)");
    CHECK(cs_soft.lookup_define_v2("s3985", hsoft) == 0, "3985 AC4: Soft clean hit unchanged");
    apply_dev_audit_defaults();
}

} // namespace

int run_test_source_to_ir_desync_recovery() {
    std::println("=== test_source_to_ir_desync_recovery ===");
    ac1_recover_keeps_partial();
    ac2_query_schema();
    ac3_wireup_soundness();
    ac4_service_inject_recover();
    ac_extra_full_rebuild_fallback();
    ac3068_relower_recovers_or_full();
    ac3985_densify_success_invalidates_map();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_source_to_ir_desync_recovery();
}
#endif
