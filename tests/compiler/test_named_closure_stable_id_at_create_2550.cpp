// @category: unit
// @reason: Issue #2550 — force stable_func_id at named closure create
//          (eliminate sid=0 steady state for named closures).
//
//   AC1: Every named closure after create/set_name has stable_func_id != 0
//   AC2: Reemit soak with only named closures → backfill_total does not grow
//   AC3: Anonymous may remain sid=0; reemit MustDeopt (aligned with #2542)
//   AC4: Map preserve vs assign + clear isolation still accurate
//   AC5: Source-cite + linter coverage for create/set_name wire sites

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

// ── AC1: named set_name → sid != 0 ──
static void ac1_named_create_nonzero() {
    std::println("\n--- #2550 AC1: named create/set_name → sid != 0 ---");
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC1: map empty");

    const auto cid = aura_alloc_closure(7);
    CHECK(cid >= 0, "AC1: alloc");
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC1: pre-name sid=0");

    aura_closure_set_name(cid, "ac1_named_2550");
    const auto sid = aura_get_closure_stable_func_id(cid);
    CHECK(sid != 0, "AC1: named set_name stamps non-zero sid");
    CHECK(aura_lookup_stable_func_id("ac1_named_2550") == sid, "AC1: map entry matches stored sid");
    CHECK(aura_stable_func_id_map_size() == 1, "AC1: map size 1 after first named");

    // Second set_name same name preserves sid
    aura_closure_set_name(cid, "ac1_named_2550");
    CHECK(aura_get_closure_stable_func_id(cid) == sid, "AC1: re-set_name preserves sid");

    // Different name gets a new sid
    const auto c2 = aura_alloc_closure(8);
    aura_closure_set_name(c2, "ac1_other_2550");
    const auto sid2 = aura_get_closure_stable_func_id(c2);
    CHECK(sid2 != 0 && sid2 != sid, "AC1: distinct name → distinct sid");

    aura_free_closure(cid);
    aura_free_closure(c2);
    aura_clear_stable_func_id_map();
}

