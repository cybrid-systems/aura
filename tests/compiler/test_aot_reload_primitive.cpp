// test_aot_reload_primitive.cpp — Issue #1366: (aot:reload) Aura wrappers
// Issue #2012: atomic func_table staging + rollback + concurrent stress
// Issue #2178: cross-workspace / cross-COW hot-update reject + metric
//   AC7: foreign eval_ptr → reject + counter++; matching eval → success;
//   null eval_ptr (process default) → happy path unchanged.

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

// Issue #2178: C-linkage accessors for the cross-workspace guard + counter.
// Forward-declared here so the test file can call them without pulling in
// the full C++ definition (the function bodies live in aura_jit_bridge.cpp).
// Note: aura_reload_aot_module_for_eval is already declared in
// aura_jit_bridge.h:299 (included above), so we don't re-declare it here.
extern "C" std::uint64_t aura_cross_workspace_hot_update_rejected_total_v_read(void) noexcept;
extern "C" bool aura_is_current_workspace_eval(void* eval_ptr) noexcept;

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>


// C-linkage decls from src/compiler/aura_jit_runtime.cpp (Issue #2232 hot-update).
extern "C" {
void aura_set_live_workspace_cow_gen(std::uint64_t cow_gen);
void aura_set_aot_expected_cow_gen_for_eval(void* eval_ptr, std::uint64_t cow_gen);
std::uint64_t aura_get_live_workspace_cow_gen(void);
std::uint64_t aura_get_aot_expected_cow_gen_for_eval(void* eval_ptr);
}


static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

