// @category: unit
// @reason: Issue #2547 — stamp explicit COW-gen on closures; hard-reject
//          implicit cross-gen soft-migrate (wires #2240 CowGenMismatch).
//
//   AC1: Alloc under gen G → soft-eligible while live gen == G
//   AC2: Advance COW-gen → dual miss → hard CowGenMismatch
//   AC3: Single-workspace write still rejects foreign eval (#2178)
//   AC4: Soft disabled → hard on gen mismatch
//   AC5: Additive query keys; #2505 breakdown extended (reason=7)

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

extern "C" int64_t aura_alloc_closure(int64_t func_id);
extern "C" void aura_closure_set_name(int64_t closure_id, const char* name);
extern "C" int64_t aura_closure_call(int64_t closure_id, int64_t* args, int64_t argc);
extern "C" void aura_aot_bump_func_table_epoch(void);
extern "C" std::uint64_t aura_aot_func_table_epoch(void);
extern "C" std::uint64_t aura_get_closure_bridge_epoch(std::int64_t closure_id);
extern "C" std::uint64_t aura_get_closure_cow_gen(std::int64_t closure_id);
extern "C" void aura_set_live_workspace_cow_gen(std::uint64_t gen) noexcept;
extern "C" std::uint64_t aura_get_live_workspace_cow_gen(void) noexcept;
extern "C" void aura_test_reset_last_cross_workspace_reject_reason(void) noexcept;
extern "C" std::uint8_t aura_last_cross_workspace_reject_reason_v_read(void) noexcept;
extern "C" std::uint64_t aura_cross_workspace_hot_update_rejected_total_v_read(void) noexcept;
extern "C" bool aura_reload_aot_module_for_eval(void* eval_ptr, const char* path,
                                                std::uint64_t version);

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

static void clear_env() {
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE");
    unsetenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT");
}

static std::int64_t alloc_stamped(const char* name) {
    if (aura_aot_func_table_epoch() == 0)
        aura_aot_bump_func_table_epoch();
    const auto cid = aura_alloc_closure(1);
    if (cid >= 0)
        aura_closure_set_name(cid, name);
    return cid;
}

// ── AC1: same cow_gen remains soft-eligible ──
static void ac1_same_gen_soft() {
    std::println("\n--- #2547 AC1: same cow_gen → soft-eligible ---");
    clear_env();
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "1", 1);
    setenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "8", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_set_live_workspace_cow_gen(42);
    CHECK(aura_get_live_workspace_cow_gen() == 42, "AC1: live cow_gen set");

    const auto cid = alloc_stamped("ac1_same_gen_2547");
    CHECK(cid >= 0, "AC1: alloc");
    CHECK(aura_get_closure_cow_gen(cid) == 42, "AC1: closure stamped cow_gen=42");

    const auto soft0 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard0 = metrics.cross_cow_hard_reject_total.load();
    const auto cgm0 = metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load();

    // Epoch drift only (same cow_gen) → soft migrate.
    aura_aot_bump_func_table_epoch();
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);

    const auto soft1 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard1 = metrics.cross_cow_hard_reject_total.load();
    const auto cgm1 = metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load();
    CHECK(soft1 == soft0 + 1, "AC1: soft migrate +1 under same cow_gen");
    CHECK(hard1 == hard0, "AC1: no hard reject under same cow_gen");
    CHECK(cgm1 == cgm0, "AC1: no cow_gen_mismatch under same gen");
    // Soft restamp includes cow_gen (still 42).
    CHECK(aura_get_closure_cow_gen(cid) == 42, "AC1: cow_gen restamped same G");

    clear_env();
    aura_set_live_workspace_cow_gen(0);
    aura_set_aot_metrics(nullptr);
}

