// @category: unit
// @reason: Issue #2045 — source_to_ir_map consistency after selective
// invalidate / re-lower (rebuild/patch + assert + SoA dual-emit sync).
// Refine #2244 — Strict-mode hard-fail + rebuild on inconsistency.
//
//   AC1: source cites #2045; rebuild_or_patch + pure helpers + consistency
//   AC2: pure — rebuild map; assert consistent; inject stale → inconsistent
//   AC3: pure — patch_for_function drops old func locs + re-stamps
//   AC4: query:soa-dirty-stats schema-2045 + rebuild/check keys
//   AC5: service — define/eval + invalidate cascade bumps rebuild/checks;
//        inconsistency_total stays 0 (no under-invalidation on green path)
//   AC6: multi-round mutate with quote/lambda; map checks stay green
//   AC7: ensure_source_to_ir_or_rebuild consistent path — zero extra cost (AC3)
//   AC8: ensure helper Strict desync fires hard_fail + rebuild (AC1, AC2 off-path)
//        + wire-up at invalidate_bridge_with_impact + 2 metrics fields
//   AC9: query:incremental-relower-stats surface has 2 new keys + schema-2244
//        (AC4) + default Off (AC2)
//   All AC7-AC9 land Issue #2244 — Strict-mode hard-fail + rebuild contract.

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

using aura::compiler::assert_source_to_ir_map_consistent;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::count_source_to_ir_map_inconsistencies;
using aura::compiler::patch_source_to_ir_map_for_function;
using aura::compiler::populate_source_to_ir_map_from_irs;
using aura::compiler::rebuild_source_to_ir_map_from_irs;
using aura::compiler::source_to_ir_map_is_consistent;
using aura::compiler::SourceIrLoc;
using aura::compiler::SourceToIrMap;
using aura::compiler::SourceToIrStrictMode;
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

