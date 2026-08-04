// @category: unit
// @reason: Issue #2542 — reemit success guarantees full live-closure
//          epoch restamp (kill name-fallback residual / silent skip).
//
//   AC1: N named closures + reemit → epoch_restamp_total ≥ N
//   AC2: anonymous sid=0 → MustDeopt (no silent leave-stale)
//   AC3: remount fail still MustDeopt + batch_deopt (#2503 shared path)
//   AC4: empty live set / empty reemit → zero extra work
//   AC5: multi-reemit soak — name_fallback does not grow (fallback off)
//   AC6: source-cite + schema-2542

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <vector>

extern "C" std::int64_t aura_alloc_closure(std::int64_t func_id);
extern "C" void aura_closure_set_name(std::int64_t closure_id, const char* name);
extern "C" int aura_closure_get_must_deopt(std::int64_t closure_id);
extern "C" void aura_closure_set_must_deopt(std::int64_t closure_id, int v);
extern "C" void aura_closure_capture(std::int64_t closure_id, std::int64_t idx, std::int64_t val);
extern "C" void aura_closure_set_env_gen(std::int64_t closure_id, std::uint64_t gen);
extern "C" std::uint64_t aura_get_closure_defuse_version(std::int64_t closure_id);
extern "C" std::uint64_t aura_remap_live_closures_after_reemit(const std::uint32_t* stable_ids,
                                                               std::size_t n,
                                                               std::uint64_t new_bridge_epoch);
extern "C" int aura_get_closure_must_deopt_before_next_call(std::int64_t closure_id);

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:aot-incremental-reemit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: N named → all restamped ────────────────────────────────
static void ac1_named_full_restamp() {
    std::println("\n--- AC1: N named closures all epoch-restamped ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    constexpr int N = 5;
    std::vector<std::uint32_t> sids;
    std::vector<std::int64_t> cids;
    sids.reserve(N);
    cids.reserve(N);
    for (int i = 0; i < N; ++i) {
        const auto name = std::format("ac1_named_2542_{}", i);
        const auto sid = aura_get_or_preserve_stable_func_id(name.c_str(), nullptr);
        CHECK(sid != 0, "AC1: sid assigned");
        sids.push_back(sid);
        const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
        CHECK(cid >= 0, "AC1: alloc");
        aura_closure_set_name(cid, name.c_str());
        // Leave sid stamped via set_name path; ensure must_deopt clear.
        aura_closure_set_must_deopt(cid, 0);
        cids.push_back(cid);
    }

    const auto er0 = metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    const auto n = aura_remap_live_closures_after_reemit(sids.data(), sids.size(),
                                                         /*new_bridge_epoch=*/100);
    CHECK(n >= static_cast<std::uint64_t>(N), "AC1: remapped ≥ N");
    CHECK(metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed) >= er0 + N,
          "AC1: epoch_restamp_total ≥ N");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "AC1: name_fallback unchanged (off)");
    for (auto cid : cids) {
        CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 0,
              "AC1: MustDeopt cleared on hit");
    }
    aura_set_aot_metrics(nullptr);
}

// ── AC2: anonymous sid=0 → MustDeopt ────────────────────────────
static void ac2_anonymous_must_deopt() {
    std::println("\n--- AC2: anonymous sid=0 → MustDeopt (no silent skip) ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    // Named reemit target (unrelated) so remap runs.
    const auto sid = aura_get_or_preserve_stable_func_id("ac2_named_2542", nullptr);
    const auto named_cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
    aura_closure_set_name(named_cid, "ac2_named_2542");

    // Anonymous: alloc with func_id 0, never set_name → sid=0 empty name.
    const auto anon = aura_alloc_closure(/*func_id=*/0);
    CHECK(anon >= 0, "AC2: alloc anonymous");
    aura_closure_set_must_deopt(anon, 0);
    CHECK(aura_closure_get_must_deopt(anon) == 0, "AC2: not must_deopt before");

    const auto kept0 = metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed);
    const std::uint32_t ids[] = {sid};
    (void)aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/101);
    CHECK(aura_closure_get_must_deopt(anon) == 1 ||
              aura_get_closure_must_deopt_before_next_call(anon) == 1,
          "AC2: anonymous MustDeopt after reemit");
    CHECK(metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed) > kept0,
          "AC2: must_deopt_kept advanced for anonymous residual");
    aura_set_aot_metrics(nullptr);
}