// ── AC2: advance cow_gen → hard CowGenMismatch ──
static void ac2_cross_gen_hard() {
    std::println("\n--- #2547 AC2: advance cow_gen → hard CowGenMismatch ---");
    clear_env();
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "1", 1);
    setenv("AURA_CROSS_COW_SOFT_MIGRATE_MAX_DRIFT", "64", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_test_reset_last_cross_workspace_reject_reason();
    aura_set_live_workspace_cow_gen(7);

    const auto cid = alloc_stamped("ac2_cross_gen_2547");
    CHECK(cid >= 0, "AC2: alloc");
    CHECK(aura_get_closure_cow_gen(cid) == 7, "AC2: stamped gen 7");

    // Advance true COW gen (bridge epoch also bumped so dual miss fires).
    aura_set_live_workspace_cow_gen(8);
    aura_aot_bump_func_table_epoch();

    const auto soft0 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard0 = metrics.cross_cow_hard_reject_total.load();
    const auto cgm0 = metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load();

    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);

    const auto soft1 = metrics.cross_cow_soft_migrate_total.load();
    const auto hard1 = metrics.cross_cow_hard_reject_total.load();
    const auto cgm1 = metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load();
    CHECK(soft1 == soft0, "AC2: no soft migrate across cow_gen");
    CHECK(hard1 == hard0 + 1, "AC2: hard reject +1");
    CHECK(cgm1 == cgm0 + 1, "AC2: cow_gen_mismatch total +1");
    CHECK(metrics.cross_cow_last_hard_reject_reason.load() == 7,
          "AC2: last reason = 7 CowGenMismatch");
    CHECK(aura_last_cross_workspace_reject_reason_v_read() ==
              static_cast<std::uint8_t>(CrossWorkspaceReject::CowGenMismatch),
          "AC2: #2240 last-cross-workspace-reject = CowGenMismatch");
    CHECK(aura_cross_cow_last_hard_reject_reason() == 7, "AC2: C ABI last reason 7");

    clear_env();
    aura_set_live_workspace_cow_gen(0);
    aura_test_reset_last_cross_workspace_reject_reason();
    aura_set_aot_metrics(nullptr);
}

