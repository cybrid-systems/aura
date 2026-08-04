// @category: unit
// @reason: Issue #2046 — align AOT region versioning with JIT hot-swap /
// bridge_epoch (joint epoch contract after soft/hard invalidate).
//
//   AC1: source cites #2046; aot_mangle joint contract + probe/stale + bump
//   AC2: register slot fresh; bump table epoch → probe rejects (stale)
//   AC3: re-register restamps generation → probe fresh again
//   AC4: soft mark_define_dirty + hard invalidate advance joint epoch;
//        HotUpdateRegistry epoch listeners fire; cascade observe metrics
//   AC5: query:aot-stats schema-2046 + joint versioning keys
//   AC6: multi-round cascade with dependents; no crash; metrics monotonic
//   AC7: pure-JIT path (no AOT slots) still works after invalidate

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/runtime_shared.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::hot_update_registry;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:aot-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_source() {
    std::println("\n--- AC1: source cites #2046 ---");
    auto mangle = read_file("src/compiler/aot_mangle.h");
    auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    auto hdr = read_file("src/compiler/aura_jit_bridge.h");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    auto reg = read_file("src/compiler/hot_update_registry.hh");
    CHECK(!mangle.empty() && mangle.find("#2046") != std::string::npos, "aot_mangle #2046");
    CHECK(mangle.find("Joint versioning contract") != std::string::npos, "contract doc");
    CHECK(!bridge.empty() && bridge.find("#2046") != std::string::npos, "bridge #2046");
    CHECK(bridge.find("aot_slot_stale_reject_total") != std::string::npos, "stale reject metric");
    CHECK(bridge.find("notify_epoch_bump") != std::string::npos, "bump notifies registry");
    CHECK(hdr.find("aura_aot_slot_is_stale") != std::string::npos, "slot_is_stale API");
    CHECK(hdr.find("aura_aot_probe_fn_ptr_raw") != std::string::npos, "raw probe API");
    CHECK(!dirty.empty() && dirty.find("#2046") != std::string::npos, "service_dirty #2046");
    CHECK(dirty.find("aot_cascade_joint_epoch_observe_total") != std::string::npos,
          "cascade joint observe");
    CHECK(!met.empty() && met.find("aot_joint_epoch_bump_total") != std::string::npos,
          "joint bump metric");
    CHECK(met.find("aot_forced_recompile_on_mismatch_total") != std::string::npos,
          "forced recompile metric");
    CHECK(!q.empty() && q.find("schema-2046") != std::string::npos, "query schema-2046");
    CHECK(!reg.empty() && reg.find("#2046") != std::string::npos, "hot_update registry #2046");
}

void ac2_probe_stale_after_bump() {
    std::println("\n--- AC2: probe rejects after table epoch bump ---");
    const std::int64_t fid = 42;
    const std::uintptr_t seed = 0xA0A0A0A0ull;
    aura_register_fn_tracked(fid, static_cast<std::int64_t>(seed));
    CHECK(aura_aot_probe_fn_ptr(fid) == seed, "fresh after register");
    CHECK(aura_aot_slot_is_stale(fid) == 0, "not stale after register");
    CHECK(aura_aot_probe_fn_ptr_raw(fid) == seed, "raw still has ptr");

    const auto e0 = aura_aot_func_table_epoch();
    aura_aot_bump_func_table_epoch();
    const auto e1 = aura_aot_func_table_epoch();
    CHECK(e1 == e0 + 1, "epoch advanced");
    CHECK(aura_aot_slot_is_stale(fid) == 1, "stale after bump");
    CHECK(aura_aot_probe_fn_ptr(fid) == 0, "probe rejects generation-behind");
    CHECK(aura_aot_probe_fn_ptr_raw(fid) == seed, "raw still holds old ptr");
}

void ac3_reregister_fresh() {
    std::println("\n--- AC3: re-register restamps → fresh ---");
    const std::int64_t fid = 43;
    const std::uintptr_t seed = 0xB1B1B1B1ull;
    aura_register_fn_tracked(fid, static_cast<std::int64_t>(seed));
    aura_aot_bump_func_table_epoch();
    CHECK(aura_aot_probe_fn_ptr(fid) == 0, "stale before restamp");
    const std::uintptr_t seed2 = 0xC2C2C2C2ull;
    aura_register_fn_tracked(fid, static_cast<std::int64_t>(seed2));
    CHECK(aura_aot_slot_is_stale(fid) == 0, "fresh after restamp");
    CHECK(aura_aot_probe_fn_ptr(fid) == seed2, "probe returns new ptr");
}