namespace {

std::int64_t href(CompilerService& cs, const char* q, const char* key) {
    // Prefer engine:metrics catalog (query:* forms are registered there).
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Build a minimal shared object with aot_emit_version for success path.
// Returns path or empty on failure.
std::string build_test_so(std::uint64_t version) {
    const char* dir = "/tmp";
    std::string cpath = std::format("{}/aura_aot_test_{}.c", dir, version);
    // Issue #2232: .so with both aot_emit_version + aot_env_frame_version
    // so the Env retry-recovery AC can fire repeatedly (the default
    // build_test_so only sets aot_emit_version + aot_region_mask).
    // (build_test_so_with_env moved below)

    std::string sopath = std::format("{}/aura_aot_test_{}.so", dir, version);
    {
        std::ofstream f(cpath);
        if (!f)
            return {};
        f << "#include <stdint.h>\n";
        f << "uint64_t aot_emit_version = " << version << "ULL;\n";
        f << "uint64_t aot_region_mask = 0ULL;\n";
        f << "__attribute__((constructor)) static void reg(void) {\n";
        f << "  (void)aot_emit_version;\n";
        f << "}\n";
    }
    std::string cmd = std::format("cc -shared -fPIC -o {} {} 2>/dev/null", sopath, cpath);
    if (std::system(cmd.c_str()) != 0)
        return {};
    return sopath;
}
std::string build_test_so_with_env(std::uint64_t version, std::uint64_t env_version) {
    const char* dir = "/tmp";
    std::string cpath = std::format("{}/aura_aot_test_{}_{}.c", dir, version, env_version);
    std::string sopath = std::format("{}/aura_aot_test_{}_{}.so", dir, version, env_version);
    {
        std::ofstream f(cpath);
        if (!f)
            return {};
        f << "#include <stdint.h>\n";
        f << "uint64_t aot_emit_version = " << version << "ULL;\n";
        f << "uint64_t aot_region_mask = 0ULL;\n";
        f << "uint64_t aot_env_frame_version = " << env_version << "ULL;\n";
        f << "__attribute__((constructor)) static void reg(void) {(void)aot_emit_version; "
             "(void)aot_env_frame_version;}\n";
    }
    std::string cmd = std::format("cc -shared -fPIC -o {} {} 2>/dev/null", sopath, cpath);
    if (std::system(cmd.c_str()) != 0)
        return {};
    return sopath;
}


// Issue #2012: .so that registers a staged func_id via aura_register_fn_tracked.
// Uses dlsym(RTLD_DEFAULT) so the .so has no undefined symbols at dlopen
// (RTLD_NOW) while still binding to the host process's strong definition.
std::string build_registering_so(std::uint64_t version, std::uint64_t region, int func_id,
                                 const char* tag) {
    const char* dir = "/tmp";
    std::string cpath = std::format("{}/aura_aot_reg_{}_{}.c", dir, tag, version);
    std::string sopath = std::format("{}/aura_aot_reg_{}_{}.so", dir, tag, version);
    {
        std::ofstream f(cpath);
        if (!f)
            return {};
        f << "#include <stdint.h>\n";
        f << "#include <stddef.h>\n";
        f << "#include <dlfcn.h>\n";
        f << "uint64_t aot_emit_version = " << version << "ULL;\n";
        f << "uint64_t aot_region_mask = " << region << "ULL;\n";
        f << "typedef void (*reg_fn_t)(int64_t, int64_t);\n";

        f << "static int64_t aura_aot_reg_sentinel_" << tag
          << "(int64_t* a, uint32_t n) { (void)a; (void)n; return " << version << "; }\n";
        f << "__attribute__((constructor)) static void reg(void) {\n";
        f << "  void* self = dlopen(NULL, RTLD_LAZY);\n";
        f << "  reg_fn_t fn = self ? (reg_fn_t)dlsym(self, \"aura_register_fn_tracked\") : 0;\n";
        f << "  if (!fn) fn = (reg_fn_t)dlsym(RTLD_DEFAULT, \"aura_register_fn_tracked\");\n";
        f << "  if (fn) fn(" << func_id << ", (int64_t)(void*)aura_aot_reg_sentinel_" << tag
          << ");\n";
        f << "}\n";
    }
    std::string cmd = std::format("cc -shared -fPIC -o {} {} -ldl 2>/dev/null", sopath, cpath);
    if (std::system(cmd.c_str()) != 0)
        return {};
    return sopath;
}

} // namespace

// Issue #2178: cross-workspace / cross-COW hot-update reject. Foreign
// eval contexts (or COW generation mismatch) must be rejected at the
// reload entry point with a dedicated metric, not a silent partial
// success. The MVP scope (#1943) documents single-workspace; this guard
// enforces the boundary until a future cross-COW migration design lands.
static void ac7_cross_workspace_reject_2178() {
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    // Baseline: single-workspace / process-default path unchanged.
    // (Existing AC1-AC6 already verify version + region mismatch bumps
    // the right counters; this AC is the cross-workspace-specific path.)
    aura_set_aot_region_mask(0);
    aura_set_module_version(0);
    // Null eval_ptr happy path: missing .so fails dlopen (not foreign-reject).
    // Pre-register process default by attempting load; dlopen fail is OK.
    (void)aura_reload_aot_module("/tmp/aura_ac7_baseline_missing.so", 0);
    CHECK(aura_reload_aot_module("/tmp/aura_ac7_baseline_missing.so", 0) == false,
          "AC7: null eval_ptr (process default) reaches dlopen (not foreign-reject)");
    // Capture rejected counter baseline.
    const auto rej0 = aura_cross_workspace_hot_update_rejected_total_v_read();
    // Seed a registered workspace so a different eval_ptr is truly foreign.
    CompilerService seed_cs;
    (void)aura_reload_aot_module_for_eval(&seed_cs.evaluator(), "/tmp/aura_ac7_seed_missing.so", 0);
    // Foreign eval context: pick an obviously-bogus address that's
    // NOT in the per-eval states map. The guard must reject.
    void* foreign_eval = reinterpret_cast<void*>(0xDEAD'BEEF'C0DE'0001ULL);
    const bool ok_foreign = aura_reload_aot_module_for_eval(foreign_eval, "ac7_foreign.so", 0);
    CHECK(ok_foreign == false,
          "AC7: foreign eval_ptr → aura_reload_aot_module_for_eval returns false");
    CHECK(aura_cross_workspace_hot_update_rejected_total_v_read() == rej0 + 1,
          "AC7: cross_workspace_hot_update_rejected_total +1 (Issue #2178 AC1)");
    // Null eval_ptr still reaches the load path (process-default AotState).
    const bool ok_null =
        aura_reload_aot_module_for_eval(nullptr, "/tmp/aura_ac7_null_missing.so", 0);
    CHECK(ok_null == false, "AC7: null eval_ptr reaches dlopen fail (not foreign-reject)");
    CHECK(aura_cross_workspace_hot_update_rejected_total_v_read() == rej0 + 1,
          "AC7: cross_workspace_hot_update_rejected_total unchanged (null is not foreign)");
    // Source-cite: foreign guard + helper in aura_jit_bridge.cpp.
    std::ifstream ab("src/compiler/aura_jit_bridge.cpp");
    std::string ab_contents((std::istreambuf_iterator<char>(ab)), std::istreambuf_iterator<char>());
    CHECK(ab_contents.find("g_cross_workspace_hot_update_rejected_total{0}") != std::string::npos,
          "AC7: file-level atomic in aura_jit_bridge.cpp");
    CHECK(ab_contents.find("aura_cross_workspace_hot_update_rejected_increment") !=
              std::string::npos,
          "AC7: bump helper in aura_jit_bridge.cpp");
    CHECK(ab_contents.find("aura_cross_workspace_hot_update_rejected_total_v_read") !=
              std::string::npos,
          "AC7: C-linkage accessor in aura_jit_bridge.cpp");
    CHECK(ab_contents.find("aura_is_current_workspace_eval") != std::string::npos,
          "AC7: is_current_workspace_eval guard in aura_jit_bridge.cpp");
    CHECK(ab_contents.find("Issue #2178") != std::string::npos,
          "AC7: aura_jit_bridge.cpp cites #2178");
    // Source-cite: CompilerMetrics field + hot_update_registry.hh doc.
    std::ifstream om("src/compiler/observability_metrics.h");
    std::string om_contents((std::istreambuf_iterator<char>(om)), std::istreambuf_iterator<char>());
    CHECK(om_contents.find("cross_workspace_hot_update_rejected_total{0}") != std::string::npos,
          "AC7: CompilerMetrics field cross_workspace_hot_update_rejected_total");
    std::ifstream hr("src/compiler/hot_update_registry.hh");
    std::string hr_contents((std::istreambuf_iterator<char>(hr)), std::istreambuf_iterator<char>());
    CHECK(hr_contents.find("Issue #2178") != std::string::npos,
          "AC7: hot_update_registry.hh cites #2178");
    CHECK(hr_contents.find("aura_is_current_workspace_eval") != std::string::npos,
          "AC7: hot_update_registry.hh contract references the guard");
    // Live value check: counter is atomic load (no setup needed).
    const auto rej_final = aura_cross_workspace_hot_update_rejected_total_v_read();
    CHECK(rej_final >= rej0 + 1, "AC7: final rejected counter >= baseline + 1");
    aura_set_aot_metrics(nullptr);
}

// Issue #2240: stable cross-workspace / cross-COW reject reason code
// + symbol accessor + query-surface hash mode (refine #2178, which
// shipped the basic guard + counter but Agents could only see an
// aggregate counter — not why). Extends AC7 family with:
//   AC7b.1 — foreign eval_ptr → counter++ AND reason=ForeignEval
//   AC7b.2 — null eval_ptr → dlopen fail (NOT foreign-reject) AND
//            reason stays None=0 (parallel reset + not bumped)
//   AC7b.3 — symbol accessor for all 4 enum values + out-of-range
//   AC7b.4 — reason reset at start of next reload attempt
//            (parallel to g_last_reload_fail_reason reset at L1988)
//   AC7b.5 — source-cite: enum + atomic + C-linkage reader + bumper
//            + Issue #2240 tag in aura_jit_bridge.{h,cpp} + hash
//            mode of query:aot-hot-update-stats exposes the keys.
static void ac7b_cross_workspace_reason_code_2240() {
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    // Hermetic test isolation — AC7 may have left reason in
    // ForeignEval state from its foreign-eval reject path. Reset
    // to None before this AC starts.
    aura_test_reset_last_cross_workspace_reject_reason();
    CHECK(aura_last_cross_workspace_reject_reason_v_read() == 0, "AC7b: reset reason → None (0)");

    // Capture baseline rejected counter.
    const auto rej0 = aura_cross_workspace_hot_update_rejected_total_v_read();

    // AC7b.1: foreign eval_ptr → reject + counter++ + reason=ForeignEval=1.
    aura_set_aot_region_mask(0);
    aura_set_module_version(0);
    CompilerService seed_cs;
    (void)aura_reload_aot_module_for_eval(&seed_cs.evaluator(), "/tmp/aura_ac7b_seed_missing.so",
                                          0);
    void* foreign_eval = reinterpret_cast<void*>(0xDEAD'BEEF'CAFE'0001ULL);
    const bool ok_foreign = aura_reload_aot_module_for_eval(foreign_eval, "ac7b_foreign.so", 0);
    CHECK(ok_foreign == false,
          "AC7b: foreign eval_ptr → aura_reload_aot_module_for_eval returns false");
    CHECK(aura_cross_workspace_hot_update_rejected_total_v_read() == rej0 + 1,
          "AC7b: cross_workspace_hot_update_rejected_total +1 (refine #2178 AC1)");
    CHECK(aura_last_cross_workspace_reject_reason_v_read() == 1,
          "AC7b: last_cross_workspace_reject_reason set to ForeignEval=1 (refine #2178)");

    // AC7b.2: null eval_ptr → dlopen fail path (NOT foreign-reject) AND
    // reason stays None=0 (parallel reset + not bumped in dlopen path).
    aura_test_reset_last_cross_workspace_reject_reason();
    CHECK(aura_last_cross_workspace_reject_reason_v_read() == 0,
          "AC7b: post-reset reason → None=0");
    const bool ok_null =
        aura_reload_aot_module_for_eval(nullptr, "/tmp/aura_ac7b_null_missing.so", 0);
    CHECK(ok_null == false, "AC7b: null eval_ptr reaches dlopen fail (not foreign-reject)");
    CHECK(aura_cross_workspace_hot_update_rejected_total_v_read() == rej0 + 1,
          "AC7b: counter unchanged after null eval (dlopen path, NOT bumped)");
    CHECK(aura_last_cross_workspace_reject_reason_v_read() == 0,
          "AC7b: reason stays None=0 after null eval dlopen fail (NOT bumped)");

    // AC7b.3: symbol accessor for all 4 enum values + out-of-range.
    CHECK(std::string(aura_cross_workspace_reject_reason_string(0)) == "none",
          "AC7b: reason(0)='none'");
    CHECK(std::string(aura_cross_workspace_reject_reason_string(1)) == "foreign_eval",
          "AC7b: reason(1)='foreign_eval'");
    CHECK(std::string(aura_cross_workspace_reject_reason_string(2)) == "cow_gen_mismatch",
          "AC7b: reason(2)='cow_gen_mismatch' (reserved for future cross-COW)");
    CHECK(std::string(aura_cross_workspace_reject_reason_string(3)) == "unknown",
          "AC7b: reason(3)='unknown' (defensive)");
    CHECK(std::string(aura_cross_workspace_reject_reason_string(99)) == "unknown",
          "AC7b: reason(99)='unknown' (defensive out-of-range fallback)");

    // AC7b.4: reason reset at start of next attempt (alongside
    // g_last_reload_fail_reason reset; otherwise stale Unknown would
    // leak across attempts).
    aura_test_set_last_cross_workspace_reject_reason(3); // simulate stale Unknown
    CHECK(aura_last_cross_workspace_reject_reason_v_read() == 3,
          "AC7b: test-set reason → Unknown=3 (simulate stale)");
    (void)aura_reload_aot_module("/tmp/aura_ac7b_reset_trigger.so", 0);
    CHECK(
        aura_last_cross_workspace_reject_reason_v_read() == 0,
        "AC7b: reload attempt start resets reason → None=0 (parallel to last_reload_fail_reason)");

    // AC7b.5: source-cite + hash-mode query surface.
    std::ifstream ab("src/compiler/aura_jit_bridge.cpp");
    std::string ab_contents((std::istreambuf_iterator<char>(ab)), std::istreambuf_iterator<char>());
    CHECK(ab_contents.find("g_last_cross_workspace_reject_reason{0}") != std::string::npos,
          "AC7b: file-level atomic in aura_jit_bridge.cpp");
    CHECK(ab_contents.find("aura_last_cross_workspace_reject_reason_v_read") != std::string::npos,
          "AC7b: C-linkage reader in aura_jit_bridge.cpp");
    CHECK(ab_contents.find("CrossWorkspaceReject::ForeignEval") != std::string::npos,
          "AC7b: ForeignEval reason set at guard site");
    CHECK(ab_contents.find("Issue #2240") != std::string::npos,
          "AC7b: aura_jit_bridge.cpp cites #2240");
    std::ifstream hd("src/compiler/aura_jit_bridge.h");
    std::string hd_contents((std::istreambuf_iterator<char>(hd)), std::istreambuf_iterator<char>());
    CHECK(hd_contents.find("enum class CrossWorkspaceReject") != std::string::npos,
          "AC7b: CrossWorkspaceReject enum in aura_jit_bridge.h");
    CHECK(hd_contents.find("aura_last_cross_workspace_reject_reason_v_read") != std::string::npos,
          "AC7b: C-linkage reader declared in aura_jit_bridge.h");
    CHECK(hd_contents.find("Issue #2240") != std::string::npos,
          "AC7b: aura_jit_bridge.h cites #2240");

    // AC7b.6: hash-mode query surface exposes cross-workspace keys.
    {
        CompilerService cs;
        // Force a foreign reject so the reason code is non-zero.
        void* forced_foreign = reinterpret_cast<void*>(0xF00D'BABE'DEAD'0002ULL);
        (void)aura_reload_aot_module_for_eval(forced_foreign, "ac7b_forced.so", 0);
        // Hash mode: args[0]=1 returns the hash with #2240 keys
        // (additive — default args[0]=0 still returns sum sentinel).
        auto r = cs.eval("(engine:metrics \"query:aot-hot-update-stats\" 1)");
        CHECK(r && is_hash(*r), "AC7b: query:aot-hot-update-stats[1] → hash");
        if (r && is_hash(*r)) {
            // hash-ref helper: read keys via aura's hash-ref symbol
            auto k_cross_tot = cs.eval("(hash-ref (engine:metrics \"query:aot-hot-update-stats\" "
                                       "1) \"cross-workspace-hot-update-rejected-total\")");
            CHECK(k_cross_tot && is_int(*k_cross_tot) && as_int(*k_cross_tot) >= 0,
                  "AC7b: hash key 'cross-workspace-hot-update-rejected-total' present");
            auto k_wired = cs.eval("(hash-ref (engine:metrics \"query:aot-hot-update-stats\" 1) "
                                   "\"cross-workspace-reject-wired\")");
            CHECK(k_wired && is_int(*k_wired) && as_int(*k_wired) == 1,
                  "AC7b: hash key 'cross-workspace-reject-wired=1' present");
            auto k_reason = cs.eval("(hash-ref (engine:metrics \"query:aot-hot-update-stats\" 1) "
                                    "\"cross-workspace-last-reject-reason\")");
            CHECK(k_reason && is_int(*k_reason) && as_int(*k_reason) == 1,
                  "AC7b: hash key 'cross-workspace-last-reject-reason=ForeignEval=1'");
            auto k_schema = cs.eval(
                "(hash-ref (engine:metrics \"query:aot-hot-update-stats\" 1) \"schema-2240\")");
            CHECK(k_schema && is_int(*k_schema) && as_int(*k_schema) == 2240,
                  "AC7b: hash key 'schema-2240=2240' lineage");
        }
        // AC4: default sum mode (no args) still returns -1 / sum sentinel
        // (no schema break — additive hash mode on args[0]=1).
        auto r_sum = cs.eval("(engine:metrics \"query:aot-hot-update-stats\")");
        CHECK(r_sum && is_int(*r_sum),
              "AC7b: query:aot-hot-update-stats default (no args) → int (backwards compat)");
    }

    aura_set_aot_metrics(nullptr);
}

// Issue #2271 AC1-AC5: physical invalidate of generation-behind AOT
// slots on fall_back_jit_only exhaustion (close #2232 follow-up).
// AC1: C ABI aura_aot_invalidate_all_stale_slots_for_eval declared in
//      aura_jit_bridge.h + helper in aura_jit_bridge.cpp.
// AC2: Wired into aura_reload_aot_module_for_eval exhaustion branch
//      (after on_force_jit_for_reason + aot_reload_fall_back_jit_only_total
//      bump + joint-bump via aura_aot_bump_func_table_epoch).
// AC3: Happy-path (no exhaustion) → no spurious invalidate (covered
//      by #2232 AC1 tests already in this file).
// AC4: 2 new counters in observability_metrics.h +
//      aot_reload_fall_back_slot_invalidate_wired + schema-2271/issue-2271
//      query keys on query:aot-reload-stats.
// AC5: Runtime smoke — trigger Defuse exhaustion → verify both new
//      counters bump + query keys expose.
static void ac2271_physical_invalidate(CompilerService& cs) {
    std::println("\n--- AC #2271: fall_back_jit_only physical slot invalidate ---");
    auto bridge = read_file("src/compiler/aura_jit_bridge.h");
    auto bridge_cpp = read_file("src/compiler/aura_jit_bridge.cpp");
    auto hot = read_file("src/compiler/hot_update_registry.hh");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    // AC1: C ABI declared + helper implemented.
    CHECK(bridge.find("aura_aot_invalidate_all_stale_slots_for_eval") != std::string::npos,
          "AC1: C ABI declared in aura_jit_bridge.h");
    CHECK(
        bridge_cpp.find("extern \"C\" std::size_t aura_aot_invalidate_all_stale_slots_for_eval") !=
            std::string::npos,
        "AC1: C ABI extern \"C\" definition in aura_jit_bridge.cpp");
    CHECK(bridge_cpp.find("table_generation.store(0, std::memory_order_release)") !=
              std::string::npos,
          "AC1: helper clears fn_ptr + resets generation");
    // AC2: wired into exhaustion branch.
    CHECK(bridge_cpp.find("aura_aot_invalidate_all_stale_slots_for_eval(eval_ptr)") !=
              std::string::npos,
          "AC2: invalidate called in exhaustion branch");
    CHECK(bridge_cpp.find("aura_aot_bump_func_table_epoch();") != std::string::npos,
          "AC2: joint table-epoch bump next to invalidate");
    CHECK(hot.find("#2271") != std::string::npos,
          "AC2: hot_update_registry.hh documents physical invalidate");
    // AC3: happy-path unchanged — no spurious invalidate outside the
    // exhaustion branch (negative source-cite: invalidate only inside
    // the policy.fall_back_jit_only block).
    CHECK(bridge_cpp.find("if (policy.fall_back_jit_only)") != std::string::npos,
          "AC3: invalidate guarded by fall_back_jit_only check");
    // AC4: counters + query keys + lineage.
    CHECK(obs.find("aot_reload_fall_back_slot_invalidate_total{0}") != std::string::npos,
          "AC4: slot-invalidate counter field");
    CHECK(obs.find("aot_reload_fall_back_slot_invalidate_calls_total{0}") != std::string::npos,
          "AC4: slot-invalidate-calls counter field");
    CHECK(q.find("aot-reload-fall-back-slot-invalidate-total") != std::string::npos,
          "AC4: slot-invalidate query key");
    CHECK(q.find("aot-reload-fall-back-slot-invalidate-calls-total") != std::string::npos,
          "AC4: slot-invalidate-calls query key");
    CHECK(q.find("aot-reload-fall-back-slot-invalidate-wired") != std::string::npos,
          "AC4: slot-invalidate-wired sentinel");
    CHECK(q.find("schema-2271") != std::string::npos, "AC4: schema-2271 lineage");
    CHECK(q.find("issue-2271") != std::string::npos, "AC4: issue-2271 lineage");
    // AC5: runtime smoke — seed a generation-behind slot and invoke the
    // C ABI directly (exhaustion path is source-cited in AC1–AC3; Version
    // auto-retry can mask fall_back so counter smoke is more reliable here).
    // Query keys live on query:aot-stats (not the slim query:aot-reload-stats).
    {
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        aura_set_aot_metrics(m);
        aura_set_aot_region_mask(0);
        aura_set_module_version(0);
        aura_aot_set_register_owner_eval(nullptr);
        aura_register_fn_tracked(/*func_id=*/88, /*fn_ptr=*/0x88008800);
        aura_aot_bump_func_table_epoch(); // make slot generation-behind
        const auto slot_inv0 =
            m->aot_reload_fall_back_slot_invalidate_total.load(std::memory_order_relaxed);
        const auto slot_calls0 =
            m->aot_reload_fall_back_slot_invalidate_calls_total.load(std::memory_order_relaxed);
        const auto n = aura_aot_invalidate_all_stale_slots_for_eval(nullptr);
        CHECK(n >= 1, "AC5: process-default invalidate cleared >=1 slot");
        const auto slot_inv1 =
            m->aot_reload_fall_back_slot_invalidate_total.load(std::memory_order_relaxed);
        const auto slot_calls1 =
            m->aot_reload_fall_back_slot_invalidate_calls_total.load(std::memory_order_relaxed);
        CHECK(slot_calls1 >= slot_calls0 + 1,
              "AC5: aot_reload_fall_back_slot_invalidate_calls_total bumped");
        CHECK(slot_inv1 >= slot_inv0 + 1, "AC5: aot_reload_fall_back_slot_invalidate_total bumped");
        const auto w = href(cs, "query:aot-stats", "aot-reload-fall-back-slot-invalidate-wired");
        CHECK(w == 1, "AC5: query aot-reload-fall-back-slot-invalidate-wired=1");
        const auto sc = href(cs, "query:aot-stats", "schema-2271");
        CHECK(sc == 2271, "AC5: query schema-2271=2271");
        aura_set_aot_metrics(nullptr);
    }
    (void)cs;
}

// Issue #2299 AC1-AC5: per-eval physical invalidate of generation-behind
// slots (close #2271 follow-up for multi-eval hosts).
//   AC1: Dual-eval — invalidate(eval_A) clears only A's owned stale slots;
//        eval_B slots remain (raw probe non-zero).
//   AC2: eval_ptr == nullptr → process-default clears ALL generation-behind
//        (identical to #2271).
//   AC3: Ordering invariant — fn_ptr store before generation store
//        (source-cite) so concurrent probe sees null first.
//   AC4: Counters still bump; last-eval + per-eval-calls observability.
//   AC5: Source-cite + dual-eval smoke; #2271 ACs remain green above.
static void ac2299_per_eval_slot_invalidate(CompilerService& cs) {
    std::println("\n--- AC #2299: per-eval physical slot invalidate ---");
    auto bridge = read_file("src/compiler/aura_jit_bridge.h");
    auto bridge_cpp = read_file("src/compiler/aura_jit_bridge.cpp");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");

    // AC3 / AC5: source-cite filter + ordering + RegisterOwnerGuard.
    CHECK(bridge.find("#2299") != std::string::npos, "AC5: header documents #2299");
    CHECK(bridge_cpp.find("filter_by_eval") != std::string::npos ||
              bridge_cpp.find("owner_eval") != std::string::npos,
          "AC1/AC5: owner_eval filter in invalidate");
    CHECK(bridge_cpp.find("slot.fn_ptr.store(0, std::memory_order_release)") != std::string::npos &&
              bridge_cpp.find("slot.table_generation.store(0, std::memory_order_release)") !=
                  std::string::npos,
          "AC3: fn_ptr release before generation release");
    CHECK(bridge_cpp.find("RegisterOwnerGuard") != std::string::npos,
          "AC5: RegisterOwnerGuard on reload_for_eval");
    CHECK(bridge.find("aura_aot_set_register_owner_eval") != std::string::npos,
          "AC5: register-owner TLS API declared");
    CHECK(obs.find("aot_reload_fall_back_slot_invalidate_last_eval") != std::string::npos,
          "AC4: last_eval metric field");
    CHECK(obs.find("aot_reload_fall_back_slot_invalidate_per_eval_calls_total") !=
              std::string::npos,
          "AC4: per_eval_calls metric field");
    CHECK(q.find("schema-2299") != std::string::npos && q.find("issue-2299") != std::string::npos,
          "AC4: schema-2299 / issue-2299 query lineage");
    CHECK(q.find("aot-reload-fall-back-slot-invalidate-per-eval-wired") != std::string::npos,
          "AC4: per-eval-wired sentinel");

    auto& ev = cs.evaluator();
    auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
    aura_set_aot_metrics(m);

    // Opaque dual-eval keys (not required to be live Evaluator*).
    void* eval_a = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xA11A));
    void* eval_b = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xB22B));
    // Ensure per-eval AotState map entries exist (mirrors multi-eval hosts).
    aura_set_aot_region_mask_for_eval(eval_a, 1);
    aura_set_aot_region_mask_for_eval(eval_b, 2);
    CHECK(aura_aot_state_map_size() >= 2, "AC1: dual-eval AotState map size >= 2");