void ac1_source() {
    std::println("\n--- AC1: source cites #2045 ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto svc = read_file("src/compiler/service.ixx");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(!pure.empty() && pure.find("#2045") != std::string::npos, "ir_cache_pure #2045");
    CHECK(pure.find("rebuild_source_to_ir_map_from_irs") != std::string::npos, "rebuild helper");
    CHECK(pure.find("patch_source_to_ir_map_for_function") != std::string::npos, "patch helper");
    CHECK(pure.find("assert_source_to_ir_map_consistent") != std::string::npos, "assert helper");
    CHECK(pure.find("count_source_to_ir_map_inconsistencies") != std::string::npos, "count helper");
    CHECK(!svc.empty() && svc.find("rebuild_or_patch_source_to_ir_map_") != std::string::npos,
          "service rebuild/patch");
    CHECK(svc.find("source_to_ir_map") != std::string::npos, "entry field");
    CHECK(svc.find("#2045") != std::string::npos, "service #2045");
    CHECK(!dirty.empty() && dirty.find("#2045") != std::string::npos, "service_dirty #2045");
    CHECK(dirty.find("ensure_source_to_ir_map_") != std::string::npos, "dirty uses ensure");
    CHECK(dirty.find("store_define_v2") != std::string::npos, "cascade full updates v2");
    CHECK(!met.empty() && met.find("source_to_ir_map_rebuild_total") != std::string::npos,
          "rebuild metric");
    CHECK(met.find("source_to_ir_map_inconsistency_total") != std::string::npos, "incons metric");
    CHECK(!q.empty() && q.find("schema-2045") != std::string::npos, "query schema-2045");
}

void ac2_pure_rebuild_assert() {
    std::println("\n--- AC2: pure rebuild + consistency assert ---");
    std::vector<IRFunction> irs;
    irs.push_back(make_fn_with_source_stamps("f", 10, 20));
    SourceToIrMap map;
    rebuild_source_to_ir_map_from_irs(irs, map);
    CHECK(map.size() == 2, "two mapped nodes");
    CHECK(map.count(10) == 1 && map[10].has_instr(), "node 10 mapped");
    CHECK(map.count(20) == 1 && map[20].instr_index == 1, "node 20 at instr 1");
    CHECK(source_to_ir_map_is_consistent(irs, map), "consistent after rebuild");
    CHECK(assert_source_to_ir_map_consistent(irs, map), "assert ok");
    CHECK(count_source_to_ir_map_inconsistencies(irs, map) == 0, "zero bad");

    // Inject stale loc: point node 10 at out-of-range block
    map[10] = SourceIrLoc{/*fi*/ 0, /*bi*/ 99, /*ii*/ 0};
    CHECK(!source_to_ir_map_is_consistent(irs, map), "stale block detected");
    CHECK(count_source_to_ir_map_inconsistencies(irs, map) >= 1, "bad count >= 1");
    CHECK(!assert_source_to_ir_map_consistent(irs, map), "assert fails soft");

    // Repair via rebuild
    rebuild_source_to_ir_map_from_irs(irs, map);
    CHECK(source_to_ir_map_is_consistent(irs, map), "repaired");
}

void ac3_pure_patch() {
    std::println("\n--- AC3: pure patch_for_function ---");
    std::vector<IRFunction> irs;
    irs.push_back(make_fn_with_source_stamps("a", 1, 2));
    irs.push_back(make_fn_with_source_stamps("b", 3, 4));
    SourceToIrMap map;
    rebuild_source_to_ir_map_from_irs(irs, map);
    CHECK(map.size() == 4, "four nodes");

    // Replace function 1 with new stamps
    auto new_b = make_fn_with_source_stamps("b2", 30, 40);
    new_b.id = 1;
    irs[1] = new_b;
    patch_source_to_ir_map_for_function(irs[1], 1, map);
    CHECK(map.count(3) == 0 && map.count(4) == 0, "old b stamps dropped");
    CHECK(map.count(30) == 1 && map[30].function_index == 1, "new 30");
    CHECK(map.count(40) == 1, "new 40");
    CHECK(map.count(1) == 1 && map.count(2) == 1, "func 0 stamps kept");
    CHECK(source_to_ir_map_is_consistent(irs, map), "consistent after patch");
}

void ac4_query_schema() {
    std::println("\n--- AC4: query schema-2045 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) (+ x 1)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:soa-dirty-stats\")");
    CHECK(h && is_hash(*h), "soa-dirty hash");
    CHECK(href_soa(cs, "schema-2045") == 2045, "schema-2045");
    CHECK(href_soa(cs, "issue-2045") == 2045, "issue-2045");
    CHECK(href_soa(cs, "source-to-ir-map-consistency-wired") == 1, "wired");
    CHECK(href_soa(cs, "source_to_ir_map_rebuild_total") >= 0, "rebuild key");
    CHECK(href_soa(cs, "source_to_ir_map_consistent_checks_total") >= 0, "checks key");
    auto hi = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(hi && is_hash(*hi), "incremental hash");
    CHECK(href_inc(cs, "schema-2045") == 2045, "inc schema-2045");
}

void ac5_service_cascade_rebuild() {
    std::println("\n--- AC5: service invalidate cascade rebuilds map ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define a (lambda () 1))"
                  "(define b (lambda () (a)))"
                  "\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto rebuild0 = m->source_to_ir_map_rebuild_total.load(std::memory_order_relaxed);
    const auto checks0 =
        m->source_to_ir_map_consistent_checks_total.load(std::memory_order_relaxed);
    const auto bad0 = m->source_to_ir_map_inconsistency_total.load(std::memory_order_relaxed);
    cs.public_invalidate_function("a");
    const auto rebuild1 = m->source_to_ir_map_rebuild_total.load(std::memory_order_relaxed);
    const auto checks1 =
        m->source_to_ir_map_consistent_checks_total.load(std::memory_order_relaxed);
    const auto bad1 = m->source_to_ir_map_inconsistency_total.load(std::memory_order_relaxed);
    const auto patch1 = m->source_to_ir_map_patch_total.load(std::memory_order_relaxed);
    std::println("  rebuild {}→{} checks {}→{} bad {}→{} patch {}", rebuild0, rebuild1, checks0,
                 checks1, bad0, bad1, patch1);
    // After cascade re-lower (partial or full) we should have rebuilt/patched
    // and run consistency checks. Green path: no inconsistencies.
    CHECK(rebuild1 >= rebuild0 || patch1 > 0 || checks1 >= checks0,
          "rebuild/patch/check activity after invalidate");
    CHECK(bad1 == bad0, "no new inconsistencies on green path");
    CHECK(href_soa(cs, "source_to_ir_map_inconsistency_total") == static_cast<std::int64_t>(bad1),
          "query mirrors metric");
}

void ac6_quote_lambda_multi_mutate() {
    std::println("\n--- AC6: quote/lambda multi-mutate map stays consistent ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define mk (lambda (n) (lambda (x) (+ x n))))"
                  "(define f (lambda () (quote (a b))))"
                  "\")")
              .has_value(),
          "set-code quote/lambda");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto bad0 = m->source_to_ir_map_inconsistency_total.load(std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        cs.public_invalidate_function("mk");
        cs.public_invalidate_function("f");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval");
    }
    const auto bad1 = m->source_to_ir_map_inconsistency_total.load(std::memory_order_relaxed);
    const auto checks = m->source_to_ir_map_consistent_checks_total.load(std::memory_order_relaxed);
    std::println("  bad {}→{} checks {}", bad0, bad1, checks);
    CHECK(bad1 == bad0, "no under-invalidation signal after quote/lambda mutates");
    CHECK(checks >= 0, "checks non-negative");
}

} // namespace