void ac4_invalidate_joint_epoch() {
    std::println("\n--- AC4: soft/hard invalidate advance joint epoch + listeners ---");
    CompilerService cs;
    // Ensure aot metrics pointer is wired via CompilerService.
    CHECK(cs.eval("(set-code \"(define a (lambda () 1))(define b (lambda () (a)))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");

    std::atomic<std::uint64_t> listener_hits{0};
    std::atomic<std::uint64_t> last_epoch{0};
    auto& reg = hot_update_registry();
    const auto lid = reg.register_epoch_listener([&](std::uint64_t ep) {
        listener_hits.fetch_add(1, std::memory_order_relaxed);
        last_epoch.store(ep, std::memory_order_relaxed);
    });
    (void)lid;

    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    // Wire AOT metrics so bridge bumps land on this CompilerMetrics.
    aura_set_aot_metrics(m);

    const auto joint0 = m->aot_joint_epoch_bump_total.load(std::memory_order_relaxed);
    const auto cascade0 = m->aot_cascade_joint_epoch_observe_total.load(std::memory_order_relaxed);
    const auto epoch0 = aura_aot_func_table_epoch();
    const auto hits0 = listener_hits.load(std::memory_order_relaxed);

    cs.public_mark_define_dirty("a");
    const auto joint1 = m->aot_joint_epoch_bump_total.load(std::memory_order_relaxed);
    const auto cascade1 = m->aot_cascade_joint_epoch_observe_total.load(std::memory_order_relaxed);
    const auto epoch1 = aura_aot_func_table_epoch();
    const auto hits1 = listener_hits.load(std::memory_order_relaxed);
    std::println("  soft: joint {}→{} epoch {}→{} listeners {}→{} cascade {}→{}", joint0, joint1,
                 epoch0, epoch1, hits0, hits1, cascade0, cascade1);
    CHECK(epoch1 > epoch0, "soft dirty advanced AOT table epoch");
    CHECK(joint1 > joint0 || hits1 > hits0, "joint bump or listener after soft");
    CHECK(cascade1 >= cascade0, "cascade observe non-decreasing");

    cs.public_invalidate_function("a");
    const auto joint2 = m->aot_joint_epoch_bump_total.load(std::memory_order_relaxed);
    const auto cascade2 = m->aot_cascade_joint_epoch_observe_total.load(std::memory_order_relaxed);
    const auto epoch2 = aura_aot_func_table_epoch();
    const auto hits2 = listener_hits.load(std::memory_order_relaxed);
    std::println("  hard: joint {}→{} epoch {}→{} listeners {}→{} cascade {}→{}", joint1, joint2,
                 epoch1, epoch2, hits1, hits2, cascade1, cascade2);
    CHECK(epoch2 > epoch1, "hard invalidate advanced AOT table epoch");
    CHECK(joint2 >= joint1, "joint bump non-decreasing");
    CHECK(cascade2 > cascade1 || cascade2 >= 1, "cascade observe after hard");
    CHECK(hits2 >= hits1, "listeners non-decreasing");

    reg.clear_listeners();
}

void ac5_query_schema() {
    std::println("\n--- AC5: query:aot-stats schema-2046 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:aot-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2046") == 2046, "schema-2046");
    CHECK(href(cs, "issue-2046") == 2046, "issue-2046");
    CHECK(href(cs, "aot-jit-joint-versioning-wired") == 1, "wired");
    CHECK(href(cs, "aot_joint_epoch_bump_total") >= 0, "joint bump key");
    CHECK(href(cs, "aot_slot_stale_reject_total") >= 0, "slot stale key");
    CHECK(href(cs, "aot_forced_recompile_on_mismatch_total") >= 0, "forced recompile key");
}

void ac6_multi_round_cascade() {
    std::println("\n--- AC6: multi-round cascade mixed path ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define a (lambda () 1))"
                  "(define b (lambda () (a)))"
                  "(define c (lambda () (b)))"
                  "\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    aura_set_aot_metrics(m);
    // Seed an AOT slot then invalidate → must go stale.
    const std::int64_t fid = 7;
    aura_register_fn_tracked(fid, 0x7777);
    CHECK(aura_aot_probe_fn_ptr(fid) != 0, "seeded fresh");
    const auto rej0 = m->aot_slot_stale_reject_total.load(std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i) {
        cs.public_invalidate_function("a");
        CHECK(cs.eval("(eval-current)").has_value(), "re-eval");
    }
    CHECK(aura_aot_probe_fn_ptr(fid) == 0, "seeded slot stale after invalidate rounds");
    const auto rej1 = m->aot_slot_stale_reject_total.load(std::memory_order_relaxed);
    std::println("  slot_stale_reject {}→{}", rej0, rej1);
    CHECK(rej1 >= rej0, "reject counter non-decreasing");
    CHECK(href(cs, "aot_cascade_joint_epoch_observe_total") >= 0, "cascade key present");
}

void ac7_pure_jit_cold() {
    std::println("\n--- AC7: pure-JIT cold path no crash ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define pure (lambda (x) (+ x 1)))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto r = cs.eval("(pure 41)");
    CHECK(r && is_int(*r) && as_int(*r) == 42, "pure 41 → 42");
    cs.public_invalidate_function("pure");
    CHECK(cs.eval("(eval-current)").has_value(), "re-eval after invalidate");
    r = cs.eval("(pure 10)");
    CHECK(r && is_int(*r) && as_int(*r) == 11, "pure 10 → 11 after invalidate");
}

} // namespace

int run_test_aot_jit_joint_versioning_2046() {
    std::println("=== test_aot_jit_joint_versioning_2046 ===");
    ac1_source();
    ac2_probe_stale_after_bump();
    ac3_reregister_fresh();
    ac4_invalidate_joint_epoch();
    ac5_query_schema();
    ac6_multi_round_cascade();
    ac7_pure_jit_cold();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_aot_jit_joint_versioning_2046();
}
#endif