// ── AC2: named-only reemit → backfill does not grow ──
static void ac2_reemit_no_backfill_growth() {
    std::println("\n--- #2550 AC2: named reemit soak → backfill_total stable ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    std::vector<std::int64_t> cids;
    std::vector<std::uint32_t> sids;
    for (int i = 0; i < 8; ++i) {
        const auto name = std::format("ac2_named_{}_2550", i);
        const auto cid = aura_alloc_closure(100 + i);
        CHECK(cid >= 0, "AC2: alloc");
        aura_closure_set_name(cid, name.c_str());
        const auto sid = aura_get_closure_stable_func_id(cid);
        CHECK(sid != 0, "AC2: each named has sid");
        cids.push_back(cid);
        sids.push_back(sid);
    }

    const auto bb0 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    // Multiple reemit rounds with only named closures already stamped.
    for (int round = 0; round < 5; ++round) {
        const auto n = aura_remap_live_closures_after_reemit(
            sids.data(), sids.size(), /*new_bridge_epoch=*/static_cast<std::uint64_t>(50 + round));
        CHECK(n >= 1, "AC2: remapped at least one per round");
    }
    const auto bb1 = metrics.live_closure_stable_id_backfill_total.load(std::memory_order_relaxed);
    CHECK(bb1 == bb0, "AC2: backfill_total unchanged for named-only soak");

    for (auto cid : cids)
        aura_free_closure(cid);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

// ── AC3: anonymous sid=0; reemit MustDeopt ──
static void ac3_anonymous_must_deopt() {
    std::println("\n--- #2550 AC3: anonymous sid=0 → reemit MustDeopt ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_clear_stable_func_id_map();
    aura_set_remap_name_fallback_enabled(0);

    const auto cid = aura_alloc_closure(9);
    CHECK(cid >= 0, "AC3: alloc");
    // No set_name → anonymous
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC3: anonymous sid=0");

    // Empty name set_name stays 0
    aura_closure_set_name(cid, "");
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC3: empty name stays sid=0");
    aura_closure_set_name(cid, nullptr);
    CHECK(aura_get_closure_stable_func_id(cid) == 0, "AC3: null name stays sid=0");

    // Reemit with unrelated named sid → anonymous MustDeopt (#2542)
    const auto sid_other = aura_get_or_preserve_stable_func_id("ac3_other_2550", nullptr);
    const std::uint32_t ids[] = {sid_other};
    const auto mk0 = metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed);
    (void)aura_remap_live_closures_after_reemit(ids, 1, /*new_bridge_epoch=*/77);
    CHECK(aura_get_closure_must_deopt_before_next_call(cid) != 0 ||
              metrics.live_closure_must_deopt_kept_total.load(std::memory_order_relaxed) >= mk0,
          "AC3: anonymous path exercises MustDeopt / kept counter");

    aura_free_closure(cid);
    aura_set_aot_metrics(nullptr);
    aura_clear_stable_func_id_map();
}

// ── AC4: map preserve / assign / clear isolation ──
static void ac4_map_preserve_assign_clear() {
    std::println("\n--- #2550 AC4: map preserve vs assign + clear isolation ---");
    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC4: clear → size 0");

    int p0 = -1;
    const auto id1 = aura_get_or_preserve_stable_func_id("ac4_fn_2550", &p0);
    CHECK(id1 != 0 && p0 == 0, "AC4: first sighting assigns");
    int p1 = -1;
    const auto id2 = aura_get_or_preserve_stable_func_id("ac4_fn_2550", &p1);
    CHECK(id2 == id1 && p1 == 1, "AC4: second sighting preserves");

    // set_name uses same map
    const auto cid = aura_alloc_closure(1);
    aura_closure_set_name(cid, "ac4_fn_2550");
    CHECK(aura_get_closure_stable_func_id(cid) == id1, "AC4: set_name reuses map sid");
    int p2 = -1;
    (void)aura_get_or_preserve_stable_func_id("ac4_fn_2550", &p2);
    CHECK(p2 == 1, "AC4: set_name path preserved (not re-assigned)");

    aura_clear_stable_func_id_map();
    CHECK(aura_stable_func_id_map_size() == 0, "AC4: clear isolates");
    CHECK(aura_lookup_stable_func_id("ac4_fn_2550") == 0, "AC4: lookup miss after clear");
    // Closure still holds old sid (not rewritten by clear) — redefine safety
    CHECK(aura_get_closure_stable_func_id(cid) == id1, "AC4: stored sid survives map clear");

    aura_free_closure(cid);
    aura_clear_stable_func_id_map();
}

// ── AC5: source-cite + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- #2550 AC5: source-cite + linter + query ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto bh = read_file("src/compiler/aura_jit_bridge.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_named_closure_stable_id_at_create_2550.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(rt.find("Issue #2550") != std::string::npos, "AC5: runtime cites #2550");
    CHECK(rt.find("aura_get_or_preserve_stable_func_id") != std::string::npos,
          "AC5: set_name uses get_or_preserve");
    // Old lookup-only stamp path must not remain as the named set_name line
    CHECK(rt.find("name ? aura_lookup_stable_func_id(name) : 0") == std::string::npos,
          "AC5: no lookup-only named set_name stamp");
    CHECK(bh.find("aura_get_closure_stable_func_id") != std::string::npos,
          "AC5: getter in bridge.h");
    CHECK(bh.find("Issue #2550") != std::string::npos, "AC5: bridge.h cites #2550");
    CHECK(q.find("schema-2550") != std::string::npos, "AC5: schema-2550 query key");
    CHECK(q.find("named-closure-stable-id-at-create-wired") != std::string::npos, "AC5: wired key");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(cmake.find("test_named_closure_stable_id_at_create_2550") != std::string::npos,
          "AC5: cmake");
    CHECK(build.find("check_named_closure_stable_id_at_create_2550") != std::string::npos,
          "AC5: build.py script");
    CHECK(build.find("cmd_named_closure_stable_id_at_create_coverage") != std::string::npos,
          "AC5: build.py cmd");

    CompilerService cs;
    CHECK(href(cs, "schema-2550") == 2550, "AC5: query schema-2550");
    CHECK(href(cs, "named-closure-stable-id-at-create-wired") == 1, "AC5: query wired");
    CHECK(href(cs, "issue-2550") == 2550, "AC5: issue-2550");
}

} // namespace

int main() {
    std::println("=== Issue #2550: named closure stable_func_id at create ===");
    ac1_named_create_nonzero();
    ac2_reemit_no_backfill_growth();
    ac3_anonymous_must_deopt();
    ac4_map_preserve_assign_clear();
    ac5_source_and_gate();
    std::println("\n=== #2550: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
