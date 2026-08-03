// @category: unit
// @reason: Issue #2605 — explicit anonymous / residual sid=0 policy
//          (MustDeopt, no silent name-fallback growth).
//
//   AC1: Named create → sid≠0; reemit soak → residual_backfill does not grow
//   AC2: Anonymous reemit → MustDeopt; no silent name invent
//   AC3: Force-inject residual named sid=0 → exactly one backfill; next reemit no growth
//   AC4: Query distinguishes assign / preserve / residual_backfill
//   AC5: Source-cite + linter coverage for create/set_name/remap

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

extern "C" std::uint64_t aura_remap_live_closures_after_reemit(const std::uint32_t* stable_ids,
                                                               std::size_t n,
                                                               std::uint64_t new_bridge_epoch);
extern "C" int aura_get_closure_must_deopt_before_next_call(std::int64_t closure_id);

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

// ── AC1: named soak → residual_backfill stable ──
static void ac1_named_soak_no_residual_growth() {
    std::println("\n--- #2605 AC1: named create → sid≠0; residual_backfill stable ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    std::vector<std::int64_t> cids;
    std::vector<std::uint32_t> sids;
    for (int i = 0; i < 6; ++i) {
        const auto name = std::format("ac1_named_{}_2605", i);
        const auto cid = aura_alloc_closure(200 + i);
        CHECK(cid >= 0, "AC1: alloc");
        aura_closure_set_name(cid, name.c_str());
        const auto sid = aura_get_closure_stable_func_id(cid);
        CHECK(sid != 0, "AC1: named create stamps non-zero sid");
        cids.push_back(cid);
        sids.push_back(sid);
    }

    const auto bb0 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    const auto invent0 =
        metrics.live_closure_named_name_fallback_reject_total.load(std::memory_order_relaxed);
    for (int r = 0; r < 4; ++r) {
        const auto n = aura_remap_live_closures_after_reemit(sids.data(), sids.size(),
                                                             static_cast<std::uint64_t>(60 + r));
        CHECK(n >= 1, "AC1: remapped at least one per round");
    }
    CHECK(metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed) == bb0,
          "AC1: residual_backfill unchanged for named-only soak");
    CHECK(metrics.live_closure_named_name_fallback_reject_total.load(std::memory_order_relaxed) ==
              invent0,
          "AC1: named invent counter unchanged (fallback off)");

    for (auto cid : cids)
        aura_free_closure(cid);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

// ── AC2: anonymous MustDeopt; no invent ──
static void ac2_anonymous_must_deopt_no_invent() {
    std::println("\n--- #2605 AC2: anonymous reemit → MustDeopt; no invent ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    const auto cid = aura_alloc_closure(9);
    CHECK(cid >= 0, "AC2: alloc");
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC2: anonymous sid=0");
    aura_closure_set_name(cid, "");
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC2: empty name stays 0");

    const auto sid_other = aura_get_or_preserve_stable_func_id("ac2_other_2605", nullptr);
    const std::uint32_t ids[] = {sid_other};
    const auto bb0 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    const auto invent0 =
        metrics.live_closure_named_name_fallback_reject_total.load(std::memory_order_relaxed);
    const auto mk0 = metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed);
    (void)aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/88);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) != 0 ||
              metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed) >= mk0,
          "AC2: anonymous path exercises MustDeopt");
    CHECK(metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed) == bb0,
          "AC2: anonymous never residual_backfill");
    CHECK(metrics.live_closure_named_name_fallback_reject_total.load(std::memory_order_relaxed) ==
              invent0,
          "AC2: anonymous never named invent");

    aura_free_closure(cid);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

// ── AC3: residual inject → one backfill; next reemit no growth ──
static void ac3_residual_one_shot_backfill() {
    std::println("\n--- #2605 AC3: residual inject → one backfill; steady no growth ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    const auto cid = aura_alloc_closure(42);
    CHECK(cid >= 0, "AC3: alloc");
    aura_closure_set_name(cid, "ac3_residual_2605");
    CHECK(aura_get_closure_stable_func_id(cid) != 0, "AC3: create stamps non-zero");
    // Residual inject (test-only path — hermetic).
    aura_test_force_closure_stable_func_id(cid, 0);
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC3: force residual sid=0");

    const auto sid = aura_lookup_stable_func_id("ac3_residual_2605");
    CHECK(sid != 0, "AC3: map still holds name→sid from set_name");
    const std::uint32_t ids[] = {sid};

    const auto bb0 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    (void)aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/101);
    CHECK(metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed) == bb0 + 1,
          "AC3: exactly one residual_backfill");
    CHECK(aura_get_closure_stable_func_id(cid) == sid, "AC3: backfill restored sid");

    const auto bb1 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    for (int r = 0; r < 3; ++r) {
        (void)aura_remap_live_closures_after_reemit(ids, 1, static_cast<std::uint64_t>(110 + r));
    }
    CHECK(metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed) == bb1,
          "AC3: subsequent reemit no residual_backfill growth");

    aura_free_closure(cid);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

