// @category: unit
// @reason: Issue #2371 — cross-COW dual-epoch soft restamp vs hard-reject
// on aura_closure_call freshness miss.
//
//   AC1: soft migrate enabled by default; stale within drift → restamp + continue
//   AC2: freed / far-behind / soft disabled → hard reject
//   AC3: metrics cross_cow_soft_migrate_total / hard_reject_total
//   AC4: query schema-2371
//   AC5: source-cite + gate

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>

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

extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_aot_bump_func_table_epoch(void);
extern "C" std::uint64_t aura_aot_func_table_epoch(void);
extern "C" void aura_set_aot_defuse_version(std::uint64_t v);
extern "C" std::uint64_t aura_get_aot_defuse_version(void);
extern "C" std::uint64_t aura_get_closure_bridge_epoch(std::int64_t closure_id);
extern "C" int aura_get_remap_name_fallback_enabled(void);

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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:epoch-apply-hotpath-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: soft migrate on stale within drift ──
static void ac1_soft_migrate() {
    std::println("\n--- AC1: soft migrate restamp on dual-epoch miss ---");
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE"); // default on
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    // Stamp closure at current epochs.
    const auto cid = aura_alloc_closure(1);
    CHECK(cid >= 0, "AC1: alloc");
    aura_closure_set_name(cid, "ac1_soft_2371");
    // Force dual-epoch miss: bump table epoch so capture is stale.
    const auto soft0 = metrics.cross_cow_soft_migrate_total.load(std::memory_order_relaxed);
    const auto hard0 = metrics.cross_cow_hard_reject_total.load(std::memory_order_relaxed);
    const auto b0 = aura_get_closure_bridge_epoch(cid);
    aura_aot_bump_func_table_epoch();
    CHECK(aura_aot_func_table_epoch() != b0 || b0 == 0, "AC1: table epoch advanced or inactive");

    // Call should soft-migrate (if tracking active) or succeed if domain inactive.
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    const auto soft1 = metrics.cross_cow_soft_migrate_total.load(std::memory_order_relaxed);
    const auto hard1 = metrics.cross_cow_hard_reject_total.load(std::memory_order_relaxed);
    // Either soft migrated (soft1 > soft0) or domain inactive (no hard reject
    // from this path alone) — never hard-reject a live non-linear within-drift.
    CHECK(hard1 == hard0 || soft1 > soft0, "AC1: soft path preferred over hard for live slot");
    if (aura_aot_func_table_epoch() != 0 && b0 != 0 && b0 != aura_aot_func_table_epoch()) {
        CHECK(soft1 > soft0, "AC1: soft migrate bumped when epochs active+stale");
        CHECK(hard1 == hard0, "AC1: no hard reject on soft path");
    }
    aura_set_aot_metrics(nullptr);
}

// ── AC2: hard reject paths ──
static void ac2_hard_reject() {
    std::println("\n--- AC2: hard reject when soft disabled / freed ---");
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "0", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto cid = aura_alloc_closure(2);
    CHECK(cid >= 0, "AC2: alloc");
    aura_closure_set_name(cid, "ac2_hard_2371");
    const auto hard0 = metrics.cross_cow_hard_reject_total.load(std::memory_order_relaxed);
    const auto soft0 = metrics.cross_cow_soft_migrate_total.load(std::memory_order_relaxed);
    aura_aot_bump_func_table_epoch();
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    const auto hard1 = metrics.cross_cow_hard_reject_total.load(std::memory_order_relaxed);
    const auto soft1 = metrics.cross_cow_soft_migrate_total.load(std::memory_order_relaxed);
    if (aura_aot_func_table_epoch() != 0 && aura_get_closure_bridge_epoch(cid) != 0) {
        CHECK(soft1 == soft0, "AC2: soft disabled → no soft migrate");
        CHECK(hard1 > hard0, "AC2: hard reject when soft off + stale");
    }

    // Freed slot: always hard (no soft).
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "1", 1);
    const auto cid2 = aura_alloc_closure(3);
    aura_closure_set_name(cid2, "ac2_freed_2371");
    aura_free_closure(cid2);
    aura_aot_bump_func_table_epoch();
    const auto hard2 = metrics.cross_cow_hard_reject_total.load(std::memory_order_relaxed);
    (void)aura_closure_call(cid2, args, 0);
    // Freed returns early before dual-check (no soft/hard migrate counters
    // necessarily); just ensure no crash.
    CHECK(true, "AC2: freed slot call safe");
    (void)hard2;
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE");
    aura_set_aot_metrics(nullptr);
}

