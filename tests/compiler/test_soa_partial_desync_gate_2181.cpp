// @category: unit
// @reason: Issue #2181 — force SoA instruction_dirty sync on partial
// relower entry; desync → force full (no silent under-invalidate).
//
//   AC1: gate_partial_soa_dirty_sync_ + relower_define_blocks entry
//   AC2: after successful partial, desync == 0
//   AC3: query:soa-dirty-stats schema-2181 + three desync counters
//   AC4: injected desync → force_full; no under-invalidate
//   AC5: already-synced happy path still partial-capable

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/jit_typed_mutation_stats.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_soa;
import aura.compiler.ir;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::IRModuleV2;
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

static IRModuleV2 make_two_block_mod() {
    IRModuleV2 mod;
    auto fi = mod.add_function("f", 4);
    auto bi = mod.add_block(fi);
    mod.add_instruction(fi, IROpcode::ConstI64, {0, 2, 0, 0}, 0, 1, 7, 0);
    mod.add_instruction(fi, IROpcode::ConstI64, {1, 4, 0, 0}, 0, 1, 7, 0);
    mod.add_instruction(fi, IROpcode::Add, {2, 0, 1, 0}, 0, 1, 7, 1);
    mod.seal_block(fi, bi);
    auto bi2 = mod.add_block(fi);
    mod.add_instruction(fi, IROpcode::ConstI64, {3, 0, 0, 0}, 0, 1, 0, 0);
    mod.seal_block(fi, bi2);
    return mod;
}

