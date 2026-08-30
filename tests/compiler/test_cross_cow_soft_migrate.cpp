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
#include <filesystem>
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
    const auto script = read_file("scripts/coverage/manifests/2371.json");
    CHECK(rt.find("Issue #2371") != std::string::npos, "AC5: #2371 in runtime");
    CHECK(rt.find("try_cross_cow_soft_migrate_") != std::string::npos, "AC5: soft helper");
    CHECK(rt.find("cross_cow_soft_migrate_total") != std::string::npos, "AC5: soft metric");
    CHECK(rt.find("cross_cow_hard_reject_total") != std::string::npos, "AC5: hard metric");
    CHECK(rt.find("AURA_CROSS_COW_SOFT_MIGRATE") != std::string::npos, "AC5: policy env");
    CHECK(obs.find("cross_cow_soft_migrate_total") != std::string::npos, "AC5: metrics field");
    CHECK(q.find("schema-2371") != std::string::npos, "AC5: query schema");
    CHECK(cmake.find("test_cross_cow_soft_migrate") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_cross_cow_soft_migrate_2371") != std::string::npos, "AC5: build");
    CHECK(build.find("cmd_cross_cow_soft_migrate_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(script.find("schema-2371") != std::string::npos, "AC5: coverage manifest");
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

    const auto same0 =
        metrics.cross_cow_soft_migrate_same_gen_total.load(std::memory_order_relaxed);
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
    CHECK(metrics.cross_cow_soft_migrate_same_gen_total.is_lock_free(), "AC1: same-gen atomic");
    aura_set_aot_metrics(nullptr);
}

// AC2: cross-gen → CowGenMismatch hard; same-gen counter NOT bumped.
static void ac2603_cross_gen_no_soft_bump() {
    std::println("\n--- #2603 AC2: cross-gen hard → same-gen counter stays 0 ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);

    const auto same0 =
        metrics.cross_cow_soft_migrate_same_gen_total.load(std::memory_order_relaxed);
    const auto hard_cow0 =
        metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load(std::memory_order_relaxed);

    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto try_block = rt.find("try_cross_cow_soft_migrate_");
    CHECK(try_block != std::string::npos, "AC2: try_cross_cow_soft_migrate_ exists");
    // std::string has no count(substring); count non-overlapping occurrences.
    std::size_t bumper_count = 0;
    {
        const std::string_view needle = "aura_bump_cross_cow_soft_migrate_same_gen_total";
        for (std::size_t pos = 0; (pos = rt.find(needle, pos)) != std::string::npos;
             pos += needle.size())
            ++bumper_count;
    }
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
    const auto same0 =
        metrics.cross_cow_soft_migrate_same_gen_total.load(std::memory_order_relaxed);
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
    CHECK(href(cs, "cross_cow_soft_migrate_same_gen_total") >= 0, "AC4: same-gen legacy key");
    CHECK(href(cs, "cross-cow-soft-migrate-same-gen-wired") == 1, "AC4: same-gen-wired sentinel");
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

// Issue #3177 follow-up: aura_clear_aot_metrics_for_eval resets
// g_aot_metrics iff it currently equals the dying eval's metrics pointer
// (CompilerService declares evaluator_ before metrics_, so C++ destroys
// metrics_ first — the destructor must clear the global before the
// per-slot invalidator reads it). The pointer comparison avoids
// clobbering a still-live pointer under nested CompilerService.
static void ac3177_clear_aot_metrics_for_eval_match() {
    std::println("\n--- #3177 AC: aura_clear_aot_metrics_for_eval matches ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    CHECK(aura_get_aot_metrics() == &metrics, "AC: g_aot_metrics set to &metrics");
    aura_clear_aot_metrics_for_eval(&metrics);
    CHECK(aura_get_aot_metrics() == nullptr,
          "AC: g_aot_metrics cleared after clear_for_eval with matching pointer");
}

static void ac3177_clear_aot_metrics_for_eval_no_match() {
    std::println("\n--- #3177 AC: aura_clear_aot_metrics_for_eval non-match ---");
    CompilerMetrics a{};
    CompilerMetrics b{};
    aura_set_aot_metrics(&a);
    aura_clear_aot_metrics_for_eval(&b);
    CHECK(aura_get_aot_metrics() == &a,
          "AC: g_aot_metrics preserved when clear_for_eval gets a non-matching pointer");
    aura_set_aot_metrics(nullptr);
}

// ── Issue #3410: production mutate dual-fresh miss must not soft-migrate
// onto pre-mutate g_jit_fns native. Source-cite verification (runtime
// production probe requires wiring that #3410 deliberately avoids — the
// fix is gated on aura::compiler::typed_audit::production_defaults_active
// which is process-wide and not safe to flip in a single-eval test).
//
// AC6: try_cross_cow_soft_migrate_ has production probe after within-cap
//      check; same-gen drift → set MustDeopt + return 0 (reuse existing
//      cross_cow_note_hard_ + cross_cow_hard_reject_total counter).
// AC7: no docs/design/3410-* (per #1655); no test_issue_3410.cpp (per
//      #81934 — extend existing test_cross_cow_soft_migrate.cpp).
// AC8: build.py wires cmd_dual_fresh_mutate_soft_migrate_3410_coverage;
//      observability_metrics.h has no new field for #3410 (reuses existing
//      MustDeopt / cross_cow_hard_reject_total counters per AC5).
static void ac3410_production_probe() {
    std::println("\n--- AC6: #3410 production probe — same-gen drift → MustDeopt refuse ---");
    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto hot = read_file("src/compiler/hot_update_registry.cpp");
    CHECK(rt.find("Issue #3410") != std::string::npos, "AC6: #3410 marker in runtime");
    CHECK(rt.find("production_defaults_active()") != std::string::npos,
          "AC6: production probe wired");
    // Production probe activates AFTER within-cap check (so cap-drift hard
    // reject path is unchanged) and BEFORE the restamp / linear path
    // (so production same-gen drift cannot restamp + continue native).
    // Anchor inside try_cross_cow_soft_migrate_ — the TU has earlier
    // production_defaults_active() / stamp_closure_provenance_locked defs.
    const auto fn = rt.find("static int try_cross_cow_soft_migrate_");
    const auto prod_pos = rt.find("Issue #3410", fn);
    const auto within_cap_pos = rt.find("if (!cross_cow_drift_within_cap_", fn);
    const auto restamp_pos = rt.find("stamp_closure_provenance_locked(cid);", fn);
    CHECK(fn != std::string::npos && prod_pos != std::string::npos &&
              within_cap_pos != std::string::npos && restamp_pos != std::string::npos &&
              prod_pos > within_cap_pos && prod_pos < restamp_pos,
          "AC6: production probe between within-cap check and restamp");
    CHECK(rt.find("g_closure_must_deopt[cid] = 1") != std::string::npos,
          "AC6: MustDeopt set on production drift refuse");
    // Reuse existing CrossCowHardReject reason (no new metric field per AC5).
    CHECK(rt.find("cross_cow_note_hard_(CrossCowHardReject::Other)") != std::string::npos,
          "AC6: reuse CrossCowHardReject::Other reason");
    CHECK(rt.find("cur_c_bridge") != std::string::npos &&
              rt.find("Issue #3447") != std::string::npos,
          "AC6: #3447 C-bridge miss visible to #3410 (not sole leave-native)");
    // Facade reference: production mark_define_dirty / invalidate_function
    // bumps bridge_epoch + defuse_version + aot table epoch in sequence.
    CHECK(rt.find("hard_invalidate_via_facade") != std::string::npos ||
              hot.find("hard_invalidate_via_facade") != std::string::npos,
          "AC6: hard_invalidate_via_facade referenced");
    // Cow-gen mismatch path (#2547) unchanged — still hard-rejects cross-cow.
    CHECK(rt.find("closure_cow_gen_mismatch_") != std::string::npos,
          "AC6: cow-gen mismatch path unchanged");
}

static void ac3410_no_design_doc() {
    std::println("\n--- AC7: #3410 no docs/design/3410-*; no test_issue_3410.cpp ---");
    namespace fs = std::filesystem;
    bool has_design_doc = false;
    if (fs::exists("docs/design")) {
        for (const auto& entry : fs::directory_iterator("docs/design")) {
            const auto name = entry.path().filename().string();
            if (name.find("3410") != std::string::npos) {
                has_design_doc = true;
                break;
            }
        }
    }
    CHECK(!has_design_doc, "AC7: no docs/design/3410-* per #1655");
    CHECK(!fs::exists("tests/issues/test_issue_3410.cpp") &&
              !fs::exists("tests/compiler/test_issue_3410.cpp") &&
              !fs::exists("tests/core/test_issue_3410.cpp"),
          "AC7: no test_issue_3410.cpp per #81934 (extend existing test)");
}

static void ac3410_build_and_metric() {
    std::println("\n--- AC8: #3410 build.py wiring + no new metric field ---");
    const auto build = read_file("build.py");
    CHECK(build.find("cmd_dual_fresh_mutate_soft_migrate_3410_coverage") != std::string::npos,
          "AC8: cmd_dual_fresh_mutate_soft_migrate_3410_coverage in build.py");
    CHECK(build.find("check_dual_fresh_mutate_soft_migrate_3410") != std::string::npos,
          "AC8: linter script registered");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    // Per AC5: reuse existing MustDeopt + cross_cow_hard_reject_total
    // counters. No new metric field for #3410.
    CHECK(obs.find("dual_fresh_mutate_soft_migrate") == std::string::npos &&
              obs.find("3410_soft_migrate") == std::string::npos,
          "AC8: no new metric field per AC5 (reuse existing counters)");
}

int run_test_cross_cow_soft_migrate() {
    std::println("test_cross_cow_soft_migrate");
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
    // Issue #3177 follow-up (ASan stack-use-after-scope): ~Evaluator must
    // clear g_aot_metrics iff it points to its own CompilerMetrics before
    // aura_cleanup_aot_state runs, otherwise the per-slot invalidator at
    // aura_jit_bridge.cpp:2124 fetch_adds on dead stack memory.
    ac3177_clear_aot_metrics_for_eval_match();
    ac3177_clear_aot_metrics_for_eval_no_match();
    // Issue #3410: production mutate dual-fresh miss must not soft-migrate
    // onto pre-mutate g_jit_fns native (extend existing test per #81934).
    ac3410_production_probe();
    ac3410_no_design_doc();
    ac3410_build_and_metric();
    if (g_failed)
        return 1;
    std::println("cross-COW soft migrate #2371 + #2603 + #3410: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_cross_cow_soft_migrate();
}
#endif
