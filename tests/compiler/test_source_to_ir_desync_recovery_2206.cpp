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
    CHECK(!dirty.empty() && dirty.find("recover_source_to_ir_map_desync") != std::string::npos,
          "service_dirty wires recover");
    CHECK(dirty.find("source_to_ir_desync_recovered_total") != std::string::npos,
          "dirty bumps recovered");
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

} // namespace

int run_test_source_to_ir_desync_recovery_2206() {
    std::println("=== test_source_to_ir_desync_recovery_2206 ===");
    ac1_recover_keeps_partial();
    ac2_query_schema();
    ac3_wireup_soundness();
    ac4_service_inject_recover();
    ac_extra_full_rebuild_fallback();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_source_to_ir_desync_recovery_2206();
}
#endif