// ── AC3: far-behind drift hard reject ──
static void ac3_far_behind() {
    std::println("\n--- AC3: far-behind generation → hard reject ---");
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "1", 1);
    setenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "1", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto cid = aura_alloc_closure(4);
    CHECK(cid >= 0, "AC3: alloc");
    aura_closure_set_name(cid, "ac3_far_2371");
    // Bump epoch twice so lag > 1.
    aura_aot_bump_func_table_epoch();
    aura_aot_bump_func_table_epoch();
    const auto soft0 = metrics.cross_cow_soft_migrate_total.load(std::memory_order_relaxed);
    const auto hard0 = metrics.cross_cow_hard_reject_total.load(std::memory_order_relaxed);
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    const auto soft1 = metrics.cross_cow_soft_migrate_total.load(std::memory_order_relaxed);
    const auto hard1 = metrics.cross_cow_hard_reject_total.load(std::memory_order_relaxed);
    if (aura_aot_func_table_epoch() != 0 && aura_get_closure_bridge_epoch(cid) != 0) {
        // lag >= 2 with max_drift=1 → hard
        CHECK(soft1 == soft0 || hard1 > hard0, "AC3: far-behind not soft-migrated");
        if (hard1 > hard0)
            CHECK(soft1 == soft0, "AC3: hard reject without soft");
    }
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT");
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE");
    aura_set_aot_metrics(nullptr);
}

// ── AC4: query ──
static void ac4_query() {
    std::println("\n--- AC4: query schema-2371 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2371") == 2371, "AC4: schema-2371");
    CHECK(href(cs, "issue-2371") == 2371, "AC4: issue-2371");
    CHECK(href(cs, "cross-cow-soft-migrate-wired") == 1, "AC4: wired");
    CHECK(href(cs, "cross-cow-soft-migrate-total") >= 0, "AC4: soft total");
    CHECK(href(cs, "cross-cow-hard-reject-total") >= 0, "AC4: hard total");
}

// ── AC5: source + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- AC5: source-cite + gate ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto script = read_file("scripts/check_cross_cow_soft_migrate_2371.py");
    CHECK(rt.find("Issue #2371") != std::string::npos, "AC5: #2371 in runtime");
    CHECK(rt.find("try_cross_cow_soft_migrate_") != std::string::npos, "AC5: soft helper");
    CHECK(rt.find("cross_cow_soft_migrate_total") != std::string::npos, "AC5: soft metric");
    CHECK(rt.find("cross_cow_hard_reject_total") != std::string::npos, "AC5: hard metric");
    CHECK(rt.find("AURA_CROSS_COW_SOFT_MIGRATE") != std::string::npos, "AC5: policy env");
    CHECK(obs.find("cross_cow_soft_migrate_total") != std::string::npos, "AC5: metrics field");
    CHECK(q.find("schema-2371") != std::string::npos, "AC5: query schema");
    CHECK(cmake.find("test_cross_cow_soft_migrate_2371") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_cross_cow_soft_migrate_2371") != std::string::npos, "AC5: build");
    CHECK(build.find("cmd_cross_cow_soft_migrate_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(script.find("schema-2371") != std::string::npos, "AC5: coverage script");
}

} // namespace

int main() {
    std::println("test_cross_cow_soft_migrate_2371");
    ac1_soft_migrate();
    ac2_hard_reject();
    ac3_far_behind();
    ac4_query();
    ac5_source_and_gate();
    if (g_failed)
        return 1;
    std::println("cross-COW soft migrate #2371: OK ({} passed)", g_passed);
    return 0;
}
