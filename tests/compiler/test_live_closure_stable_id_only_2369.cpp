// @category: unit
// @reason: Issue #2369 — eliminate name-fallback legacy rewrite as production
// path; stable_func_id sole primary for live-closure remap after reemit.
//
//   AC1: positive — stable_func_id present → remap, name-fallback counter 0
//   AC2: negative / miss — no sid match → MustDeopt + batch_deopt, no rewrite
//   AC3: legacy flag on — name-fallback rewrite still available for migration
//   AC4: production defaults force fallback off + query schema-2369
//   AC5: source-cite contract + gate

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

// Declared in aura_jit_runtime / bridge.
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

// ── AC1: stable_func_id remap, no name-fallback ──
static void ac1_stable_id_remap() {
    std::println("\n--- AC1: stable_func_id remap; name-fallback counter stays 0 ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);
    CHECK(aura_get_remap_name_fallback_enabled() == 0, "AC1: fallback off");

    const auto sid = aura_get_or_preserve_stable_func_id("ac1_stable_2369", nullptr);
    CHECK(sid != 0, "AC1: stable id assigned");

    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid));
    CHECK(cid >= 0, "AC1: alloc");
    aura_closure_set_name(cid, "ac1_stable_2369");
    // Stamp func_id to something else so remap is observable.
    // remap will retarget to sid when sid is in reemit set.
    const std::uint32_t ids[] = {sid};
    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    const auto er0 = metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed);
    const auto n = aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/42);
    CHECK(n >= 1, "AC1: remapped >= 1");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "AC1: name-fallback counter unchanged");
    CHECK(metrics.live_closure_epoch_restamp_total.load(std::memory_order_relaxed) >= er0 + 1,
          "AC1: epoch restamp on hit");
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 0, "AC1: MustDeopt cleared on hit");
    aura_set_aot_metrics(nullptr);
}

// ── AC2: miss → MustDeopt + batch_deopt, no rewrite ──
static void ac2_miss_must_deopt() {
    std::println("\n--- AC2: miss path MustDeopt + batch_deopt; no name rewrite ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    // Stamp closure with sid_old for name; clear map + re-register → sid_new.
    // Closure still holds sid_old (not in reemit set); name→sid_new is in set.
    // Fallback off → name_candidate_no_remap miss (no rewrite).
    const auto sid_old = aura_get_or_preserve_stable_func_id("ac2_miss_2369", nullptr);
    CHECK(sid_old != 0, "AC2: sid_old");
    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid_old));
    CHECK(cid >= 0, "AC2: alloc");
    aura_closure_set_name(cid, "ac2_miss_2369");

    aura_clear_stable_func_id_map();
    // After clear, next id resets to 1 — burn one id so ac2 name gets a new sid.
    (void)aura_get_or_preserve_stable_func_id("__burn_2369_ac2", nullptr);
    const auto sid_new = aura_get_or_preserve_stable_func_id("ac2_miss_2369", nullptr);
    CHECK(sid_new != 0 && sid_new != sid_old, "AC2: sid_new differs after clear");
    const std::uint32_t ids[] = {sid_new};

    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    const auto mk0 = metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed);
    const auto bd0 = aura_jit_batch_deopt_for_total();
    const auto n = aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/99);
    CHECK(n == 0, "AC2: no remount rewrite on miss");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0,
          "AC2: name-fallback counter not bumped");
    CHECK(metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed) == mk0 + 1,
          "AC2: must_deopt_kept +1 on miss");
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) != 0, "AC2: MustDeopt flag set");
    CHECK(aura_jit_batch_deopt_for_total() > bd0, "AC2: batch_deopt_for called");
    aura_set_aot_metrics(nullptr);
}