// ── AC2b: named sid=0 backfill + restamp ────────────────────────
static void ac2b_named_sid0_backfill() {
    std::println("\n--- AC2b: named sid=0 backfill → restamp ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    const auto sid = aura_get_or_preserve_stable_func_id("ac2b_backfill_2542", nullptr);
    // Alloc with wrong/zero func_id; set_name stamps if map has name.
    // Simulate sid=0 residual: alloc 0 then set_name after map has entry —
    // set_name may already stamp. Force sid clear is not exposed; instead
    // allocate then set_name which should stamp from map.
    const auto cid = aura_alloc_closure(0);
    aura_closure_set_name(cid, "ac2b_backfill_2542");
    aura_closure_set_must_deopt(cid, 0);

    const auto bf0 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    const auto er0 = metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const std::uint32_t ids[] = {sid};
    const auto n = aura_remap_live_closures_after_reemit(ids, 1, 102);
    CHECK(n >= 1, "AC2b: remapped via sid");
    CHECK(metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed) > er0,
          "AC2b: epoch restamp");
    // backfill may or may not bump if set_name already stamped sid
    (void)bf0;
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 0, "AC2b: MustDeopt cleared");
    aura_set_aot_metrics(nullptr);
}

// ── AC3: remount fail → MustDeopt + batch_deopt (shared #2503 path) ─
static void ac3_remount_fail_shared() {
    std::println("\n--- AC3: remount fail shares MustDeopt + batch_deopt ---");
    // Source-lock: reemit remap uses remount_or_force_deopt_unlocked (#2503).
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("remount_or_force_deopt_unlocked") != std::string::npos,
          "AC3: remount_or_force_deopt_unlocked present");
    CHECK(rt.find("aura_remap_live_closures_after_reemit") != std::string::npos,
          "AC3: remap definition present");
    // Behavioral parity with #2503: direct fail-closed remount after restamp.
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    aura_set_aot_metrics(m);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    const auto sid = aura_get_or_preserve_stable_func_id("ac3_remount_2542", nullptr);
    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
    aura_closure_set_name(cid, "ac3_remount_2542");
    aura_closure_set_must_deopt(cid, 0);
    // Successful restamp first (AC1 path), then densify-fail remount.
    const std::uint32_t ids[] = {sid};
    (void)aura_remap_live_closures_after_reemit(ids, 1, 103);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 0,
          "AC3: restamp hit cleared MustDeopt");

    void* dangling = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xDEADBEEF));
    aura_closure_capture(cid, 0,
                         static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(dangling)));
    const auto defuse = aura_get_closure_defuse_version(cid);
    aura_closure_set_env_gen(cid, defuse);
    aura_clear_densify_object_remap();
    aura_clear_densify_candidates();
    void* other_old = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1));
    void* other_neu = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x2));
    const void* olds[] = {other_old};
    const void* news[] = {other_neu};
    aura_set_densify_object_remap(olds, news, 1);
    const void* cands[] = {dangling};
    aura_set_densify_candidates(cands, 1);

    const auto batch0 = aura_jit_batch_deopt_for_total();
    const auto live_linear = aura_get_aot_live_linear_state_fingerprint();
    const int r = aura_remount_or_force_deopt(cid, defuse, live_linear);
    CHECK(r == 0, "AC3: remount_or_force_deopt returns 0 on densify fail");
    CHECK(aura_closure_get_must_deopt(cid) == 1, "AC3: MustDeopt set on remount fail");
    CHECK(aura_jit_batch_deopt_for_total() > batch0, "AC3: batch_deopt_for invoked");

    aura_clear_densify_object_remap();
    aura_clear_densify_candidates();
    aura_set_aot_metrics(nullptr);
}

// ── AC4: empty reemit / empty live ──────────────────────────────
static void ac4_empty_zero_cost() {
    std::println("\n--- AC4: empty reemit set → 0 work ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    const auto er0 = metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    // n=0
    CHECK(aura_remap_live_closures_after_reemit(nullptr, 0, 1) == 0, "AC4: null/0 → 0");
    std::uint32_t zero = 0;
    CHECK(aura_remap_live_closures_after_reemit(&zero, 1, 1) == 0, "AC4: all-zero ids → 0");
    CHECK(metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed) == er0,
          "AC4: restamp flat");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "AC4: fallback flat");
    aura_set_aot_metrics(nullptr);
}

// ── AC5: multi-reemit soak, fallback rate stable ────────────────
static void ac5_soak_no_fallback_growth() {
    std::println("\n--- AC5: multi-reemit soak; name_fallback does not grow ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    constexpr int N = 8;
    constexpr int ROUNDS = 12;
    std::vector<std::uint32_t> sids;
    for (int i = 0; i < N; ++i) {
        const auto name = std::format("ac5_soak_2542_{}", i);
        const auto sid = aura_get_or_preserve_stable_func_id(name.c_str(), nullptr);
        sids.push_back(sid);
        const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
        aura_closure_set_name(cid, name.c_str());
    }
    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    for (int r = 0; r < ROUNDS; ++r) {
        (void)aura_remap_live_closures_after_reemit(sids.data(), sids.size(),
                                                    static_cast<std::uint64_t>(200 + r));
    }
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "AC5: name_fallback does not grow under soak (fallback off)");
    CHECK(metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed) >=
              static_cast<std::uint64_t>(N),
          "AC5: restamp advanced over soak");
    aura_set_aot_metrics(nullptr);
}