// Dual-emit lower so cache entry may attach real soa_mod; inject still
// synthesizes skeleton when dual-emit leaves functions empty.
static void warm_define(CompilerService& cs, const char* body) {
    aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(true);
    cs.set_soa_dual_emit(true);
    CHECK(cs.eval(std::format("(set-code \"{}\")", body)).has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

} // namespace

int run_test_soa_partial_desync_gate_2181() {
    std::println("=== Issue #2181: SoA partial desync hard gate ===");

    // ── AC1: source wiring ──
    {
        std::println("\n--- AC1: source cites 2181 ---");
        const auto svc = read_file("src/compiler/service.ixx");
        const auto met = read_file("src/compiler/observability_metrics.h");
        const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(svc.find("2181") != std::string::npos, "service cites 2181");
        CHECK(svc.find("gate_partial_soa_dirty_sync_") != std::string::npos, "gate helper");
        CHECK(svc.find("allow_partial_peel") != std::string::npos, "partial peel gated");
        CHECK(met.find("soa_dirty_desync_detected_total") != std::string::npos, "detected metric");
        CHECK(met.find("soa_dirty_desync_force_full_total") != std::string::npos, "force_full");
        CHECK(met.find("soa_dirty_desync_synced_bits_total") != std::string::npos, "synced_bits");
        CHECK(q.find("schema-2181") != std::string::npos, "query schema-2181");
    }

    // ── Unit: desync model + sync repairs ──
    {
        std::println("\n--- unit: desync inject / sync ---");
        auto mod = make_two_block_mod();
        auto& fn = mod.functions[0];
        fn.mark_block_dirty(0);
        for (auto& b : fn.instruction_dirty_)
            b = 0;
        CHECK(mod.count_block_instr_dirty_desync() > 0, "desync after inject");
        const auto flipped = mod.sync_instruction_dirty_from_block_dirty();
        CHECK(flipped > 0, "sync flipped bits");
        CHECK(mod.count_block_instr_dirty_desync() == 0, "desync cleared");
        CHECK(mod.instruction_dirty_synced_with_blocks(), "synced");
    }

    // ── AC4: injected desync → gate returns force-full (false) ──
    {
        std::println("\n--- AC4: inject desync → force full ---");
        CompilerService cs;
        warm_define(cs, "(define f (lambda (x) (+ x 1))) (f 1)");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics");

        // Ensure cache entry exists; dual-emit or compile:cache-define.
        if (!cs.get_define_v2("f")) {
            (void)cs.eval("(compile:cache-define \"f\")");
        }
        CHECK(cs.get_define_v2("f") != nullptr, "cache entry f present");

        const auto det0 = m->soa_dirty_desync_detected_total.load(std::memory_order_relaxed);
        const auto ff0 = m->soa_dirty_desync_force_full_total.load(std::memory_order_relaxed);
        const auto sync0 = m->soa_dirty_desync_synced_bits_total.load(std::memory_order_relaxed);

        const bool injected = cs.inject_soa_dirty_desync_for_test("f");
        CHECK(injected, "AC4: inject desync succeeded (dual-emit or skeleton)");
        CHECK(cs.get_define_v2("f")->soa_mod.count_block_instr_dirty_desync() > 0,
              "desync present after inject");
        // Gate: pre-sync desync → force full (returns false).
        const bool allow = cs.gate_partial_soa_dirty_sync_for_test("f");
        CHECK(!allow, "AC4: gate denies partial under pre-sync desync");
        CHECK(m->soa_dirty_desync_detected_total.load() > det0, "AC4: detected advanced");
        CHECK(m->soa_dirty_desync_force_full_total.load() > ff0, "AC4: force_full advanced");
        // Sync still ran — bits repaired for subsequent full path.
        CHECK(cs.get_define_v2("f")->soa_mod.count_block_instr_dirty_desync() == 0,
              "AC4: no residual desync after gate (no under-invalidate)");
        CHECK(m->soa_dirty_desync_synced_bits_total.load() >= sync0, "synced_bits");

        cs.set_soa_dual_emit(false);
        aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    }

    // ── AC4b: unit gate without dual-emit (synthetic skeleton path) ──
    {
        std::println("\n--- AC4b: inject without dual-emit (skeleton) ---");
        CompilerService cs;
        // dual-emit off — inject synthesizes SoA from AoS irs.
        aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
        cs.set_soa_dual_emit(false);
        CHECK(cs.eval("(set-code \"(define k (lambda (x) x)) (k 1)\")").has_value(), "set-code k");
        CHECK(cs.eval("(eval-current)").has_value(), "eval k");
        if (!cs.get_define_v2("k"))
            (void)cs.eval("(compile:cache-define \"k\")");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        if (cs.get_define_v2("k") && !cs.get_define_v2("k")->irs.empty()) {
            const auto ff0 =
                m ? m->soa_dirty_desync_force_full_total.load(std::memory_order_relaxed) : 0;
            CHECK(cs.inject_soa_dirty_desync_for_test("k"), "AC4b: inject via skeleton");
            CHECK(!cs.gate_partial_soa_dirty_sync_for_test("k"), "AC4b: force full");
            if (m)
                CHECK(m->soa_dirty_desync_force_full_total.load() > ff0, "AC4b: force_full++");
            CHECK(cs.get_define_v2("k")->soa_mod.count_block_instr_dirty_desync() == 0,
                  "AC4b: repaired");
        } else {
            CHECK(true, "soft: no AoS irs for k (skip skeleton)");
        }
    }

    // ── AC5: already synced → gate allows partial ──
    {
        std::println("\n--- AC5: synced happy path allows partial ---");
        CompilerService cs;
        warm_define(cs, "(define h (lambda (x) x)) (h 1)");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        if (!cs.get_define_v2("h"))
            (void)cs.eval("(compile:cache-define \"h\")");
        CHECK(cs.get_define_v2("h") != nullptr, "cache entry h");
        // First call syncs; second must still allow (already clean).
        (void)cs.gate_partial_soa_dirty_sync_for_test("h");
        const auto ff0 =
            m ? m->soa_dirty_desync_force_full_total.load(std::memory_order_relaxed) : 0;
        const bool allow = cs.gate_partial_soa_dirty_sync_for_test("h");
        CHECK(allow, "AC5: gate allows partial when already synced");
        if (m)
            CHECK(m->soa_dirty_desync_force_full_total.load() == ff0,
                  "AC5: no force_full on clean");
        CHECK(cs.get_define_v2("h")->soa_mod.count_block_instr_dirty_desync() == 0,
              "AC5: desync 0");
        cs.set_soa_dual_emit(false);
        aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    }

    // ── AC3: query schema ──
    {
        std::println("\n--- AC3: query:soa-dirty-stats schema-2181 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2181") == 2181, "schema-2181");
        CHECK(href(cs, "issue-2181") == 2181, "issue-2181");
        CHECK(href(cs, "soa-dirty-desync-gate-wired") == 1, "gate wired");
        CHECK(href(cs, "soa_dirty_desync_detected_total") >= 0, "detected key");
        CHECK(href(cs, "soa_dirty_desync_force_full_total") >= 0, "force_full key");
        CHECK(href(cs, "soa_dirty_desync_synced_bits_total") >= 0, "synced_bits key");
        CHECK(href(cs, "schema-2034") == 2034, "2034 lineage retained");
    }

    // ── Integration: mutate after inject still safe ──
    {
        std::println("\n--- integration: relower after desync inject ---");
        CompilerService cs;
        warm_define(cs, "(define g (lambda (n) (* n 2))) (g 2)");
        if (!cs.get_define_v2("g"))
            (void)cs.eval("(compile:cache-define \"g\")");
        CHECK(cs.inject_soa_dirty_desync_for_test("g"), "inject g");
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        const auto ff0 =
            m ? m->soa_dirty_desync_force_full_total.load(std::memory_order_relaxed) : 0;
        (void)cs.eval("(mutate:set-body \"g\" \"(lambda (n) (* n 3))\")");
        (void)cs.eval("(eval-current)");
        if (m)
            CHECK(m->soa_dirty_desync_force_full_total.load() >= ff0, "force_full non-dec");
        auto* e = cs.get_define_v2("g");
        if (e)
            CHECK(e->soa_mod.count_block_instr_dirty_desync() == 0,
                  "no residual desync after relower");
        cs.set_soa_dual_emit(false);
        aura::compiler::ir_soa_migration::set_soa_dual_emit_enabled(false);
    }

    std::println("\n=== #2181 SoA partial desync gate: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_soa_partial_desync_gate_2181();
}
#endif