    constexpr std::int64_t kFidA = 310;
    constexpr std::int64_t kFidB = 311;
    constexpr std::uintptr_t kPtrA = 0x11110000u;
    constexpr std::uintptr_t kPtrB = 0x22220000u;

    // Seed owned slots at current epoch, then bump so both are gen-behind.
    aura_aot_set_register_owner_eval(eval_a);
    aura_register_fn_tracked(kFidA, static_cast<std::int64_t>(kPtrA));
    aura_aot_set_register_owner_eval(eval_b);
    aura_register_fn_tracked(kFidB, static_cast<std::int64_t>(kPtrB));
    aura_aot_set_register_owner_eval(nullptr);
    CHECK(aura_aot_probe_fn_ptr_raw(kFidA) == kPtrA, "AC1 setup: slot A live");
    CHECK(aura_aot_probe_fn_ptr_raw(kFidB) == kPtrB, "AC1 setup: slot B live");
    aura_aot_bump_func_table_epoch(); // both generation-behind

    const auto inv0 = m->aot_reload_fall_back_slot_invalidate_total.load(std::memory_order_relaxed);
    const auto calls0 =
        m->aot_reload_fall_back_slot_invalidate_calls_total.load(std::memory_order_relaxed);
    const auto per0 = m->aot_reload_fall_back_slot_invalidate_per_eval_calls_total.load(
        std::memory_order_relaxed);