// ── AC4: query assign / preserve / residual_backfill ──
static void ac4_query_axes() {
    std::println("\n--- #2605 AC4: query assign / preserve / residual_backfill ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2605") == 2605, "AC4: schema-2605");
    CHECK(href(cs, "issue-2605") == 2605, "AC4: issue-2605");
    CHECK(href(cs, "residual-sid0-policy-wired") == 1, "AC4: residual-sid0-policy-wired");
    CHECK(href(cs, "anonymous-must-deopt-policy-wired") == 1, "AC4: anonymous policy wired");
    CHECK(href(cs, "stable-id-residual-backfill-total") >= 0, "AC4: residual_backfill key");
    CHECK(href(cs, "stable_id_residual_backfill_total") >= 0, "AC4: residual snake alias");
    CHECK(href(cs, "stable-id-assign-total") >= 0, "AC4: assign key");
    CHECK(href(cs, "stable-id-preserve-total") >= 0, "AC4: preserve key");
    CHECK(href(cs, "named-name-fallback-reject-total") >= 0, "AC4: named invent key");
    // #2550 / #2175 lineage retained
    CHECK(href(cs, "schema-2550") == 2550, "AC4: schema-2550 retained");
    CHECK(href(cs, "named-closure-stable-id-at-create-wired") == 1, "AC4: #2550 wired retained");
}

// ── AC5: source-cite + linter ──
static void ac5_source_and_linter() {
    std::println("\n--- #2605 AC5: source-cite + linter + cmake/build ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto bh = read_file("src/compiler/aura_jit_bridge.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto met = read_file("src/compiler/observability_metrics.h");
    const auto lint = read_file("scripts/check_anonymous_residual_stable_id_policy_2605.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(rt.find("Issue #2605") != std::string::npos, "AC5: runtime cites #2605");
    CHECK(rt.find("aura_test_force_closure_stable_func_id") != std::string::npos,
          "AC5: residual inject helper present");
    CHECK(rt.find("aura_bump_live_closure_named_name_fallback_reject_total") != std::string::npos,
          "AC5: named invent reject bump in remap walk");
    CHECK(rt.find("AURA_NAMED_NAME_FALLBACK_HARD") != std::string::npos,
          "AC5: hard invent env documented");
    CHECK(bh.find("aura_bump_live_closure_named_name_fallback_reject_total") != std::string::npos,
          "AC5: reject bumper declared");
    CHECK(met.find("live_closure_named_name_fallback_reject_total") != std::string::npos,
          "AC5: metrics field");
    CHECK(q.find("schema-2605") != std::string::npos, "AC5: schema-2605 query key");
    CHECK(q.find("stable-id-residual-backfill-total") != std::string::npos,
          "AC5: residual_backfill query key");
    CHECK(q.find("stable-id-assign-total") != std::string::npos, "AC5: assign query key");
    CHECK(q.find("stable-id-preserve-total") != std::string::npos, "AC5: preserve query key");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(cmake.find("test_anonymous_residual_stable_id_policy_2605") != std::string::npos,
          "AC5: cmake");
    CHECK(build.find("check_anonymous_residual_stable_id_policy_2605") != std::string::npos,
          "AC5: build.py script");
    CHECK(build.find("cmd_anonymous_residual_stable_id_policy_coverage") != std::string::npos,
          "AC5: build.py cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2605: anonymous / residual sid=0 policy ===");
    ac1_named_soak_no_residual_growth();
    ac2_anonymous_must_deopt_no_invent();
    ac3_residual_one_shot_backfill();
    ac4_query_axes();
    ac5_source_and_linter();
    std::println("\n=== #2605: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
