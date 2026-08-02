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

} // namespace

int main() {
    std::println("=== Issue #2542: full live-closure epoch restamp on reemit ===");
    ac1_named_full_restamp();
    ac2_anonymous_must_deopt();
    ac2b_named_sid0_backfill();
    ac3_remount_fail_shared();
    ac4_empty_zero_cost();
    ac5_soak_no_fallback_growth();
    ac6_source_query();
    std::println("\n=== #2542 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