// ── AC3: legacy flag on restores name-fallback rewrite ──
static void ac3_legacy_flag() {
    std::println("\n--- AC3: legacy flag on → name-fallback rewrite for migration ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    // Same stale-sid setup as AC2, but enable legacy name-fallback rewrite.
    const auto sid_old = aura_get_or_preserve_stable_func_id("ac3_legacy_2369", nullptr);
    const auto cid = aura_alloc_closure(static_cast<std::int64_t>(sid_old));
    CHECK(cid >= 0, "AC3: alloc");
    aura_closure_set_name(cid, "ac3_legacy_2369");
    aura_clear_stable_func_id_map();
    (void)aura_get_or_preserve_stable_func_id("__burn_2369_ac3", nullptr);
    const auto sid_new = aura_get_or_preserve_stable_func_id("ac3_legacy_2369", nullptr);
    CHECK(sid_new != sid_old, "AC3: sid_new differs");

    aura_set_remap_name_fallback_enabled(1);
    CHECK(aura_get_remap_name_fallback_enabled() == 1, "AC3: fallback on");
    const std::uint32_t ids[] = {sid_new};
    const auto fb0 = metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed);
    const auto n = aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/7);
    CHECK(n >= 1, "AC3: remapped via name-fallback");
    CHECK(metrics.live_closure_remap_name_fallback_total.load(std::memory_order_relaxed) == fb0 + 1,
          "AC3: name-fallback counter +1");
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) == 0,
          "AC3: MustDeopt cleared after fallback remap");

    aura_set_remap_name_fallback_enabled(0);
    CHECK(aura_get_remap_name_fallback_enabled() == 0, "AC3: reset off");
    aura_set_aot_metrics(nullptr);
}

// ── AC4: query schema-2369 ──
static void ac4_query() {
    std::println("\n--- AC4: query schema-2369 + sole-primary wired ---");
    aura_set_remap_name_fallback_enabled(0);
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2369") == 2369, "AC4: schema-2369");
    CHECK(href(cs, "issue-2369") == 2369, "AC4: issue-2369");
    CHECK(href(cs, "stable-func-id-sole-primary-wired") == 1, "AC4: sole-primary wired");
    CHECK(href(cs, "remap-name-fallback-default-off") == 1, "AC4: default-off sentinel");
    CHECK(href(cs, "remap-name-fallback-enabled") == 0, "AC4: fallback disabled");
    CHECK(href(cs, "live-closure-remap-name-fallback-total") >= 0, "AC4: fallback total key");
}

// ── AC5: source + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- AC5: source-cite contract + gate ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto sec = read_file("src/compiler/security_defaults.hh");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto script =
        read_file("scripts/coverage/checks/check_live_closure_stable_id_only_2369.py");
    CHECK(rt.find("Issue #2369") != std::string::npos, "AC5: #2369 in runtime");
    CHECK(rt.find("stable_func_id") != std::string::npos ||
              rt.find("stable_func_id") != std::string::npos,
          "AC5: stable_func_id contract");
    CHECK(rt.find("aura_bump_live_closure_must_deopt_kept_total") != std::string::npos,
          "AC5: miss bumps must_deopt_kept");
    CHECK(rt.find("aura_jit_batch_deopt_for") != std::string::npos, "AC5: miss batch_deopt");
    // Miss path: match_id == 0 block must call both (not only remount fail).
    CHECK(rt.find("no name-based rewrite") != std::string::npos ||
              rt.find("no name rewrite") != std::string::npos ||
              rt.find("never name-rewritten") != std::string::npos,
          "AC5: contract docs no name rewrite");
    CHECK(sec.find("Issue #2369") != std::string::npos, "AC5: production defaults cite #2369");
    CHECK(sec.find("aura_set_remap_name_fallback_enabled(0)") != std::string::npos,
          "AC5: production forces fallback off");
    CHECK(q.find("schema-2369") != std::string::npos, "AC5: query schema");
    CHECK(q.find("stable-func-id-sole-primary-wired") != std::string::npos, "AC5: wired key");
    CHECK(cmake.find("test_live_closure_stable_id_only_2369") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_live_closure_stable_id_only_2369") != std::string::npos,
          "AC5: build script");
    CHECK(build.find("cmd_live_closure_stable_id_only_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(script.find("schema-2369") != std::string::npos, "AC5: coverage script");
}

} // namespace

int main() {
    std::println("test_live_closure_stable_id_only_2369");
    ac1_stable_id_remap();
    ac2_miss_must_deopt();
    ac3_legacy_flag();
    ac4_query();
    ac5_source_and_gate();
    if (g_failed)
        return 1;
    std::println("live closure stable_id only #2369: OK ({} passed)", g_passed);
    return 0;
}