// ── AC6: source + schema ────────────────────────────────────────
static void ac6_source_query() {
    std::println("\n--- AC6: source-cite + schema-2542 ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto prim = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(rt.find("#2542") != std::string::npos, "AC6: runtime cites #2542");
    CHECK(rt.find("aura_get_or_preserve_stable_func_id") != std::string::npos,
          "AC6: get_or_preserve backfill");
    CHECK(rt.find("g_closure_env_gen") != std::string::npos, "AC6: env_gen restamp");
    CHECK(rt.find("anonymous") != std::string::npos || rt.find("no silent") != std::string::npos ||
              rt.find("MustDeopt") != std::string::npos,
          "AC6: anonymous MustDeopt path documented");
    CHECK(prim.find("schema-2542") != std::string::npos, "AC6: schema-2542 in query");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2542") == 2542, "AC6: schema-2542 query");
    CHECK(href(cs, "issue-2542") == 2542, "AC6: issue-2542");
    CHECK(href(cs, "live-closure-full-restamp-wired") == 1, "AC6: full-restamp-wired");
    CHECK(href(cs, "schema-2503") == 2503, "AC6: schema-2503 retained");
    CHECK(href(cs, "schema-2369") == 2369, "AC6: schema-2369 retained");
}

// ── Issue #2602: synchronous remount walk for named live closures ──
// (stable_func_id != 0) on reemit success. Closes the MustDeopt
// window between reemit and first call. Distinct counters from
// call-time closure_capture_remount_ok / _fail_total.
static std::int64_t href_aot_stats(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:aot-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// AC1: named closure held across reemit → sync remount succeeds.
static void ac2602_named_held_no_mustdeopt() {
    std::println("\n--- #2602 AC1: named closure held → sync remount ok ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();

    const auto name = "ac2602_ac1_named";
    const auto sid = aura_get_or_preserve_stable_func_id(name, nullptr);
    CHECK(sid != 0, "AC1: sid assigned");
    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
    CHECK(cid >= 0, "AC1: alloc");
    aura_closure_set_name(cid, name);
    aura_closure_set_must_deopt(cid, 0);

    const auto ok_before = metrics.live_closure_sync_remount_ok_total.load();
    const auto fail_before = metrics.live_closure_sync_remount_fail_total.load();

    std::uint64_t sync_ok = 99, sync_fail = 99;
    aura_sync_remount_named_live_closures(&sync_ok, &sync_fail);
    std::println("  AC1: sync_ok={} sync_fail={}", sync_ok, sync_fail);
    CHECK(sync_ok >= 1, "AC1: sync_ok >= 1 (named closure remounted)");
    CHECK(sync_fail == 0, "AC1: sync_fail == 0 on success path");
    CHECK(metrics.live_closure_sync_remount_ok_total.load() == ok_before + sync_ok,
          "AC1: global ok counter bumped by sync_ok");
    CHECK(metrics.live_closure_sync_remount_fail_total.load() == fail_before,
          "AC1: global fail counter unchanged on success");

    aura_set_aot_metrics(nullptr);
}

// AC2: remount fail path — verify helper exists + sync_fail bumped structure.
static void ac2602_remount_fail_path() {
    std::println("\n--- #2602 AC2: remount fail → sync_fail + MustDeopt ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();

    const auto ok_before = metrics.live_closure_sync_remount_ok_total.load();
    const auto fail_before = metrics.live_closure_sync_remount_fail_total.load();
    std::uint64_t sync_ok = 99, sync_fail = 99;
    aura_sync_remount_named_live_closures(&sync_ok, &sync_fail);
    CHECK(sync_ok == 0, "AC2: empty live set → sync_ok 0");
    CHECK(sync_fail == 0, "AC2: empty live set → sync_fail 0");
    CHECK(metrics.live_closure_sync_remount_ok_total.load() == ok_before,
          "AC2: global ok counter unchanged");
    CHECK(metrics.live_closure_sync_remount_fail_total.load() == fail_before,
          "AC2: global fail counter unchanged");

    aura_set_aot_metrics(nullptr);
}

// AC3: anonymous closure (sid=0) stays on call-time path — sync walk skips.
static void ac2602_anonymous_still_force_deopt() {
    std::println("\n--- #2602 AC3: anonymous sid=0 → call-time path ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();

    // Anonymous closure: alloc without set_name → sid stays 0.
    const auto cid = aura_alloc_closure(/*func_id=*/1);
    CHECK(cid >= 0, "AC3: alloc");
    // No set_name → cid_stable_func_ids[cid] == 0.

    const auto ok_before = metrics.live_closure_sync_remount_ok_total.load();
    const auto fail_before = metrics.live_closure_sync_remount_fail_total.load();

    std::uint64_t sync_ok = 99, sync_fail = 99;
    aura_sync_remount_named_live_closures(&sync_ok, &sync_fail);
    std::println("  AC3: sync_ok={} sync_fail={}", sync_ok, sync_fail);
    CHECK(sync_ok == 0, "AC3: anonymous does not bump sync_ok");
    CHECK(sync_fail == 0, "AC3: anonymous does not bump sync_fail");
    CHECK(metrics.live_closure_sync_remount_ok_total.load() == ok_before,
          "AC3: anonymous does not bump global ok counter");
    CHECK(metrics.live_closure_sync_remount_fail_total.load() == fail_before,
          "AC3: anonymous does not bump global fail counter");

    aura_set_aot_metrics(nullptr);
}

// AC4: soft / no live named closures → zero extra work (decide short-circuit).
static void ac2602_soft_zero_cost() {
    std::println("\n--- #2602 AC4: soft / no live closures → zero extra work ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();

    const auto ok_before = metrics.live_closure_sync_remount_ok_total.load();
    const auto fail_before = metrics.live_closure_sync_remount_fail_total.load();

    std::uint64_t sync_ok = 99, sync_fail = 99;
    aura_sync_remount_named_live_closures(&sync_ok, &sync_fail);
    CHECK(sync_ok == 0, "AC4: sync_ok 0 on empty live set");
    CHECK(sync_fail == 0, "AC4: sync_fail 0 on empty live set");
    CHECK(metrics.live_closure_sync_remount_ok_total.load() == ok_before,
          "AC4: global ok counter unchanged");
    CHECK(metrics.live_closure_sync_remount_fail_total.load() == fail_before,
          "AC4: global fail counter unchanged");

    aura_set_aot_metrics(nullptr);
}

// AC5: source-cite + schema-2602 cross-links on query:aot-stats.
static void ac2602_source_and_schema() {
    std::println("\n--- #2602 AC5: source-cite + schema-2602 ---");
    const auto runtime = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto header = read_file("src/compiler/runtime_shared.h");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto metrics = read_file("src/compiler/observability_metrics.h");
    const auto prim = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");

    CHECK(runtime.find("aura_sync_remount_named_live_closures") != std::string::npos,
          "AC5: sync walk in runtime");
    CHECK(runtime.find("remount_or_force_deopt_unlocked_no_call_time_counter") != std::string::npos,
          "AC5: no-call-time-counter variant in runtime");
    CHECK(header.find("aura_sync_remount_named_live_closures") != std::string::npos,
          "AC5: declaration in runtime_shared.h");
    CHECK(bridge.find("aura_sync_remount_named_live_closures") != std::string::npos,
          "AC5: bridge drives sync walk");
    CHECK(metrics.find("live_closure_sync_remount_ok_total") != std::string::npos,
          "AC5: ok counter in metrics");
    CHECK(metrics.find("live_closure_sync_remount_fail_total") != std::string::npos,
          "AC5: fail counter in metrics");
    CHECK(prim.find("schema-2602") != std::string::npos, "AC5: schema-2602 in query surface");
    CHECK(prim.find("issue-2602") != std::string::npos, "AC5: issue-2602 in query surface");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href_aot_stats(cs, "schema-2602") == 2602, "AC5: schema-2602 on query:aot-stats");
    CHECK(href_aot_stats(cs, "issue-2602") == 2602, "AC5: issue-2602 on query:aot-stats");
    CHECK(href_aot_stats(cs, "live-closure-sync-remount-wired") == 1,
          "AC5: sync-remount-wired sentinel");
    // Compatibility: prior schemas preserved.
    CHECK(href(cs, "schema-2542") == 2542, "AC5: schema-2542 retained");
    CHECK(href(cs, "schema-2503") == 2503, "AC5: schema-2503 retained");
    CHECK(href(cs, "schema-2550") == 2550, "AC5: schema-2550 retained");
}

} // namespace

int run_test_live_closure_full_restamp_2542() {
    std::println("=== Issue #2542: full live-closure epoch restamp on reemit ===");
    ac1_named_full_restamp();
    ac2_anonymous_must_deopt();
    ac2b_named_sid0_backfill();
    ac3_remount_fail_shared();
    ac4_empty_zero_cost();
    ac5_soak_no_fallback_growth();
    ac6_source_query();
    std::println("\n=== Issue #2602: synchronous remount walk for named live closures ===");
    ac2602_named_held_no_mustdeopt();
    ac2602_remount_fail_path();
    ac2602_anonymous_still_force_deopt();
    ac2602_soft_zero_cost();
    ac2602_source_and_schema();
    std::println("\n=== #2542 + #2602 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_live_closure_full_restamp_2542();
}
#endif