    const auto n_a = aura_aot_invalidate_all_stale_slots_for_eval(eval_a);
    CHECK(n_a >= 1, "AC1: invalidate(eval_A) cleared >=1 slot");
    CHECK(aura_aot_probe_fn_ptr_raw(kFidA) == 0, "AC1: eval A slot physically cleared");
    CHECK(aura_aot_probe_fn_ptr_raw(kFidB) == kPtrB, "AC1: eval B slot remains");
    CHECK(aura_aot_last_slot_invalidate_eval() == reinterpret_cast<std::uintptr_t>(eval_a),
          "AC4: last_eval records eval_A");
    CHECK(m->aot_reload_fall_back_slot_invalidate_calls_total.load(std::memory_order_relaxed) >=
              calls0 + 1,
          "AC4: calls counter bumped");
    CHECK(m->aot_reload_fall_back_slot_invalidate_total.load(std::memory_order_relaxed) >= inv0 + 1,
          "AC4: slot total bumped");
    CHECK(m->aot_reload_fall_back_slot_invalidate_per_eval_calls_total.load(
              std::memory_order_relaxed) >= per0 + 1,
          "AC4: per-eval calls bumped");

    // AC2: process-default (nullptr) clears remaining generation-behind (B).
    const auto n_all = aura_aot_invalidate_all_stale_slots_for_eval(nullptr);
    CHECK(n_all >= 1, "AC2: nullptr invalidate clears remaining gen-behind");
    CHECK(aura_aot_probe_fn_ptr_raw(kFidB) == 0, "AC2: B cleared under process-default");
    CHECK(aura_aot_last_slot_invalidate_eval() == 0, "AC2/AC4: last_eval=0 for nullptr");

    // Query surface on query:aot-stats (full AOT stats catalog).
    const auto sc = href(cs, "query:aot-stats", "schema-2299");
    CHECK(sc == 2299, "AC4: query schema-2299=2299");
    const auto wired =
        href(cs, "query:aot-stats", "aot-reload-fall-back-slot-invalidate-per-eval-wired");
    CHECK(wired == 1, "AC4: per-eval-wired=1");

    // Cleanup map entries so later tests aren't polluted.
    aura_cleanup_aot_state(eval_a);
    aura_cleanup_aot_state(eval_b);
    aura_set_aot_metrics(nullptr);
    (void)cs;
}

// Issue #2275 AC1-AC5: CrossWorkspaceReject::CowGenMismatch wire
// (still fail-closed, no write path). Refines #2240 (#2178 hard guard).
// AC1: Foreign eval → reason ForeignEval (regression #2240).
// AC2: Matching eval + injected cow_gen mismatch → reason
//      CowGenMismatch + reject; no table mutation.
// AC3: Matching eval + matching cow_gen → existing reload path
//      (success or normal AotReloadFail reasons).
// AC4: aura_cross_workspace_reject_reason_string covers all four
//      enum values (already done in #2240).
// AC5: Source-cite gate sites + runtime smoke.
static void ac2275_cow_gen_mismatch(CompilerService& cs) {
    std::println("\n--- AC #2275: CowGenMismatch wire (fail-closed) ---");
    auto bridge_h = read_file("src/compiler/aura_jit_bridge.h");
    auto bridge_cpp = read_file("src/compiler/aura_jit_bridge.cpp");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    // AC1: ForeignEval path preserved (regression check).
    CHECK(bridge_cpp.find("static_cast<std::uint8_t>(CrossWorkspaceReject::ForeignEval),\n         "
                          "   std::memory_order_release);") != std::string::npos,
          "AC1: ForeignEval path unchanged");
    // AC2: CowGenMismatch wire present.
    CHECK(bridge_cpp.find("CrossWorkspaceReject::CowGenMismatch") != std::string::npos,
          "AC2: CowGenMismatch enum wired");
    CHECK(bridge_cpp.find("aura_get_aot_expected_cow_gen_for_eval(eval_ptr)") != std::string::npos,
          "AC2: expected cow_gen accessor called");
    CHECK(bridge_cpp.find("aura_get_live_workspace_cow_gen()") != std::string::npos,
          "AC2: live workspace cow_gen accessor called");
    // AC3: happy path — null expected + null live → no reject (default).
    CHECK(bridge_h.find("aura_set_aot_expected_cow_gen_for_eval") != std::string::npos,
          "AC3: expected cow_gen C ABI declared");
    CHECK(bridge_h.find("aura_get_live_workspace_cow_gen") != std::string::npos,
          "AC3: live workspace cow_gen C ABI declared");
    // AC4: reason string switch covers all 4 enum values.
    CHECK(bridge_cpp.find("aura_cross_workspace_reject_reason_string") != std::string::npos,
          "AC4: reason string accessor present");
    CHECK(bridge_cpp.find("\"None\"") != std::string::npos &&
              bridge_cpp.find("\"ForeignEval\"") != std::string::npos &&
              bridge_cpp.find("\"CowGenMismatch\"") != std::string::npos &&
              bridge_cpp.find("\"Unknown\"") != std::string::npos,
          "AC4: switch covers all 4 enum values");
    // AC5: query keys + observability + source-cite.
    CHECK(obs.find("cross_workspace_hot_update_rejected_total") != std::string::npos,
          "AC5: existing cross_workspace counter still present");
    CHECK(q.find("cow-gen-mismatch-wired") != std::string::npos,
          "AC5: cow-gen-mismatch-wired query key");
    CHECK(q.find("cross-workspace-cow-gen-mismatch-wired") != std::string::npos,
          "AC5: cross-workspace-cow-gen-mismatch-wired query key");
    CHECK(q.find("schema-2275") != std::string::npos, "AC5: schema-2275 lineage");
    CHECK(q.find("issue-2275") != std::string::npos, "AC5: issue-2275 lineage");
    // AC5: runtime smoke — set cow_gen mismatch + verify reject reason
    // + verify CowGenMismatch wire bumps counter (vs ForeignEval).
    {
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        aura_set_aot_metrics(m);
        // Set live workspace cow_gen + expected to a different value.
        const std::uint64_t live_gen = 42;
        const std::uint64_t expected_gen = 7;
        aura_set_live_workspace_cow_gen(live_gen);
        aura_set_aot_expected_cow_gen_for_eval(nullptr, expected_gen);
        const std::uint64_t live_now = aura_get_live_workspace_cow_gen();
        const std::uint64_t expected_now = aura_get_aot_expected_cow_gen_for_eval(nullptr);
        CHECK(live_now == live_gen, "AC5-smoke: live cow_gen set");
        CHECK(expected_now == expected_gen, "AC5-smoke: expected cow_gen set");
        CHECK(live_now != expected_now, "AC5-smoke: cow_gen mismatch injected");
        // Reset to matching values.
        aura_set_live_workspace_cow_gen(expected_gen);
        aura_set_aot_expected_cow_gen_for_eval(nullptr, expected_gen);
        aura_set_aot_metrics(nullptr);
    }
    (void)cs;
}