// ── AC3: write path still rejects foreign eval ──
static void ac3_write_path_foreign() {
    std::println("\n--- #2547 AC3: write path foreign eval still reject ---");
    // Seed a workspace eval so foreign is truly foreign (#2178 pattern).
    CompilerService seed_cs;
    (void)aura_reload_aot_module_for_eval(&seed_cs.evaluator(), "/tmp/aura_2547_seed_missing.so",
                                          0);
    const auto rej0 = aura_cross_workspace_hot_update_rejected_total_v_read();
    void* foreign = reinterpret_cast<void*>(0xDEAD'BEEF'C0DE'2547ULL);
    const bool ok = aura_reload_aot_module_for_eval(foreign, "ac3_foreign_2547.so", 0);
    CHECK(!ok, "AC3: foreign eval reload false");
    CHECK(aura_cross_workspace_hot_update_rejected_total_v_read() == rej0 + 1,
          "AC3: cross_workspace reject +1");
    // Source-cite: write path fail-closed retained; #2547 does not open it.
    const auto bridge = read_file("src/compiler/aura_jit_bridge.h");
    CHECK(bridge.find("Does NOT open cross-workspace hot-update write") != std::string::npos ||
              bridge.find("fail-closed") != std::string::npos,
          "AC3: contract documents no write path");
    CHECK(bridge.find("CowGenMismatch") != std::string::npos, "AC3: CowGenMismatch in bridge.h");
    CHECK(read_file("src/compiler/aura_jit_runtime.cpp").find("NOT open cross-workspace") !=
                  std::string::npos ||
              read_file("src/compiler/aura_jit_runtime.cpp").find("#2178") != std::string::npos ||
              bridge.find("#2178") != std::string::npos,
          "AC3: #2178 lineage retained");
}

// ── AC4: soft disabled → hard on gen mismatch ──
static void ac4_soft_disabled() {
    std::println("\n--- #2547 AC4: soft disabled → hard on gen mismatch ---");
    clear_env();
    setenv("AURA_CROSS_COW_SOFT_MIGRATE", "0", 1);
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_set_live_workspace_cow_gen(3);

    const auto cid = alloc_stamped("ac4_soft_off_2547");
    CHECK(cid >= 0, "AC4: alloc");
    aura_set_live_workspace_cow_gen(4);
    aura_aot_bump_func_table_epoch();

    const auto soft0 = metrics.cross_cow_soft_migrate_total.load();
    const auto cgm0 = metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load();
    int64_t args[1] = {0};
    (void)aura_closure_call(cid, args, 0);
    CHECK(metrics.cross_cow_soft_migrate_total.load() == soft0, "AC4: no soft when disabled");
    // Either CowGenMismatch (primary) or Disabled depending on check order —
    // primary cow_gen check fires first → CowGenMismatch.
    CHECK(metrics.cross_cow_hard_reject_cow_gen_mismatch_total.load() == cgm0 + 1,
          "AC4: cow_gen_mismatch under soft-off");
    CHECK(aura_cross_cow_soft_migrate_enabled() == 0, "AC4: soft disabled");

    clear_env();
    aura_set_live_workspace_cow_gen(0);
    aura_set_aot_metrics(nullptr);
}

// ── AC5: schema + source ──
static void ac5_schema_source() {
    std::println("\n--- #2547 AC5: additive schema + source-cite ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2547") == 2547, "AC5: schema-2547");
    CHECK(href(cs, "issue-2547") == 2547, "AC5: issue-2547");
    CHECK(href(cs, "cross-cow-closure-cow-gen-stamp-wired") == 1, "AC5: stamp wired");
    CHECK(href(cs, "cross-cow-gen-mismatch-hard-wired") == 1, "AC5: hard wired");
    CHECK(href(cs, "cross-cow-hard-reject-cow-gen-mismatch-total") >= 0, "AC5: counter key");
    CHECK(href(cs, "schema-2505") == 2505, "AC5: schema-2505 retained");
    CHECK(href(cs, "cross-cow-hard-reject-far-behind-total") >= 0, "AC5: #2505 keys retained");
    CHECK(href(cs, "cross-cow-no-write-path-wired") == 1, "AC5: no write path sentinel");

    const auto rt = read_file("src/compiler/aura_jit_runtime.cpp");
    const auto br = read_file("src/compiler/aura_jit_bridge.h");
    const auto obs = read_file("src/compiler/observability_metrics.h");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto cmake = read_file("CMakeLists.txt");

    CHECK(rt.find("g_closure_cow_gens") != std::string::npos, "AC5: cow_gens vector");
    CHECK(rt.find("CowGenMismatch") != std::string::npos, "AC5: CowGenMismatch in runtime");
    CHECK(rt.find("closure_cow_gen_mismatch_") != std::string::npos, "AC5: mismatch helper");
    CHECK(rt.find("stamp_closure_provenance_locked") != std::string::npos &&
              rt.find("cow_gen") != std::string::npos,
          "AC5: stamp includes cow_gen");
    CHECK(br.find("#2547") != std::string::npos, "AC5: bridge.h cites #2547");
    CHECK(br.find("7=CowGenMismatch") != std::string::npos ||
              br.find("CowGenMismatch (#2547") != std::string::npos,
          "AC5: reason enum documents 7");
    CHECK(obs.find("cross_cow_hard_reject_cow_gen_mismatch_total") != std::string::npos,
          "AC5: metrics field");
    CHECK(q.find("schema-2547") != std::string::npos, "AC5: query schema");
    CHECK(cmake.find("test_closure_cow_gen_stamp_2547") != std::string::npos, "AC5: cmake");
}

} // namespace

int main() {
    std::println("=== Issue #2547: closure cow_gen stamp + hard mismatch ===");
    ac1_same_gen_soft();
    ac2_cross_gen_hard();
    ac3_write_path_foreign();
    ac4_soft_disabled();
    ac5_schema_source();
    clear_env();
    aura_set_live_workspace_cow_gen(0);
    if (g_failed)
        return 1;
    std::println("\n=== #2547: {} passed, {} failed ===", g_passed, g_failed);
    return 0;
}