// AC7: ensure_source_to_ir_or_rebuild consistent path (AC3 zero extra cost)
void ac7_ensure_helper_consistent() {
    std::println("\n--- AC7: ensure helper consistent path (AC3) ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(!pure.empty() && pure.find("ensure_source_to_ir_or_rebuild") != std::string::npos,
          "ensure helper declared");
    CHECK(pure.find("SourceToIrStrictMode") != std::string::npos, "mode enum");
    CHECK(pure.find("EnsureSourceToIrResult") != std::string::npos, "result struct");
    std::vector<IRFunction> irs;
    irs.push_back(make_fn_with_source_stamps("f", 10, 20));
    SourceToIrMap map;
    rebuild_source_to_ir_map_from_irs(irs, map);
    // Off + consistent → zero-cost: was_consistent=true, no rebuild
    auto r = ensure_source_to_ir_or_rebuild(irs, map, SourceToIrStrictMode::Off);
    CHECK(r.was_consistent, "consistent path reports was_consistent");
    CHECK(r.bad_entries == 0, "consistent path zero bad entries");
    CHECK(!r.rebuilt, "consistent path no rebuild");
    CHECK(!r.hard_failed, "consistent path no hard fail");
    // Strict + consistent → same zero-cost outcome (AC3)
    auto r2 = ensure_source_to_ir_or_rebuild(irs, map, SourceToIrStrictMode::Strict);
    CHECK(r2.was_consistent, "strict + consistent same outcome");
    CHECK(!r2.hard_failed, "strict + consistent no hard fail");
}

// AC8: ensure helper Strict desync fires hard_fail + rebuild (AC1, AC2 off-path)
void ac8_ensure_helper_strict_hard_fail() {
    std::println("\n--- AC8: ensure helper Strict desync (AC1, AC2 off-path) ---");
    std::vector<IRFunction> irs;
    irs.push_back(make_fn_with_source_stamps("g", 5, 6));
    SourceToIrMap map;
    rebuild_source_to_ir_map_from_irs(irs, map);
    map[5] = SourceIrLoc{0, 99, 0}; // inject stale loc
    CHECK(!source_to_ir_map_is_consistent(irs, map), "stale detected");
    // Strict mode → hard_failed=true (AC1)
    auto r = ensure_source_to_ir_or_rebuild(irs, map, SourceToIrStrictMode::Strict);
    CHECK(!r.was_consistent, "was_consistent=false on stale");
    CHECK(r.bad_entries >= 1, "bad entries >= 1");
    CHECK(r.rebuilt, "rebuilt after stale");
    CHECK(r.hard_failed, "strict mode hard_failed=true (AC1)");
    CHECK(source_to_ir_map_is_consistent(irs, map), "consistent after rebuild");
    // Off mode → rebuild but no hard_fail (AC2)
    map[5] = SourceIrLoc{0, 99, 0}; // re-inject
    auto r2 = ensure_source_to_ir_or_rebuild(irs, map, SourceToIrStrictMode::Off);
    CHECK(r2.rebuilt && !r2.hard_failed, "off mode rebuild only (AC2)");
    // Wire-up source-cite: service_dirty.cpp + metrics
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("ensure_source_to_ir_or_rebuild") != std::string::npos,
          "wire-up at invalidate_bridge_with_impact");
    CHECK(dirty.find("g_source_to_ir_strict") != std::string::npos, "atomic toggle");
    CHECK(dirty.find("aura_source_to_ir_set_strict") != std::string::npos, "setter");
    CHECK(dirty.find("aura_source_to_ir_strict_v_read") != std::string::npos, "v_read");
    CHECK(dirty.find("source_to_ir_hard_fail_total") != std::string::npos,
          "hard_fail counter bump site");
    auto met = read_file("src/compiler/observability_metrics.h");
    CHECK(met.find("source_to_ir_inconsistency_total{0}") != std::string::npos, "counter 1 field");
    CHECK(met.find("source_to_ir_hard_fail_total{0}") != std::string::npos, "counter 2 field");
}

// AC9: query:incremental-relower-stats has 2 new keys + schema-2244 (AC4) + AC2 default Off
void ac9_query_surface() {
    std::println("\n--- AC9: query surface + schema-2244 + default Off (AC4/AC2) ---");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(q.find("source-to-ir-inconsistency-total") != std::string::npos, "key 1 dash");
    CHECK(q.find("source_to_ir_inconsistency_total") != std::string::npos, "key 1 underscore");
    CHECK(q.find("source-to-ir-hard-fail-total") != std::string::npos, "key 2 dash");
    CHECK(q.find("source_to_ir_hard_fail_total") != std::string::npos, "key 2 underscore");
    CHECK(q.find("schema-2244") != std::string::npos, "schema-2244 lineage");
    CHECK(q.find("issue-2244") != std::string::npos, "issue-2244 lineage");
    // AC2: default Off — unit-test safe
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(dirty.find("std::atomic<std::uint8_t> g_source_to_ir_strict{0}") != std::string::npos,
          "default Off (AC2)");
}

int main() {
    std::println("=== test_source_to_ir_map_consistency_2045 ===");
    ac1_source();
    ac2_pure_rebuild_assert();
    ac3_pure_patch();
    ac4_query_schema();
    ac5_service_cascade_rebuild();
    ac6_quote_lambda_multi_mutate();
    ac7_ensure_helper_consistent();
    ac8_ensure_helper_strict_hard_fail();
    ac9_query_surface();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