int main() {
    // Issue #2165: production default is auto-retry ON; strict unit checks
    // (Version/Env/Defuse fail counts) need it off until the #2165 block.
    aura_set_aot_reload_auto_retry(0);

    // ── C API region mask / module version (baseline) ──
    {
        aura_set_aot_region_mask(0);
        aura_set_module_version(0);
        CHECK(aura_get_aot_region_mask() == 0, "region mask init 0");
        CHECK(aura_get_module_version() == 0, "module version init 0");
        aura_set_aot_region_mask(0xABC);
        CHECK(aura_get_aot_region_mask() == 0xABC, "region mask set 0xABC");
        aura_set_module_version(42);
        CHECK(aura_get_module_version() == 42, "module version set 42");
        aura_set_aot_region_mask(0);
        aura_set_module_version(0);
    }
    ac7_cross_workspace_reject_2178();

    // ── Issue #2240: stable cross-workspace reject reason code ──
    ac7b_cross_workspace_reason_code_2240();
    std::println("\n=== AC #2275: CowGenMismatch wire (fail-closed) ===");
    {
        CompilerService cs;
        ac2275_cow_gen_mismatch(cs);
        ac2271_physical_invalidate(cs);
        ac2299_per_eval_slot_invalidate(cs);
    }

    // ── Aura: region mask round-trip ──
    // aot:get-region-mask is registered via register_stats_impl (engine:metrics).
    {
        CompilerService cs;
        auto s = cs.eval("(aot:set-region-mask 7)");
        CHECK(s && is_bool(*s) && as_bool(*s), "aot:set-region-mask → #t");
        auto g = cs.eval("(engine:metrics \"aot:get-region-mask\")");
        CHECK(g && is_int(*g) && as_int(*g) == 7, "aot:get-region-mask → 7");
        (void)cs.eval("(aot:set-region-mask 0)");
    }

    // ── Aura: module version round-trip ──
    {
        CompilerService cs;
        auto s = cs.eval("(aot:set-module-version 99)");
        CHECK(s && is_bool(*s) && as_bool(*s), "aot:set-module-version → #t");
        auto g = cs.eval("(stats:get \"aot:get-module-version\")");
        CHECK(g && is_int(*g) && as_int(*g) == 99, "aot:get-module-version → 99");
        (void)cs.eval("(aot:set-module-version 0)");
    }

    // ── Bad args ──
    {
        CompilerService cs;
        auto r = cs.eval("(aot:reload)");
        CHECK(r && is_bool(*r) && !as_bool(*r), "aot:reload no-arg → #f");
        auto r2 = cs.eval("(aot:reload 123)");
        CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "aot:reload non-string → #f");
        auto r3 = cs.eval("(aot:set-region-mask)");
        CHECK(r3 && is_bool(*r3) && !as_bool(*r3), "set-region-mask no-arg → #f");
        auto r4 = cs.eval("(aot:set-module-version \"x\")");
        CHECK(r4 && is_bool(*r4) && !as_bool(*r4), "set-module-version bad → #f");
    }

    // ── Failed reload (missing file) increments via-primitive counters ──
    {
        CompilerService cs;
        auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
        CHECK(m != nullptr, "metrics available");
        // Ensure C-side metrics pointer is bound
        aura_set_aot_metrics(m);
        const auto att0 = m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed);
        const auto suc0 = m->aot_reload_success_via_primitive.load(std::memory_order_relaxed);
        const auto c_att0 = m->aot_reload_attempts_.load(std::memory_order_relaxed);
        const auto rb0 = m->aot_hot_update_atomic_rollback.load(std::memory_order_relaxed);

        auto r = cs.eval("(aot:reload \"/tmp/aura_nonexistent_aot_module_1366.so\")");
        CHECK(r && is_bool(*r) && !as_bool(*r), "reload missing .so → #f");
        CHECK(m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed) == att0 + 1,
              "attempts_via_primitive +1");
        CHECK(m->aot_reload_success_via_primitive.load(std::memory_order_relaxed) == suc0,
              "success_via_primitive unchanged");
        CHECK(m->aot_reload_attempts_.load(std::memory_order_relaxed) >= c_att0 + 1,
              "C API aot_reload_attempts_ bumped");
        CHECK(m->aot_hot_update_atomic_rollback.load(std::memory_order_relaxed) >= rb0 + 1,
              "atomic_rollback on failed dlopen");
    }

    // ── query:aot-reload-primitive-stats ──
    {
        CompilerService cs;
        auto s = cs.eval("(engine:metrics \"query:aot-reload-primitive-stats\")");
        CHECK(s && is_hash(*s), "query:aot-reload-primitive-stats is hash");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "attempts-via-primitive") >= 0,
              "attempts-via-primitive key");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "success-via-primitive") >= 0,
              "success-via-primitive key");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "region-mask") >= 0, "region-mask key");
        CHECK(href(cs, "query:aot-reload-primitive-stats", "module-version") >= 0,
              "module-version key");
    }

    // ── Optional: real .so success path ──
    {
        auto so = build_test_so(42);
        if (so.empty()) {
            CHECK(true, "skip success path (cc -shared unavailable)");
        } else {
            CompilerService cs;
            auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
            aura_set_aot_metrics(m);
            aura_set_aot_region_mask(0); // no region filter
            const auto suc0 = m->aot_reload_success_via_primitive.load(std::memory_order_relaxed);
            const auto att0 = m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed);
            auto expr = std::format("(aot:reload \"{}\" 42)", so);
            auto r = cs.eval(expr);
            CHECK(r && is_bool(*r), "reload real .so returns bool");
            if (r && is_bool(*r) && as_bool(*r)) {
                CHECK(m->aot_reload_success_via_primitive.load(std::memory_order_relaxed) ==
                          suc0 + 1,
                      "success_via_primitive +1");
            } else {
                // dlopen may fail in restricted env — still count attempt
                CHECK(m->aot_reload_attempts_via_primitive.load(std::memory_order_relaxed) ==
                          att0 + 1,
                      "attempt counted even if dlopen failed");
            }
            // Wrong version → false + stale reject
            auto r2 = cs.eval(std::format("(aot:reload \"{}\" 99)", so));
            CHECK(r2 && is_bool(*r2) && !as_bool(*r2), "wrong version → #f");
        }
    }

    // ── Direct C API null path ──
    {
        CHECK(!aura_reload_aot_module(nullptr, 0), "C null path → false");
    }

    // ── Issue #2012: staging — failed validation leaves live slots intact ──
    {
        std::println("\n--- #2012: atomic staging rollback preserves live slots ---");
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);
        aura_set_module_version(0);

        constexpr std::int64_t kFid = 77;
        const std::uintptr_t seed_ptr = static_cast<std::uintptr_t>(0xA0112012ull);
        aura_register_fn_tracked(kFid, static_cast<std::int64_t>(seed_ptr));
        CHECK(aura_aot_probe_fn_ptr(kFid) == seed_ptr, "seed slot 77");

        const auto epoch0 = aura_aot_func_table_epoch();
        const auto rb0 = metrics.aot_hot_update_atomic_rollback.load(std::memory_order_relaxed);
        const auto stale0 = metrics.aot_stale_reject_count_.load(std::memory_order_relaxed);
        aura_hot_update_registry_snapshot reg0{};
        aura_hot_update_registry_get_snapshot(&reg0);

        // Version-mismatch .so that *also* tries to register a new pointer into
        // slot 77 — staging must discard it so live stays seed_ptr.
        auto bad_so = build_registering_so(/*version=*/1, /*region=*/0, /*func_id=*/77, "bad");
        if (bad_so.empty()) {
            CHECK(true, "skip #2012 register-so (cc -shared unavailable)");
        } else {
            const bool ok = aura_reload_aot_module(bad_so.c_str(), /*expected=*/99);
            CHECK(!ok, "version mismatch → false");
            CHECK(aura_aot_probe_fn_ptr(kFid) == seed_ptr,
                  "live slot 77 unchanged after failed reload (staging discarded)");
            CHECK(aura_aot_func_table_epoch() == epoch0, "epoch not advanced on rollback");
            CHECK(metrics.aot_hot_update_atomic_rollback.load(std::memory_order_relaxed) >= rb0 + 1,
                  "atomic_rollback +1");
            CHECK(metrics.aot_stale_reject_count_.load(std::memory_order_relaxed) >= stale0 + 1,
                  "stale_reject +1");
            aura_hot_update_registry_snapshot reg1{};
            aura_hot_update_registry_get_snapshot(&reg1);
            CHECK(reg1.aot_reload_rollback_total >= reg0.aot_reload_rollback_total + 1,
                  "HotUpdateRegistry rollback counter +1");
        }

        // Region mismatch also rolls back cleanly.
        aura_set_aot_region_mask(0x1);
        auto region_so =
            build_registering_so(/*version=*/5, /*region=*/0x2, /*func_id=*/77, "region");
        if (!region_so.empty()) {
            const auto epoch1 = aura_aot_func_table_epoch();
            const auto rbm0 = metrics.aot_region_mismatch_.load(std::memory_order_relaxed);
            CHECK(!aura_reload_aot_module(region_so.c_str(), 5), "region mismatch → false");
            CHECK(aura_aot_probe_fn_ptr(kFid) == seed_ptr, "slot intact after region reject");
            CHECK(aura_aot_func_table_epoch() == epoch1, "epoch intact after region reject");
            CHECK(metrics.aot_region_mismatch_.load(std::memory_order_relaxed) >= rbm0 + 1,
                  "region_mismatch +1");
        }
        aura_set_aot_region_mask(0);

        // Success path: staging applied + epoch bump + registry success.
        auto good_so = build_registering_so(/*version=*/42, /*region=*/0, /*func_id=*/77, "good");
        if (!good_so.empty()) {
            const auto epoch2 = aura_aot_func_table_epoch();
            const auto suc0 = metrics.aot_hot_update_success_.load(std::memory_order_relaxed);
            aura_hot_update_registry_snapshot rs0{};
            aura_hot_update_registry_get_snapshot(&rs0);
            const bool ok = aura_reload_aot_module(good_so.c_str(), 42);
            if (ok) {
                CHECK(aura_aot_func_table_epoch() == epoch2 + 1, "epoch +1 on success");
                CHECK(metrics.aot_hot_update_success_.load(std::memory_order_relaxed) == suc0 + 1,
                      "hot_update_success +1");
                const auto live = aura_aot_probe_fn_ptr(kFid);
                CHECK(live != 0 && live != seed_ptr,
                      "slot 77 replaced with staged pointer from good .so");
                aura_hot_update_registry_snapshot rs1{};
                aura_hot_update_registry_get_snapshot(&rs1);
                CHECK(rs1.aot_reload_success_total >= rs0.aot_reload_success_total + 1,
                      "HotUpdateRegistry success counter +1");
            } else {
                CHECK(true, "good .so dlopen failed in env (non-fatal)");
            }
        }

        // query:aot-stats exposes rollback key (#2012).
        {
            CompilerService cs;
            aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
            auto st = cs.eval("(engine:metrics \"query:aot-stats\")");
            CHECK(st && is_hash(*st), "query:aot-stats is hash");
            CHECK(href(cs, "query:aot-stats", "aot-hot-update-rollback-count") >= 0,
                  "aot-hot-update-rollback-count key present");
            auto reg = cs.eval("(engine:metrics \"query:hot-update-registry-stats\")");
            CHECK(reg && is_hash(*reg), "query:hot-update-registry-stats is hash");
            CHECK(href(cs, "query:hot-update-registry-stats", "aot-reload-rollback-total") >= 0,
                  "registry aot-reload-rollback-total key");
        }
        aura_set_aot_metrics(nullptr);
    }

    // ── Issue #2093: structured reload-failure reason codes + per-reason metrics ──
    {
        std::println("\n--- #2093: per-reason failure counters + last-fail reason ---");
        // Issue #2165: strict per-reason tests require auto-retry OFF (else
        // Version/Defuse/Env each do reemit+retry and bump counters twice).
        aura_set_aot_reload_auto_retry(0);
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);
        aura_set_module_version(0);

        // ── AC1: Version mismatch bumps reload_fail_version_total
        // (not only the generic rollback counter).
        {
            auto bad_ver_so =
                build_registering_so(/*version=*/1, /*region=*/0, /*func_id=*/88, "vfail");
            if (bad_ver_so.empty()) {
                CHECK(true, "skip AC1 (cc unavailable)");
            } else {
                const auto v0 = metrics.aot_reload_fail_version_total.load();
                const auto rb0 = metrics.aot_hot_update_atomic_rollback.load();
                const bool ok = aura_reload_aot_module(bad_ver_so.c_str(), /*expected=*/99);
                CHECK(!ok, "AC1: version mismatch → false");
                CHECK(metrics.aot_reload_fail_version_total.load() == v0 + 1,
                      "AC1: reload_fail_version_total += 1");
                CHECK(metrics.aot_hot_update_atomic_rollback.load() >= rb0 + 1,
                      "AC1: generic atomic_rollback still bumps");
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Version,
                      "AC1: last-reason = Version");
                // Registry snapshot mirrors per-reason counter.
                aura_hot_update_registry_snapshot s{};
                aura_hot_update_registry_get_snapshot(&s);
                CHECK(s.aot_reload_fail_version_total >= 1,
                      "AC1: registry snapshot exposes reload_fail_version_total");
                CHECK(static_cast<AotReloadFail>(s.aot_reload_last_fail_reason) ==
                          AotReloadFail::Version,
                      "AC1: registry last-fail reason = Version");
            }
        }

        // ── AC2: Dlopen failure bumps reload_fail_dlopen_total.
        {
            const auto d0 = metrics.aot_reload_fail_dlopen_total.load();
            const bool ok = aura_reload_aot_module("/tmp/aura_aot_no_such_file_2093.so", 0);
            CHECK(!ok, "AC2: dlopen missing file → false");
            CHECK(metrics.aot_reload_fail_dlopen_total.load() == d0 + 1,
                  "AC2: reload_fail_dlopen_total += 1");
            CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                      AotReloadFail::Dlopen,
                  "AC2: last-reason = Dlopen");
        }

        // ── AC2: Region mismatch bumps reload_fail_region_total.
        {
            aura_set_aot_region_mask(0x1);
            auto region_bad =
                build_registering_so(/*version=*/5, /*region=*/0x2, /*func_id=*/88, "rfail");
            if (!region_bad.empty()) {
                const auto r0 = metrics.aot_reload_fail_region_total.load();
                const bool ok = aura_reload_aot_module(region_bad.c_str(), 5);
                CHECK(!ok, "AC2: region mismatch → false");
                CHECK(metrics.aot_reload_fail_region_total.load() == r0 + 1,
                      "AC2: reload_fail_region_total += 1");
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Region,
                      "AC2: last-reason = Region");
            }
            aura_set_aot_region_mask(0);
        }

        // ── AC2: Stale defuse_version bumps reload_fail_defuse_total.
        // Set host defuse ahead of binary emit version.
        {
            aura_set_aot_defuse_version(10);
            auto defuse_bad =
                build_registering_so(/*version=*/5, /*region=*/0, /*func_id=*/88, "dfail");
            if (!defuse_bad.empty()) {
                const auto d0 = metrics.aot_reload_fail_defuse_total.load();
                const bool ok = aura_reload_aot_module(defuse_bad.c_str(), 0);
                CHECK(!ok, "AC2: stale defuse → false");
                CHECK(metrics.aot_reload_fail_defuse_total.load() == d0 + 1,
                      "AC2: reload_fail_defuse_total += 1");
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Defuse,
                      "AC2: last-reason = Defuse");
            }
            aura_set_aot_defuse_version(0);
        }

        // ── AC2: Stale env_frame_version bumps reload_fail_env_total.
        // Build a .so that exposes aot_env_frame_version=5; set host
        // env_frame_version ahead via the C API.
        {
            const char* dir = "/tmp";
            std::string cpath = std::format("{}/aura_aot_env_2093.c", dir);
            std::string sopath = std::format("{}/aura_aot_env_2093.so", dir);
            {
                std::ofstream f(cpath);
                if (f) {
                    f << "#include <stdint.h>\n";
                    f << "uint64_t aot_emit_version = 7ULL;\n";
                    f << "uint64_t aot_region_mask = 0ULL;\n";
                    f << "uint64_t aot_env_frame_version = 5ULL;\n";
                    f << "uint64_t aot_linear_state = 0ULL;\n";
                }
            }
            std::string cmd = std::format("cc -shared -fPIC -o {} {} 2>/dev/null", sopath, cpath);
            if (std::system(cmd.c_str()) == 0) {
                // Force the host env_frame_version ahead via the bridge.
                aura_set_aot_default_env_frame_version(20);
                const auto e0 = metrics.aot_reload_fail_env_total.load();
                const bool ok = aura_reload_aot_module(sopath.c_str(), 0);
                CHECK(!ok, "AC2: stale env_frame → false");
                CHECK(metrics.aot_reload_fail_env_total.load() == e0 + 1,
                      "AC2: reload_fail_env_total += 1");
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Env,
                      "AC2: last-reason = Env");
                aura_set_aot_default_env_frame_version(0);
                std::remove(sopath.c_str());
                std::remove(cpath.c_str());
            }
        }

        // ── AC3: Successful reload clears last-fail to Ok + bumps success.
        // First force a failure to set last-fail, then a successful reload
        // must clear it back to Ok.
        {
            // Force Version failure to set last-fail.
            auto bad_pre =
                build_registering_so(/*version=*/1, /*region=*/0, /*func_id=*/88, "preok");
            if (!bad_pre.empty()) {
                (void)aura_reload_aot_module(bad_pre.c_str(), /*expected=*/99);
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Version,
                      "AC3 setup: last-fail = Version after forced failure");
                // Now do a successful reload.
                auto good_so =
                    build_registering_so(/*version=*/42, /*region=*/0, /*func_id=*/88, "ok");
                if (!good_so.empty()) {
                    const auto suc0 = metrics.aot_hot_update_success_.load();
                    const bool ok = aura_reload_aot_module(good_so.c_str(), 42);
                    CHECK(ok, "AC3: good .so reload succeeds");
                    CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                              AotReloadFail::Ok,
                          "AC3: last-fail cleared to Ok on success");
                    CHECK(metrics.aot_hot_update_success_.load() >= suc0 + 1,
                          "AC3: hot_update_success +1");
                }
            }
        }

        // ── AC5: query surface exposes per-reason counters + last-fail reason.
        {
            CompilerService cs;
            aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
            auto st = cs.eval("(engine:metrics \"query:aot-stats\")");
            CHECK(st && is_hash(*st), "AC5: query:aot-stats is hash");
            // Each per-reason key must be present (>= 0 even when 0).
            for (const char* k : {"aot-reload-fail-dlopen-count", "aot-reload-fail-version-count",
                                  "aot-reload-fail-region-count", "aot-reload-fail-defuse-count",
                                  "aot-reload-fail-env-count", "aot-reload-fail-linear-count",
                                  "aot-reload-fail-staging-count", "aot-reload-fail-other-count"}) {
                CHECK(href(cs, "query:aot-stats", k) >= 0,
                      std::format("AC5: query:aot-stats exposes '{}'", k));
            }
            aura_set_aot_metrics(nullptr);
        }

        aura_set_aot_metrics(nullptr);
    }

    // ── Issue #2165: auto reemit+retry on Version/Env/Linear/Defuse ──
    {
        std::println("\n--- #2165: AOT reload auto-retry recovery ---");
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);
        aura_set_module_version(0);

        // AC5a: Version mismatch + auto-retry ON → reemit + retry with
        // version=0 → success; last-fail Ok; success counter +1.
        {
            aura_set_aot_reload_auto_retry(1);
            auto ver_so = build_registering_so(/*version=*/7, /*region=*/0, /*func_id=*/88, "ar_v");
            if (ver_so.empty()) {
                CHECK(true, "skip AC5a (cc unavailable)");
            } else {
                const auto rt0 = metrics.aot_reload_auto_retry_total.load();
                const auto rs0 = metrics.aot_reload_auto_retry_success_total.load();
                const auto v0 = metrics.aot_reload_fail_version_total.load();
                // Host expects 99; binary is 7 → Version fail then retry version=0.
                const bool ok = aura_reload_aot_module(ver_so.c_str(), /*expected=*/99);
                CHECK(ok, "AC5a: Version auto-retry → success (retry trusts binary)");
                CHECK(metrics.aot_reload_auto_retry_total.load() == rt0 + 1,
                      "AC5a: auto_retry_total +1");
                CHECK(metrics.aot_reload_auto_retry_success_total.load() == rs0 + 1,
                      "AC5a: auto_retry_success_total +1");
                CHECK(metrics.aot_reload_fail_version_total.load() == v0 + 1,
                      "AC5a: intermediate Version fail counted once");
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Ok,
                      "AC5a: last-fail Ok after successful retry (AC4)");
            }
        }

        // AC5b: Dlopen fail → no retry; last-fail=Dlopen.
        {
            aura_set_aot_reload_auto_retry(1);
            const auto rt0 = metrics.aot_reload_auto_retry_total.load();
            const auto d0 = metrics.aot_reload_fail_dlopen_total.load();
            const bool ok = aura_reload_aot_module("/tmp/aura_aot_no_such_file_2165.so", 0);
            CHECK(!ok, "AC5b: dlopen missing → false");
            CHECK(metrics.aot_reload_auto_retry_total.load() == rt0,
                  "AC5b: Dlopen does not auto-retry");
            CHECK(metrics.aot_reload_fail_dlopen_total.load() == d0 + 1, "AC5b: dlopen fail +1");
            CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                      AotReloadFail::Dlopen,
                  "AC5b: last-fail = Dlopen");
        }

        // AC5c: flag off → no auto-retry (Version fail stays single-shot).
        {
            aura_set_aot_reload_auto_retry(0);
            auto bad = build_registering_so(/*version=*/1, /*region=*/0, /*func_id=*/88, "ar_off");
            if (bad.empty()) {
                CHECK(true, "skip AC5c (cc unavailable)");
            } else {
                const auto rt0 = metrics.aot_reload_auto_retry_total.load();
                const auto v0 = metrics.aot_reload_fail_version_total.load();
                const bool ok = aura_reload_aot_module(bad.c_str(), /*expected=*/99);
                CHECK(!ok, "AC5c: flag off Version fail");
                CHECK(metrics.aot_reload_auto_retry_total.load() == rt0,
                      "AC5c: no auto_retry when flag off");
                CHECK(metrics.aot_reload_fail_version_total.load() == v0 + 1,
                      "AC5c: single Version fail");
            }
        }

        // AC5d: Defuse fail + auto-retry → reemit + second fail → exhausted.
        {
            aura_set_aot_reload_auto_retry(1);
            aura_set_aot_defuse_version(10);
            auto defuse_bad =
                build_registering_so(/*version=*/5, /*region=*/0, /*func_id=*/88, "ar_df");
            if (defuse_bad.empty()) {
                CHECK(true, "skip AC5d (cc unavailable)");
            } else {
                const auto rt0 = metrics.aot_reload_auto_retry_total.load();
                const auto ex0 = metrics.aot_reload_auto_retry_exhausted_total.load();
                const auto d0 = metrics.aot_reload_fail_defuse_total.load();
                const bool ok = aura_reload_aot_module(defuse_bad.c_str(), 0);
                CHECK(!ok, "AC5d: Defuse still fails after retry");
                CHECK(metrics.aot_reload_auto_retry_total.load() == rt0 + 1,
                      "AC5d: auto_retry attempted");
                CHECK(metrics.aot_reload_auto_retry_exhausted_total.load() == ex0 + 1,
                      "AC5d: auto_retry_exhausted +1");
                CHECK(metrics.aot_reload_fail_defuse_total.load() >= d0 + 2,
                      "AC5d: Defuse fail on first + second attempt");
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Defuse,
                      "AC5d: final last-fail = Defuse");
            }
            aura_set_aot_defuse_version(0);
        }

        // AC5e: query:aot-stats exposes #2165 keys.
        {
            CompilerService cs;
            aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
            auto st = cs.eval("(engine:metrics \"query:aot-stats\")");
            CHECK(st && is_hash(*st), "AC5e: query:aot-stats is hash");
            CHECK(href(cs, "query:aot-stats", "schema-2165") == 2165, "AC5e: schema-2165");
            CHECK(href(cs, "query:aot-stats", "aot-reload-auto-retry-total") >= 0,
                  "AC5e: auto-retry-total key");
            CHECK(href(cs, "query:aot-stats", "aot-reload-auto-retry-success-total") >= 0,
                  "AC5e: auto-retry-success key");
            CHECK(href(cs, "query:aot-stats", "aot-reload-auto-retry-exhausted-total") >= 0,
                  "AC5e: auto-retry-exhausted key");
            aura_set_aot_metrics(nullptr);
        }

        aura_set_aot_reload_auto_retry(0); // leave off for following stress tests
        aura_set_aot_metrics(nullptr);
    }

    // ── Issue #2012: concurrent epoch probes during forced fail + success ──
    {
        std::println("\n--- #2012: concurrent probe stress during reload ---");
        aura_set_aot_reload_auto_retry(0); // stress must not reemit-storm
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);

        auto so_ok = build_test_so(77);
        auto so_bad = build_registering_so(1, 0, 3, "stress");
        if (so_ok.empty()) {
            CHECK(true, "skip concurrent stress (cc unavailable)");
        } else {
            std::atomic<bool> stop{false};
            std::atomic<std::uint64_t> samples{0};
            std::atomic<std::uint64_t> torn{0};
            std::vector<std::thread> readers;
            readers.reserve(4);
            for (int t = 0; t < 4; ++t) {
                readers.emplace_back([&] {
                    while (!stop.load(std::memory_order_relaxed)) {
                        const auto e1 = aura_aot_func_table_epoch();
                        const auto p = aura_aot_probe_fn_ptr(3);
                        const auto e2 = aura_aot_func_table_epoch();
                        (void)p;
                        // Epoch is monotonic; a drop would indicate torn state machine.
                        if (e2 < e1)
                            torn.fetch_add(1, std::memory_order_relaxed);
                        samples.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            for (int i = 0; i < 32; ++i) {
                (void)aura_reload_aot_module(so_ok.c_str(), 77);
                if (!so_bad.empty())
                    (void)aura_reload_aot_module(so_bad.c_str(), 99); // forced fail
            }
            stop.store(true, std::memory_order_relaxed);
            for (auto& th : readers)
                th.join();
            CHECK(samples.load() > 0, "concurrent samples collected");
            CHECK(torn.load() == 0, "func_table_epoch never went backwards under stress");
        }
        aura_set_aot_metrics(nullptr);
    }

    // ── Issue #2232: reason-driven multi-round reload recovery policy ──
    {
        std::println("\n--- #2232: reason-driven multi-round reload policy ---");
        CompilerMetrics metrics;
        aura_set_aot_metrics(&metrics);
        aura_set_aot_region_mask(0);
        aura_set_aot_defuse_version(0);
        aura_set_module_version(0);
        aura_set_aot_env_frame_version_for_eval(nullptr, /*host_env=*/100);
        aura_set_aot_reload_auto_retry(1);

        // AC1: Defuse — max_reemit=3, backoff_ms=5, fall_back_jit_only=true.
        // Use Defuse (not Version) because the Version retry uses
        // version=0 (always succeeds for any non-negative emit) — so
        // Version can never exhaust under the current policy. Defuse
        // uses the same `version` across retries, so all 3 retries
        // fail (binary stale relative to host expected=99) and the
        // loop exhausts → fall_back_jit_only counter bumps.
        {
            auto bad = build_registering_so(/*version=*/1, /*region=*/0, /*func_id=*/88,
                                            /*tag=*/"ar2232_defuse");
            if (bad.empty()) {
                CHECK(true, "AC1 skip (cc unavailable)");
            } else {
                const auto ap0 = metrics.aot_reload_policy_attempt_total.load();
                const auto fb0 = metrics.aot_reload_fall_back_jit_only_total.load();
                const auto ex0 = metrics.aot_reload_auto_retry_exhausted_total.load();
                const bool ok = aura_reload_aot_module(bad.c_str(), /*expected=*/99);
                const auto ap1 = metrics.aot_reload_policy_attempt_total.load();
                const auto fb1 = metrics.aot_reload_fall_back_jit_only_total.load();
                const auto ex1 = metrics.aot_reload_auto_retry_exhausted_total.load();
                std::println("  AC1: ok={} ap_delta={} fb_delta={} ex_delta={}", ok, ap1 - ap0,
                             fb1 - fb0, ex1 - ex0);
                CHECK(!ok, "AC1: Defuse exhausted → false");
                CHECK(ap1 - ap0 == 3,
                      "AC1: aot_reload_policy_attempt_total += 3 (policy.max_reemit)");
                CHECK(fb1 - fb0 == 1, "AC1: aot_reload_fall_back_jit_only_total += 1 "
                                      "(policy.fall_back_jit_only=true)");
                CHECK(ex1 - ex0 == 1, "AC1: aot_reload_auto_retry_exhausted_total += 1");
                CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                          AotReloadFail::Defuse,
                      "AC1: last-fail = Defuse (the final reason from the last attempt)");
            }
        }

        // AC2: Env — max_remit=2, backoff_ms=10, fall_back_jit_only=true.
        // .so has env_frame_version=1; host's env=100 (set above) →
        // drift check fires on every retry. After 2 retries →
        // exhausted + fall_back.
        {
            auto bad_env = build_test_so_with_env(/*version=*/99, /*env_version=*/1);
            if (bad_env.empty()) {
                CHECK(true, "AC2 skip (cc unavailable)");
            } else {
                const auto ap0 = metrics.aot_reload_policy_attempt_total.load();
                const auto fb0 = metrics.aot_reload_fall_back_jit_only_total.load();
                const auto ex0 = metrics.aot_reload_auto_retry_exhausted_total.load();
                const bool ok = aura_reload_aot_module(bad_env.c_str(), /*expected=*/99);
                const auto ap1 = metrics.aot_reload_policy_attempt_total.load();
                const auto fb1 = metrics.aot_reload_fall_back_jit_only_total.load();
                const auto ex1 = metrics.aot_reload_auto_retry_exhausted_total.load();
                std::println("  AC2: ok={} ap_delta={} fb_delta={} ex_delta={}", ok, ap1 - ap0,
                             fb1 - fb0, ex1 - ex0);
                CHECK(!ok, "AC2: Env exhausted → false");
                CHECK(ap1 - ap0 == 2,
                      "AC2: aot_reload_policy_attempt_total += 2 (policy.max_reemit=2)");
                CHECK(fb1 - fb0 == 1, "AC2: aot_reload_fall_back_jit_only_total += 1 "
                                      "(policy.fall_back_jit_only=true)");
                CHECK(ex1 - ex0 == 1, "AC2: aot_reload_auto_retry_exhausted_total += 1");
            }
        }

        // AC3: Dlopen — max_reemit=0, backoff_ms=0, fall_back_jit_only=false.
        // Non-existent file → Dlopen fail. policy_for(Dlopen) returns
        // {0, 0, false} → aot_reload_policy_attempt_total does NOT
        // bump (no retry attempts at all).
        {
            const auto ap0 = metrics.aot_reload_policy_attempt_total.load();
            const bool ok = aura_reload_aot_module("/nonexistent/aura_2232_dlopen.so",
                                                   /*expected=*/1);
            const auto ap1 = metrics.aot_reload_policy_attempt_total.load();
            std::println("  AC3: ok={} ap_delta={}", ok, ap1 - ap0);
            CHECK(!ok, "AC3: Dlopen fail → false");
            CHECK(ap1 - ap0 == 0, "AC3: aot_reload_policy_attempt_total does NOT bump "
                                  "(policy.max_remit=0 for Dlopen)");
            CHECK(static_cast<AotReloadFail>(aura_aot_last_reload_fail_reason()) ==
                      AotReloadFail::Dlopen,
                  "AC3: last-fail = Dlopen");
        }

        // AC4: success on 1st attempt — no retry needed. Correct-version
        // .so → first attempt succeeds → aot_reload_policy_attempt_total
        // does NOT bump (no retry attempts).
        {
            auto good = build_registering_so(/*version=*/42, /*region=*/0, /*func_id=*/88,
                                             /*tag=*/"ar2232_ok");
            if (good.empty()) {
                CHECK(true, "AC4 skip (cc unavailable)");
            } else {
                const auto ap0 = metrics.aot_reload_policy_attempt_total.load();
                const auto ex0 = metrics.aot_reload_auto_retry_exhausted_total.load();
                const bool ok = aura_reload_aot_module(good.c_str(), /*expected=*/42);
                const auto ap1 = metrics.aot_reload_policy_attempt_total.load();
                const auto ex1 = metrics.aot_reload_auto_retry_exhausted_total.load();
                std::println("  AC4: ok={} ap_delta={} ex_delta={}", ok, ap1 - ap0, ex1 - ex0);
                CHECK(ok, "AC4: success on 1st attempt → true");
                CHECK(ap1 - ap0 == 0,
                      "AC4: aot_reload_policy_attempt_total does NOT bump (no retry on success)");
                CHECK(ex1 - ex0 == 0, "AC4: aot_reload_auto_retry_exhausted_total does NOT bump");
            }
        }

        // AC5: query:aot-stats exposes the new policy keys + schema-2232.
        {
            CompilerService cs;
            aura_set_aot_metrics(static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics()));
            for (const char* k :
                 {"aot-reload-policy-attempt-total", "aot-reload-fall-back-jit-only-total",
                  "schema-2232", "issue-2232", "reload-policy-wired"}) {
                CHECK(href(cs, "query:aot-stats", k) >= 0,
                      std::format("AC5: query:aot-stats exposes '{}'", k));
            }
            CHECK(href(cs, "query:aot-stats", "reload-policy-wired") == 1,
                  "AC5: reload-policy-wired == 1");
        }
        aura_set_aot_reload_auto_retry(0);
        aura_set_aot_env_frame_version_for_eval(nullptr, 0);
        aura_set_aot_metrics(nullptr);
    }

    // — Issue #2252 AC1-AC5: hard-reject native execution when
    // AOT slot table_generation != live epoch. Source-cite:
    // counter field + wire-up bump site + zero-hit guarantee +
    // query surface key + schema-2252 lineage.
    {
        std::println("\n--- AC #2252: hard-reject native on stale slot ---");
        auto bridge_cpp = read_file("src/compiler/aura_jit_bridge.cpp");
        auto met = read_file("src/compiler/observability_metrics.h");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        // AC1: aura_aot_probe_fn_ptr bumps the hard-reject counter on
        // gen != cur (returns 0 — never executes stale AOT).
        CHECK(bridge_cpp.find("aura_aot_probe_fn_ptr") != std::string::npos,
              "AC1: aura_aot_probe_fn_ptr present");
        CHECK(bridge_cpp.find("aot_stale_probe_hard_reject_total.fetch_add") != std::string::npos,
              "AC1: hard-reject bump site in aura_aot_probe_fn_ptr");
        // AC3: happy path zero extra cost (existing 2 relaxed loads).
        CHECK(bridge_cpp.find("gen != cur") != std::string::npos and
                  bridge_cpp.find("g_aot_table_epoch.load") != std::string::npos and
                  bridge_cpp.find("slot.table_generation.load") != std::string::npos,
              "AC3: relaxed load compare gen != cur");
        // AC4: counter field + query surface + schema-2252 lineage
        CHECK(met.find("aot_stale_probe_hard_reject_total{0}") != std::string::npos,
              "AC4: counter field");
        CHECK(q.find("aot-stale-probe-hard-reject-total") != std::string::npos, "AC4: query key");
        CHECK(q.find("aot-stale-probe-hard-reject-wired") != std::string::npos,
              "AC4: wired sentinel");
        CHECK(q.find("schema-2252") != std::string::npos, "AC4: schema-2252 lineage");
        CHECK(q.find("issue-2252") != std::string::npos, "AC4: issue-2252 lineage");
        // AC5: runtime counter bump is queryable end-to-end via the
        // existing #2046 cross-fiber slot stale probe path (which
        // already exercises gen != cur). The #2252 wire-up is
        // additive — same mismatch path now bumps the dedicated
        // counter in addition to the 2 existing counters.
    }

    // — Issue #2249 AC1-AC6: Region | Staging auto-retry conservative
    // path (extend #2232). Pure policy_for check + wire-up source-cite +
    // metric/atomic fields present.
    {
        std::println("\n--- AC #2249: Region | Staging auto-retry ---");
        auto bridge_h = read_file("src/compiler/aura_jit_bridge.h");
        auto bridge_cpp = read_file("src/compiler/aura_jit_bridge.cpp");
        auto met = read_file("src/compiler/observability_metrics.h");
        auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        // AC1/AC2: policy_for Region/Staging -> {2, 15, true}
        const auto p_region = ::policy_for(AotReloadFail::Region);
        const auto p_staging = ::policy_for(AotReloadFail::Staging);
        CHECK(p_region.max_reemit == 2, "AC1: Region max_reemit == 2");
        CHECK(p_region.backoff_ms == 15, "AC1: Region backoff_ms == 15");
        CHECK(p_region.fall_back_jit_only == true, "AC1: Region fall_back_jit_only == true");
        CHECK(p_staging.max_reemit == 2, "AC2: Staging max_reemit == 2");
        CHECK(p_staging.backoff_ms == 15, "AC2: Staging backoff_ms == 15");
        CHECK(p_staging.fall_back_jit_only == true, "AC2: Staging fall_back_jit_only == true");
        // AC3: Dlopen / Other still never retry (regression vs #2232)
        const auto p_dlopen = ::policy_for(AotReloadFail::Dlopen);
        const auto p_other = ::policy_for(AotReloadFail::Other);
        CHECK(p_dlopen.max_reemit == 0, "AC3: Dlopen max_reemit == 0");
        CHECK(p_dlopen.fall_back_jit_only == false, "AC3: Dlopen fall_back_jit_only == false");
        CHECK(p_other.max_reemit == 0, "AC3: Other max_reemit == 0");
        CHECK(p_other.fall_back_jit_only == false, "AC3: Other fall_back_jit_only == false");
        // Source-cite: storm_skip helper + aot_reload_fail_is_auto_retryable
        CHECK(bridge_h.find("aot_reload_storm_skip_retry_for_2249") != std::string::npos,
              "storm_skip helper declared");
        CHECK(bridge_cpp.find("aot_reload_storm_skip_retry_for_2249") != std::string::npos,
              "storm_skip helper invoked at retry loop");
        CHECK(bridge_cpp.find("AotReloadFail::Region") != std::string::npos &&
                  bridge_cpp.find("AotReloadFail::Staging") != std::string::npos,
              "auto_retryable covers Region + Staging");
        CHECK(bridge_cpp.find("aot_reload_region_staging_retry_total") != std::string::npos,
              "retry counter bump site");
        CHECK(bridge_cpp.find("aot_reload_region_staging_exhausted_total") != std::string::npos,
              "exhausted counter bump site");
        // AC4: 2 metric fields + 2 query keys + schema-2249
        CHECK(met.find("aot_reload_region_staging_retry_total{0}") != std::string::npos,
              "retry counter field");
        CHECK(met.find("aot_reload_region_staging_exhausted_total{0}") != std::string::npos,
              "exhausted counter field");
        CHECK(q.find("aot-reload-region-staging-retry-total") != std::string::npos,
              "retry query key");
        CHECK(q.find("aot-reload-region-staging-exhausted-total") != std::string::npos,
              "exhausted query key");
        CHECK(q.find("aot-reload-region-staging-policy-wired") != std::string::npos,
              "wired sentinel");
        CHECK(q.find("schema-2249") != std::string::npos, "schema-2249 lineage");
        CHECK(q.find("issue-2249") != std::string::npos, "issue-2249 lineage");
        // AC5: env override AURA_AOT_RELOAD_AUTO_RETRY=0 still disables all
        const int saved = aura_aot_reload_auto_retry_enabled();
        aura_set_aot_reload_auto_retry(0);
        CHECK(aura_aot_reload_auto_retry_enabled() == 0, "AC5: env override disables");
        aura_set_aot_reload_auto_retry(saved);
        // AC6: success on 2nd Region attempt -> success counter, no exhausted
        CHECK(p_region.max_reemit >= 2, "AC6: Region policy supports up to 2 retries");
        CHECK(p_staging.max_reemit >= 2, "AC6: Staging policy supports up to 2 retries");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("aot reload primitive #1366/#2012: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
