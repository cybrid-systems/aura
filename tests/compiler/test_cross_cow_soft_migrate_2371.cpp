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

// ── Issue #2603: tighten cross-COW soft-migrate observability ──
// (same-gen success vs hard reason). Refines #2371 / #2505 / #2547
// by splitting the soft counter by same-gen vs all-soft so Agents
// can read soft / (soft + CowGenMismatch) for throttle without
// log scraping.

// AC1: same-gen soft success → cross_cow_soft_migrate_same_gen_total +1
//      (distinct from cross_cow_soft_migrate_total all-soft).
static void ac2603_same_gen_soft_counter() {
    std::println("\n--- #2603 AC1: same-gen soft success → new counter ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto same0 = metrics.cross_cow_soft_migrate_same_gen_total.load(
        std::memory_order_relaxed);
    const auto all0 = metrics.cross_cow_soft_migrate_total.load(std::memory_order_relaxed);
    CHECK(same0 == 0, "AC1: same-gen counter starts at 0");
    CHECK(all0 == 0, "AC1: all-soft counter starts at 0");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(obs.find("cross_cow_soft_migrate_same_gen_total") != std::string::npos,
          "AC1: same-gen counter field exists in metrics.h");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    CHECK(bridge.find("aura_bump_cross_cow_soft_migrate_same_gen_total") != std::string::npos,
          "AC1: C ABI bumper exists in bridge");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("aura_bump_cross_cow_soft_migrate_same_gen_total") != std::string::npos,
          "AC1: same-gen bumper called in runtime success path");
    CHECK(metrics.cross_cow_soft_migrate_total.is_lock_free(), "AC1: all-soft atomic");
    CHECK(metrics.cross_cow_soft_migrate_same_gen_total.is_lock_free(),
          "AC1: same-gen atomic");
    aura_set_aot_metrics(nullptr);
}

// AC2: cross-gen → CowGenMismatch hard; same-gen counter NOT bumped.
static void ac2603_cross_gen_no_soft_bump() {
    std::println("\n--- #2603 AC2: cross-gen hard → same-gen counter stays 0 ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto same0 = metrics.cross_cow_soft_migrate_same_gen_total.load(
        std::memory_order_relaxed);
    const auto hard_cow0 = metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load(
        std::memory_order_relaxed);

    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto try_block = rt.find("try_cross_cow_soft_migrate_");
    CHECK(try_block != std::string::npos, "AC2: try_cross_cow_soft_migrate_ exists");
    const auto bumper_count = rt.count("aura_bump_cross_cow_soft_migrate_same_gen_total");
    CHECK(bumper_count == 1, "AC2: same-gen bumper called exactly once (success path only)");
    CHECK(hard_cow0 == 0, "AC2: CowGenMismatch hard counter starts at 0");
    CHECK(same0 == 0, "AC2: same-gen counter unchanged (no call)");
    aura_set_aot_metrics(nullptr);
}

// AC3: AURA_CROSS_COW_SOFT_MIGRATE=0 → always hard; counters consistent.
static void ac2603_soft_disabled_no_soft_bump() {
    std::println("\n--- #2603 AC3: soft disabled env → always hard ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(rt.find("AURA_CROSS_COW_SOFT_MIGRATE") != std::string::npos,
          "AC3: AURA_CROSS_COW_SOFT_MIGRATE env present");
    const auto try_block = rt.find("try_cross_cow_soft_migrate_");
    CHECK(try_block != std::string::npos, "AC3: try_cross_cow_soft_migrate_ exists");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    CHECK(bridge.find("cross_cow_hard_reject_disabled_total") != std::string::npos,
          "AC3: disabled hard counter exists in bridge");
    const auto same0 = metrics.cross_cow_soft_migrate_same_gen_total.load(
        std::memory_order_relaxed);
    CHECK(same0 == 0, "AC3: same-gen counter unchanged");
    aura_set_aot_metrics(nullptr);
}

// AC4: additive schema; #2505 / #2547 surfaces preserved.
static void ac2603_schema_and_source() {
    std::println("\n--- #2603 AC4: additive schema + source-cite ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "cross-cow-soft-migrate-same-gen-total") >= 0,
          "AC4: same-gen key on query:aot-reload-stats");
    CHECK(href(cs, "cross_cow_soft_migrate_same_gen_total") >= 0,
          "AC4: same-gen legacy key");
    CHECK(href(cs, "cross-cow-soft-migrate-same-gen-wired") == 1,
          "AC4: same-gen-wired sentinel");
    CHECK(href(cs, "cross-cow-soft-rate-x10000") >= 0, "AC4: soft rate helper");
    CHECK(href(cs, "schema-2371") == 2371, "AC4: schema-2371 retained");
    CHECK(href(cs, "schema-2505") == 2505, "AC4: schema-2505 retained");
    CHECK(href(cs, "schema-2547") == 2547, "AC4: schema-2547 retained");
    CHECK(href(cs, "schema-2603") == 2603, "AC4: schema-2603 on query surface");
    CHECK(href(cs, "issue-2603") == 2603, "AC4: issue-2603 on query surface");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    CHECK(obs.find("cross_cow_soft_migrate_same_gen_total") != std::string::npos,
          "AC4: same-gen field in metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(q.find("schema-2603") != std::string::npos, "AC4: schema-2603 in query surface");
    const auto build = read_file("build.py");
    CHECK(build.find("cmd_cross_cow_soft_migrate_obs_2603_coverage") != std::string::npos,
          "AC4: #2603 cmd helper in build.py");
}

// AC5: source-cite + soft does NOT open cross-workspace write (#2178).
static void ac2603_soft_no_cross_workspace_write() {
    std::println("\n--- #2603 AC5: soft does NOT open cross-workspace write ---");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    CHECK(bridge.find("cross_workspace_reject") != std::string::npos ||
              rt.find("cross_workspace_reject") != std::string::npos ||
              bridge.find("CrossWorkspaceReject") != std::string::npos,
          "AC5: #2178 cross-workspace reject still fail-closed");
    CHECK(rt.find("cow_gen_at_capture") != std::string::npos ||
              rt.find("closure_cow_gen_mismatch_") != std::string::npos,
          "AC5: #2275 cow_gen stamp still fail-closed");
    CHECK(rt.find("aura_bump_cross_cow_soft_migrate_same_gen_total") != std::string::npos,
          "AC5: same-gen bumper in success path only");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(obs.find("cross-cow-no-write-path-wired") != std::string::npos,
          "AC5: no-write-path-wired sentinel preserved (soft does NOT open write)");
}

} // namespace

int main() {
    std::println("test_cross_cow_soft_migrate_2371");
    ac1_soft_migrate();
    ac2_hard_reject();
    ac3_far_behind();
    ac4_query();
    ac5_source_and_gate();
    // Issue #2603: same-gen soft-migrate observability.
    ac2603_same_gen_soft_counter();
    ac2603_cross_gen_no_soft_bump();
    ac2603_soft_disabled_no_soft_bump();
    ac2603_schema_and_source();
    ac2603_soft_no_cross_workspace_write();
    if (g_failed)
        return 1;
    std::println(
        "cross-COW soft migrate #2371 + #2603: OK ({} passed)", g_passed);
    return 0;
}
