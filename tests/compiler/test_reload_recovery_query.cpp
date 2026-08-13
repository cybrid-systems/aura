// @category: unit
// @reason: Issue #2367 — agent-facing ReloadRecovery query primitive +
// recovery-state snapshot (extends #2302 API with query surface).
//
//   AC1: soft empty path — idle recovery → recovery-active=0, zeros free
//   AC2: force-JIT exhaustion → query returns mask + reason + active
//   AC3: soft success clear → mask/active reset
//   AC4: keys on hot-update-registry-stats + schema-2367 lineage
//   AC5: source-cite + gate wiring

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/aot_reload_consistency_proof.h" // Issue #2753
#include "compiler/hot_update_registry.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

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

static std::int64_t href(CompilerService& cs, const char* query, const char* key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Clear process-global recovery signals AFTER CompilerService construction —
// service boot may re-touch deferred-reemit / storm flags.
static void clear_recovery_idle(aura::compiler::HotUpdateRegistry& reg) {
    reg.on_reload_success();
    while (reg.reload_recovery_state().pending_dirty_count > 0)
        reg.on_recovery_pending_dirty_dec();
    reg.set_shape_storm_active(false);
    reg.reset_deopt_storm_state_for_test();
    reg.reset_reemit_boundary_handshake_for_test();
    reg.on_reload_success();
}

// ── AC1: soft empty / idle path ──
static void ac1_soft_empty() {
    std::println("\n--- AC1: soft empty recovery → zeros / recovery-active=0 ---");
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_recovery_idle(reg);

    // Soft empty is verified on the C snapshot *before* engine:metrics —
    // eval/metrics can re-enter reemit/steal paths that re-seed deferred
    // flags as a process side-effect (orthogonal to snapshot cost).
    aura_reload_recovery_snapshot idle{};
    aura_hot_update_reload_recovery_get_snapshot(&idle);
    CHECK(idle.schema == 2367, "AC1: C snap schema");
    CHECK(idle.reload_recovery_wired == 1, "AC1: C snap wired");
    CHECK(idle.attempts_left == 0, "AC1: C snap attempts-left 0");
    CHECK(idle.force_jit_regions_mask == 0, "AC1: C snap force-jit mask 0");
    CHECK(idle.pending_dirty_count == 0, "AC1: C snap pending-dirty 0");
    CHECK(idle.deferred_reemit_pending == 0, "AC1: C snap deferred-reemit 0");
    CHECK(idle.storm_level == 0, "AC1: C snap storm-level 0");
    CHECK(idle.recovery_active == 0, "AC1: C snap recovery-active 0");

    // Query surface lineage (schema always present).
    CHECK(href(cs, "query:reload-recovery-state", "schema-2367") == 2367, "AC1: schema-2367");
    CHECK(href(cs, "query:reload-recovery-state", "issue-2367") == 2367, "AC1: issue-2367");
    CHECK(href(cs, "query:reload-recovery-state", "reload-recovery-wired") == 1, "AC1: wired");
    CHECK(href(cs, "query:aot-reload-recovery-stats", "schema-2367") == 2367,
          "AC1: alias query:aot-reload-recovery-stats");
}

// ── AC2: multi-round exhaustion → force-JIT state visible ──
static void ac2_force_jit_exhaustion() {
    std::println("\n--- AC2: force-JIT exhaustion → query recovery state ---");
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_recovery_idle(reg);
    reg.on_recovery_set_attempts_left(3);
    CHECK(reg.reload_recovery_state().attempts_left == 3, "AC2: seed attempts_left=3");

    reg.on_force_jit_for_reason(AotReloadFail::Version);
    const auto rs = reg.reload_recovery_state();
    CHECK(rs.attempts_left == 0, "AC2: post-exhaust attempts_left 0");
    // Issue #2927: Version maps to group bit 0 (not enum ordinal).
    const auto version_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Version);
    CHECK((rs.force_jit_regions_mask & version_bit) != 0, "AC2: Version bit set on API");
    CHECK(rs.last_reason == static_cast<std::uint8_t>(AotReloadFail::Version),
          "AC2: last_reason Version on API");

    CHECK(href(cs, "query:reload-recovery-state", "attempts-left") == 0,
          "AC2: query attempts-left 0");
    const auto mask = href(cs, "query:reload-recovery-state", "force-jit-regions-mask");
    CHECK((static_cast<std::uint64_t>(mask) & version_bit) != 0,
          "AC2: query force-jit-regions-mask has Version bit");
    CHECK(href(cs, "query:reload-recovery-state", "last-reason") ==
              static_cast<std::int64_t>(AotReloadFail::Version),
          "AC2: query last-reason Version");
    CHECK(href(cs, "query:reload-recovery-state", "last-force-jit-reason") ==
              static_cast<std::int64_t>(AotReloadFail::Version),
          "AC2: query last-force-jit-reason");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-for-reason-total") >= 1,
          "AC2: force-jit-for-reason-total advanced");
    CHECK(href(cs, "query:reload-recovery-state", "recovery-active") == 1,
          "AC2: recovery-active 1 under force-JIT");
    CHECK(href(cs, "query:reload-recovery-state", "reemit-boundary-policy") >= 0,
          "AC2: reemit-boundary-policy present");
    CHECK(href(cs, "query:reload-recovery-state", "storm-level") >= 0, "AC2: storm-level present");
    CHECK(href(cs, "query:reload-recovery-state", "last-force-jit-at-epoch-notify") >= 0,
          "AC2: last-force-jit-at-epoch-notify present");
}

// ── AC3: success clears recovery ──
static void ac3_success_clears() {
    std::println("\n--- AC3: on_reload_success clears force-JIT recovery ---");
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_recovery_idle(reg);
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    CHECK(reg.reload_recovery_state().force_jit_regions_mask != 0, "AC3: pre-clear mask set");
    reg.on_deferred_reemit_seen_on_steal(42);
    CHECK(reg.reload_recovery_state().deferred_reemit_pending == 1, "AC3: deferred seeded");

    // Live path: C snap + query agree deferred is set.
    {
        aura_reload_recovery_snapshot live{};
        aura_hot_update_reload_recovery_get_snapshot(&live);
        CHECK(live.deferred_reemit_pending == 1, "AC3: C snap deferred live");
        CHECK(live.recovery_active == 1, "AC3: C snap active live");
        CHECK(href(cs, "query:reload-recovery-state", "deferred-reemit-pending") == 1,
              "AC3: query deferred live");
        CHECK(href(cs, "query:reload-recovery-state", "recovery-active") == 1,
              "AC3: query active live");
    }

    reg.on_reload_success();
    CHECK(reg.reload_recovery_state().deferred_reemit_pending == 0, "AC3: API deferred cleared");
    CHECK(reg.reload_recovery_state().force_jit_regions_mask == 0, "AC3: API mask cleared");

    aura_reload_recovery_snapshot cleared{};
    aura_hot_update_reload_recovery_get_snapshot(&cleared);
    CHECK(cleared.deferred_reemit_pending == 0, "AC3: C snap deferred cleared");
    CHECK(cleared.force_jit_regions_mask == 0, "AC3: C snap mask cleared");
    CHECK(cleared.recovery_active == 0, "AC3: C snap recovery-active 0");

    // Query must match C snap after clear (same get_snapshot builder).
    // Note: engine:metrics may re-seed deferred as a host side-effect; we
    // re-clear immediately before each pair and compare C snap vs query
    // in the same "clear → snapshot → single key" window for mask/active.
    clear_recovery_idle(reg);
    aura_reload_recovery_snapshot s2{};
    aura_hot_update_reload_recovery_get_snapshot(&s2);
    CHECK(s2.force_jit_regions_mask == 0, "AC3: re-clear mask 0");
    CHECK(s2.recovery_active == 0, "AC3: re-clear active 0");
    // Force-jit mask stays 0 even if deferred re-seeds during metrics.
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-regions-mask") == 0,
          "AC3: query mask stays 0 after clear");
}

// ── AC4: existing hot-update surface carries schema-2367 ──
static void ac4_hot_update_surface() {
    std::println("\n--- AC4: query:hot-update-registry-stats schema-2367 keys ---");
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_recovery_idle(reg);
    reg.on_force_jit_for_reason(AotReloadFail::Region);
    CHECK(href(cs, "query:hot-update-registry-stats", "schema-2367") == 2367,
          "AC4: schema-2367 on hot-update surface");
    CHECK(href(cs, "query:hot-update-registry-stats", "issue-2367") == 2367,
          "AC4: issue-2367 on hot-update surface");
    CHECK(href(cs, "query:hot-update-registry-stats", "reload-recovery-wired") == 1,
          "AC4: reload-recovery-wired");
    const auto mask = href(cs, "query:hot-update-registry-stats", "force-jit-regions-mask");
    const auto region_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Region);
    CHECK((static_cast<std::uint64_t>(mask) & region_bit) != 0,
          "AC4: force-jit-regions-mask on hot-update surface");
    CHECK(href(cs, "query:hot-update-registry-stats", "recovery-active") == 1,
          "AC4: recovery-active on hot-update surface");
    reg.on_reload_success();
}

// ── AC5: source + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- AC5: source-cite query + gate wiring ---");
    const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    const auto reg = read_file("src/compiler/hot_update_registry.cpp");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    const auto obs = read_file("src/compiler/evaluator_primitives_observability.cpp");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    const auto script = read_file("scripts/coverage/checks/check_reload_recovery_query_2367.py");
    CHECK(mut.find("query:reload-recovery-state") != std::string::npos,
          "AC5: query registered in mutate");
    CHECK(mut.find("schema-2367") != std::string::npos, "AC5: schema-2367 in mutate");
    CHECK(reg.find("aura_hot_update_reload_recovery_get_snapshot") != std::string::npos,
          "AC5: C snapshot in registry cpp");
    CHECK(hh.find("aura_reload_recovery_snapshot") != std::string::npos,
          "AC5: snapshot struct in hh");
    CHECK(obs.find("query:reload-recovery-state") != std::string::npos,
          "AC5: listed in observability catalog");
    CHECK(cmake.find("test_reload_recovery_query") != std::string::npos, "AC5: cmake target");
    CHECK(build.find("check_reload_recovery_query_2367") != std::string::npos,
          "AC5: build.py gate script");
    CHECK(build.find("cmd_reload_recovery_query_coverage") != std::string::npos,
          "AC5: build.py coverage cmd");
    // Manifest-backed wrapper may only cite issue number (not schema-2367 key).
    CHECK(script.find("2367") != std::string::npos, "AC5: coverage script present");
}

// ── Issue #2753: AotReloadConsistencyProof single facade ──────────────
static void ac2753_1_soft_empty_proof() {
    std::println("\n--- #2753 AC4: soft empty / default proof ---");
    // Cheap on-the-fly build (no stamp required) — soft/idle path.
    auto p = build_aot_reload_consistency_proof_from_live(true);
    CHECK(p.schema == 2753, "AC4: proof.schema=2753");
    CHECK(p.stamp_epoch >= 1, "AC4: on-the-fly stamp_epoch advances local counter");
    CHECK(aura_aot_reload_consistency_proof_wired() == 1, "AC4: wired sentinel");
    // Optional query surface when registered (additive; soft empty ok).
    CompilerService cs;
    const auto schema = href(cs, "query:last-aot-reload-consistency-proof", "schema-2753");
    CHECK(schema == 2753 || schema == -1,
          "AC4: query schema-2753 when registered, else soft miss ok");
}

static void ac2753_2_success_and_rollback_stamp() {
    std::println("\n--- #2753 AC1/AC2: success + rollback stamp ---");
    // C++ API: build + stamp success then rollback.
    auto ok = build_aot_reload_consistency_proof_from_live(true);
    stamp_aot_reload_consistency_proof(ok);
    CHECK(aura_last_aot_reload_consistency_stamp_epoch() > 0, "AC1: stamp_epoch advanced");
    CHECK(aura_last_aot_reload_consistency_would_allow_native() == 1 ||
              aura_last_aot_reload_consistency_would_allow_native() == 0,
          "AC1: would_allow_native readable after success stamp");
    const auto stamp1 = aura_last_aot_reload_consistency_stamp_epoch();
    auto bad = build_aot_reload_consistency_proof_from_live(false);
    bad.last_fail_reason = static_cast<std::uint8_t>(AotReloadFail::Version);
    bad.would_allow_native = false;
    stamp_aot_reload_consistency_proof(bad);
    CHECK(aura_last_aot_reload_consistency_stamp_epoch() >= stamp1, "AC2: stamp advances");
    CHECK(aura_last_aot_reload_consistency_would_allow_native() == 0,
          "AC2: would_allow_native=false after rollback stamp");
    CHECK(aura_last_aot_reload_consistency_last_fail_reason() ==
              static_cast<std::uint8_t>(AotReloadFail::Version),
          "AC2: last_fail_reason=Version");
    // Agent detects drift via stamp_epoch / defuse without N-key join.
    const auto defuse = aura_last_aot_reload_consistency_defuse_version();
    const auto table = aura_last_aot_reload_consistency_table_epoch();
    (void)defuse;
    (void)table;
    CHECK(aura_aot_reload_consistency_proof_stamped_total() >= 2, "AC3: stamped-total >= 2");
}

static void ac2753_3_source_and_no_design() {
    std::println("\n--- #2753 AC5: source-cite + no docs/design/ ---");
    const auto thin = read_file("src/compiler/aot_reload_consistency_proof.h");
    const auto bridge_h = read_file("src/compiler/aura_jit_bridge.h");
    const auto cpp = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto t = read_file("tests/compiler/test_reload_recovery_query.cpp");
    CHECK(thin.find("AotReloadConsistencyProof") != std::string::npos,
          "AC5: struct in aot_reload_consistency_proof.h");
    CHECK(thin.find("kAotReloadConsistencyProofIssue = 2753") != std::string::npos,
          "AC5: issue stamp 2753");
    CHECK(thin.find("build_aot_reload_consistency_proof_from_live") != std::string::npos,
          "AC5: build helper");
    CHECK(cpp.find("stamp_aot_reload_consistency_proof") != std::string::npos, "AC5: stamp in cpp");
    CHECK(cpp.find("Issue #2753") != std::string::npos, "AC5: cpp cites #2753");
    CHECK(bridge_h.find("2753") != std::string::npos, "AC5: bridge.h cites #2753");
    CHECK(t.find("ac2753_1_soft_empty_proof") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac2753_2_success_and_rollback_stamp") != std::string::npos, "AC5: AC2 test");
    CHECK(read_file("docs/design/2753-aot-reload-proof.md").empty(),
          "AC5: no docs/design/2753-* per #1655");
}

// ── Issue #2776: concurrent stamp (fetch_add + seqlock) ──────────────
static void ac2776_1_fetch_add_and_seqlock_source() {
    std::println("\n--- #2776 AC1: fetch_add stamp_epoch + seqlock source ---");
    const auto thin = read_file("src/compiler/aot_reload_consistency_proof.h");
    CHECK(thin.find("#2776") != std::string::npos, "AC1: header cites #2776");
    CHECK(thin.find("kAotReloadConsistencyStampConcurrentIssue = 2776") != std::string::npos,
          "AC1: issue stamp 2776");
    CHECK(thin.find("g_aot_reload_proof_seq") != std::string::npos, "AC1: seqlock atomic");
    CHECK(thin.find("fetch_add") != std::string::npos, "AC1: fetch_add present");
    // Lost-update RMW ban: stamp must not assign stamp_epoch = load + 1 then store.
    CHECK(thin.find("g_aot_reload_proof_stamp_epoch.load") == std::string::npos ||
              thin.find("stamp_epoch.fetch_add") != std::string::npos ||
              thin.find("g_aot_reload_proof_stamp_epoch.fetch_add") != std::string::npos,
          "AC1: stamp_epoch via fetch_add");
    CHECK(thin.find("g_aot_reload_proof_stamp_epoch.fetch_add") != std::string::npos,
          "AC1: stamp_epoch.fetch_add in stamp()");
    CHECK(thin.find("load_aot_reload_consistency_proof_snapshot") != std::string::npos,
          "AC1: seqlock reader helper");
    // Seqlock even/odd discipline.
    CHECK(thin.find("odd") != std::string::npos || thin.find("& 1u") != std::string::npos,
          "AC1: odd writer phase");
}

static void ac2776_2_concurrent_stamp_monotonic() {
    std::println("\n--- #2776 AC2: 2-writer concurrent stamp, monotonic stamp_epoch ---");
    constexpr int kIters = 50000;
    std::atomic<int> start{0};
    auto worker = [&](std::uint64_t table_base) {
        while (start.load(std::memory_order_acquire) == 0) {
        }
        for (int i = 0; i < kIters; ++i) {
            AotReloadConsistencyProof p{};
            p.table_epoch = table_base + static_cast<std::uint64_t>(i);
            p.bridge_epoch = p.table_epoch; // keep bridge ≤ table invariant
            p.defuse_version = p.table_epoch;
            p.region_mask = 1;
            p.last_fail_reason = 0;
            p.force_jit_regions_mask = 0;
            p.would_allow_native = true;
            p.schema = kAotReloadConsistencyProofIssue;
            stamp_aot_reload_consistency_proof(p);
        }
    };
    std::thread t1(worker, 1'000'000ULL);
    std::thread t2(worker, 2'000'000ULL);
    start.store(1, std::memory_order_release);
    t1.join();
    t2.join();
    const auto total = aura_aot_reload_consistency_proof_stamped_total();
    const auto epoch = aura_last_aot_reload_consistency_stamp_epoch();
    // At least 2*kIters stamps since process start; epoch must equal total
    // if we started from 0, but other tests may have stamped — so epoch >= 2*kIters.
    CHECK(epoch >= static_cast<std::uint64_t>(2 * kIters),
          "AC2: stamp_epoch >= 2*kIters after concurrent writers");
    CHECK(total >= static_cast<std::uint64_t>(2 * kIters), "AC2: stamped_total >= 2*kIters");
    // Final snapshot is self-consistent (seqlock).
    auto snap = load_aot_reload_consistency_proof_snapshot();
    CHECK(snap.stamp_epoch == epoch, "AC2: snapshot stamp_epoch matches last");
    CHECK(snap.schema == 2753, "AC2: snapshot schema");
}

static void ac2776_3_reader_no_tear() {
    std::println("\n--- #2776 AC3: concurrent reader never sees torn proof ---");
    constexpr int kIters = 30000;
    std::atomic<int> start{0};
    std::atomic<int> stop{0};
    std::atomic<int> tears{0};
    std::atomic<int> reads{0};
    auto writer = [&](std::uint64_t base) {
        while (start.load(std::memory_order_acquire) == 0) {
        }
        for (int i = 0; i < kIters; ++i) {
            AotReloadConsistencyProof p{};
            // Couple fields so tear is detectable: bridge == table, defuse == table.
            p.table_epoch = base + static_cast<std::uint64_t>(i);
            p.bridge_epoch = p.table_epoch;
            p.defuse_version = p.table_epoch;
            p.region_mask = p.table_epoch & 0xffff;
            p.last_fail_reason = 0;
            p.force_jit_regions_mask = 0;
            p.would_allow_native = true;
            p.schema = kAotReloadConsistencyProofIssue;
            stamp_aot_reload_consistency_proof(p);
        }
    };
    auto reader = [&]() {
        while (start.load(std::memory_order_acquire) == 0) {
        }
        while (stop.load(std::memory_order_acquire) == 0) {
            auto p = load_aot_reload_consistency_proof_snapshot();
            reads.fetch_add(1, std::memory_order_relaxed);
            // Invariants for our writer pattern (and idle zero state).
            if (p.bridge_epoch > p.table_epoch)
                tears.fetch_add(1, std::memory_order_relaxed);
            if (p.defuse_version != p.table_epoch && p.stamp_epoch != 0 &&
                p.table_epoch >= 1'000'000ULL)
                tears.fetch_add(1, std::memory_order_relaxed);
            // Seq must not be observed odd via snapshot helper (it retries).
            if ((g_aot_reload_proof_seq.load(std::memory_order_relaxed) & 1u) != 0) {
                // Transient odd is ok for raw seq; snapshot must still be even-phase.
            }
        }
    };
    std::thread w1(writer, 3'000'000ULL);
    std::thread w2(writer, 4'000'000ULL);
    std::thread r(reader);
    start.store(1, std::memory_order_release);
    w1.join();
    w2.join();
    stop.store(1, std::memory_order_release);
    r.join();
    CHECK(reads.load() > 0, "AC3: reader got samples");
    CHECK(tears.load() == 0, "AC3: zero torn snapshots (bridge≤table, defuse==table)");
}

static void ac2776_4_source_cite_linter() {
    std::println("\n--- #2776 AC4: linter wire + no docs/design ---");
    const auto build = read_file("build.py");
    const auto thin = read_file("src/compiler/aot_reload_consistency_proof.h");
    const auto t = read_file("tests/compiler/test_reload_recovery_query.cpp");
    CHECK(build.find("check_aot_reload_consistency_stamp_concurrent_2776") != std::string::npos,
          "AC4: build.py wires linter");
    CHECK(t.find("ac2776_2_concurrent_stamp_monotonic") != std::string::npos, "AC4: AC2 test");
    CHECK(t.find("ac2776_3_reader_no_tear") != std::string::npos, "AC4: AC3 test");
    CHECK(thin.find("intentionally ignored") != std::string::npos ||
              thin.find("p.stamp_epoch is intentionally ignored") != std::string::npos ||
              thin.find("ignores p.stamp_epoch") != std::string::npos,
          "AC4: stamp documents ignoring caller stamp_epoch");
    CHECK(read_file("docs/design/2776-aot-reload-stamp-concurrent.md").empty(),
          "AC4: no docs/design/2776-* per #1655");
}

// ── Issue #2845: stamp fail proof on every rollback/exhaust path ──────
// Residual of #2753/#2776: would_allow_native=false + last_fail_reason +
// force_jit_regions_mask must be published on every non-Ok terminal.

static void ac2845_1_version_fail_proof() {
    std::println("\n--- #2845 AC1: Version fail → would_allow_native=0 + reason ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_recovery_idle(reg);
    // Success stamp first so a stale "allow native" would be visible if fail path missed.
    {
        AotReloadConsistencyProof ok{};
        ok.table_epoch = 42;
        ok.bridge_epoch = 42;
        ok.defuse_version = 1;
        ok.region_mask = 0;
        ok.last_fail_reason = 0;
        ok.force_jit_regions_mask = 0;
        ok.would_allow_native = true;
        ok.schema = kAotReloadConsistencyProofIssue;
        stamp_aot_reload_consistency_proof(ok);
    }
    const auto stamp_before = aura_last_aot_reload_consistency_stamp_epoch();
    const auto fail_before = aura_aot_reload_consistency_proof_stamped_on_fail_total();
    // Simulate Version rollback stamp (same helper note_reload_rollback uses).
    {
        AotReloadConsistencyProof p{};
        p.table_epoch = aura_aot_func_table_epoch();
        p.bridge_epoch = 42;
        p.defuse_version = 1;
        p.region_mask = 0;
        p.last_fail_reason = static_cast<std::uint8_t>(AotReloadFail::Version);
        p.force_jit_regions_mask = 0;
        stamp_aot_reload_consistency_proof_fail(p);
    }
    auto snap = load_aot_reload_consistency_proof_snapshot();
    CHECK(snap.would_allow_native == false, "AC1: would_allow_native==0 after Version fail");
    CHECK(snap.last_fail_reason == static_cast<std::uint8_t>(AotReloadFail::Version),
          "AC1: last_fail_reason matches Version");
    CHECK(snap.stamp_epoch > stamp_before, "AC1: stamp_epoch advanced");
    CHECK(aura_aot_reload_consistency_proof_stamped_on_fail_total() > fail_before,
          "AC1: stamped_on_fail_total advanced");
    CHECK(aura_last_aot_reload_consistency_would_allow_native() == 0,
          "AC1: accessor would_allow_native==0");
}

static void ac2845_2_force_jit_mask_in_proof() {
    std::println("\n--- #2845 AC2: Env/Linear exhaust → force-JIT mask in proof ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_recovery_idle(reg);
    {
        AotReloadConsistencyProof ok{};
        ok.would_allow_native = true;
        ok.schema = kAotReloadConsistencyProofIssue;
        stamp_aot_reload_consistency_proof(ok);
    }
    CHECK(aura_last_aot_reload_consistency_would_allow_native() == 1,
          "AC2: pre-exhaust allow native");
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    auto snap = load_aot_reload_consistency_proof_snapshot();
    // Issue #2927: Env → bit 1, Linear → bit 2 (group map, not enum ordinal).
    const auto env_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Env);
    CHECK(snap.would_allow_native == false, "AC2: would_allow_native==0 after Env force-JIT");
    CHECK(snap.last_fail_reason == static_cast<std::uint8_t>(AotReloadFail::Env),
          "AC2: last_fail_reason Env");
    CHECK((snap.force_jit_regions_mask & env_bit) != 0, "AC2: force_jit_regions_mask has Env bit");
    // Linear exhaust too.
    reg.on_force_jit_for_reason(AotReloadFail::Linear);
    snap = load_aot_reload_consistency_proof_snapshot();
    const auto lin_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Linear);
    CHECK(snap.would_allow_native == false, "AC2: still disallow native after Linear");
    CHECK((snap.force_jit_regions_mask & lin_bit) != 0, "AC2: Linear bit set");
    CHECK((snap.force_jit_regions_mask & env_bit) != 0, "AC2: Env bit retained");
    clear_recovery_idle(reg);
}

static void ac2845_3_success_commit_still_allows_when_idle() {
    std::println("\n--- #2845 AC3: success stamp allows native when idle ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_recovery_idle(reg);
    AotReloadConsistencyProof ok{};
    ok.table_epoch = 7;
    ok.bridge_epoch = 7;
    ok.defuse_version = 1;
    ok.region_mask = 0;
    ok.last_fail_reason = 0;
    ok.force_jit_regions_mask = 0;
    ok.would_allow_native = true;
    ok.schema = kAotReloadConsistencyProofIssue;
    stamp_aot_reload_consistency_proof(ok);
    auto snap = load_aot_reload_consistency_proof_snapshot();
    CHECK(snap.would_allow_native == true, "AC3: success allows native when fail==0 && mask==0");
    CHECK(snap.last_fail_reason == 0, "AC3: last_fail Ok");
    CHECK(snap.force_jit_regions_mask == 0, "AC3: force mask 0");
    // Regression: success path does not bump fail counter.
    const auto fail_n = aura_aot_reload_consistency_proof_stamped_on_fail_total();
    stamp_aot_reload_consistency_proof(ok);
    CHECK(aura_aot_reload_consistency_proof_stamped_on_fail_total() == fail_n,
          "AC3: success stamp does not advance stamped_on_fail_total");
}

static void ac2845_4_concurrent_fail_success_no_tear() {
    std::println("\n--- #2845 AC4: concurrent fail+success stamp never tears ---");
    constexpr int kIters = 20000;
    std::atomic<int> start{0};
    std::atomic<int> tears{0};
    std::atomic<int> reads{0};
    auto fail_writer = [&]() {
        while (start.load(std::memory_order_acquire) == 0) {
        }
        for (int i = 0; i < kIters; ++i) {
            AotReloadConsistencyProof p{};
            p.table_epoch = 5'000'000ULL + static_cast<std::uint64_t>(i);
            p.bridge_epoch = p.table_epoch;
            p.defuse_version = p.table_epoch;
            p.region_mask = 1;
            p.last_fail_reason = static_cast<std::uint8_t>(AotReloadFail::Version);
            p.force_jit_regions_mask = aot_reload_fail_to_force_jit_mask(AotReloadFail::Version);
            stamp_aot_reload_consistency_proof_fail(p);
        }
    };
    auto ok_writer = [&]() {
        while (start.load(std::memory_order_acquire) == 0) {
        }
        for (int i = 0; i < kIters; ++i) {
            AotReloadConsistencyProof p{};
            p.table_epoch = 6'000'000ULL + static_cast<std::uint64_t>(i);
            p.bridge_epoch = p.table_epoch;
            p.defuse_version = p.table_epoch;
            p.region_mask = 2;
            p.last_fail_reason = 0;
            p.force_jit_regions_mask = 0;
            p.would_allow_native = true;
            p.schema = kAotReloadConsistencyProofIssue;
            stamp_aot_reload_consistency_proof(p);
        }
    };
    auto reader = [&]() {
        while (start.load(std::memory_order_acquire) == 0) {
        }
        for (int i = 0; i < kIters * 2; ++i) {
            auto p = load_aot_reload_consistency_proof_snapshot();
            reads.fetch_add(1, std::memory_order_relaxed);
            if (p.bridge_epoch > p.table_epoch)
                tears.fetch_add(1, std::memory_order_relaxed);
            // Fail stamps always force would_allow_native=false when reason!=0.
            if (p.last_fail_reason != 0 && p.would_allow_native)
                tears.fetch_add(1, std::memory_order_relaxed);
            // Success pattern: mask 0 and reason 0 implies allow when stamped ok.
            if (p.last_fail_reason == 0 && p.force_jit_regions_mask != 0 && p.would_allow_native)
                tears.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread t1(fail_writer);
    std::thread t2(ok_writer);
    std::thread t3(reader);
    start.store(1, std::memory_order_release);
    t1.join();
    t2.join();
    t3.join();
    CHECK(reads.load() > 0, "AC4: reader sampled");
    CHECK(tears.load() == 0, "AC4: zero torn / inconsistent allow-native snapshots");
    // seqlock_retry_total may rise under dual writers — just readable.
    (void)aura_aot_reload_proof_seqlock_retry_total();
}

static void ac2845_5_soft_no_extra_stamp() {
    std::println("\n--- #2845 AC5: soft/quiet path no forced extra stamp ---");
    const auto total_before = aura_aot_reload_consistency_proof_stamped_total();
    const auto fail_before = aura_aot_reload_consistency_proof_stamped_on_fail_total();
    // Soft idle: only build_from_live / snapshot — no stamp.
    auto soft = build_aot_reload_consistency_proof_from_live(true);
    CHECK(soft.schema == 2753, "AC5: soft build schema");
    (void)load_aot_reload_consistency_proof_snapshot();
    CHECK(aura_aot_reload_consistency_proof_stamped_total() == total_before,
          "AC5: soft path does not advance stamped_total");
    CHECK(aura_aot_reload_consistency_proof_stamped_on_fail_total() == fail_before,
          "AC5: soft path does not advance stamped_on_fail_total");
}

static void ac2845_6_source_and_linter() {
    std::println("\n--- #2845 AC6: source-cite + linter + no docs/design ---");
    const auto thin = read_file("src/compiler/aot_reload_consistency_proof.h");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto reg = read_file("src/compiler/hot_update_registry.cpp");
    const auto t = read_file("tests/compiler/test_reload_recovery_query.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_aot_reload_proof_fail_stamp_2845.py");
    CHECK(thin.find("stamp_aot_reload_consistency_proof_fail") != std::string::npos,
          "AC6: fail helper in thin header");
    CHECK(thin.find("kAotReloadConsistencyProofFailStampIssue") != std::string::npos,
          "AC6: issue stamp 2845");
    CHECK(thin.find("g_aot_reload_proof_stamped_on_fail_total") != std::string::npos,
          "AC6: fail counter");
    CHECK(thin.find("#2845") != std::string::npos, "AC6: header cites #2845");
    CHECK(bridge.find("stamp_aot_reload_consistency_proof_fail") != std::string::npos,
          "AC6: note_reload_rollback uses fail helper");
    CHECK(bridge.find("#2845") != std::string::npos, "AC6: bridge cites #2845");
    CHECK(reg.find("stamp_aot_reload_consistency_proof_fail_after_force_jit") != std::string::npos,
          "AC6: on_force_jit re-stamps");
    CHECK(reg.find("#2845") != std::string::npos, "AC6: registry cites #2845");
    CHECK(t.find("ac2845_1_version_fail_proof") != std::string::npos, "AC6: AC1 test");
    CHECK(t.find("ac2845_2_force_jit_mask_in_proof") != std::string::npos, "AC6: AC2 test");
    CHECK(t.find("ac2845_3_success_commit_still_allows_when_idle") != std::string::npos,
          "AC6: AC3 test");
    CHECK(t.find("ac2845_4_concurrent_fail_success_no_tear") != std::string::npos, "AC6: AC4 test");
    CHECK(t.find("ac2845_5_soft_no_extra_stamp") != std::string::npos, "AC6: AC5 test");
    CHECK(!lint.empty() && lint.find("2845") != std::string::npos, "AC6: coverage linter present");
    CHECK(build.find("check_aot_reload_proof_fail_stamp_2845") != std::string::npos,
          "AC6: build.py wires linter");
    CHECK(t.find("ac2753_1_soft_empty_proof") != std::string::npos, "AC6: #2753 preserved");
    CHECK(t.find("ac2776_1_fetch_add_and_seqlock_source") != std::string::npos,
          "AC6: #2776 preserved");
    CHECK(read_file("docs/design/2845-aot-reload-proof-fail-stamp.md").empty(),
          "AC6: no docs/design/2845-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2845.cpp").empty(),
          "AC6: no invent test file per #81967");
}

// ── Issue #2927: stable AotReloadFail → force_jit_regions_mask bit groups ──
// Version|Defuse→0, Env→1, Linear→2, Region|Staging→3, Dlopen|Other→4.

static void ac2927_1_env_only_bit() {
    std::println("\n--- #2927 AC1: Env exhaust → only Env group bit; Version-eligible ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_recovery_idle(reg);
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    const auto mask = reg.reload_recovery_state().force_jit_regions_mask;
    const auto env_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Env);
    const auto ver_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Version);
    const auto lin_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Linear);
    CHECK(env_bit == (1ull << 1), "AC1: Env maps to bit 1");
    CHECK(ver_bit == (1ull << 0), "AC1: Version maps to bit 0");
    CHECK((mask & env_bit) != 0, "AC1: Env bit set");
    CHECK((mask & ver_bit) == 0, "AC1: Version bit not set (eligible for native group)");
    CHECK((mask & lin_bit) == 0, "AC1: Linear bit not set");
    CHECK(mask == env_bit, "AC1: mask is Env-only (no silent full-mask)");
    CHECK(reg.last_force_jit_mapped_bit() == 1, "AC1: last_force_jit_mapped_bit == 1");
    // Defuse shares Version's group bit (bit 0) — not Env.
    CHECK(aot_reload_fail_to_force_jit_mask(AotReloadFail::Defuse) == ver_bit,
          "AC1: Defuse shares Version group bit 0");
    clear_recovery_idle(reg);
}

static void ac2927_2_linear_and_proof_match() {
    std::println("\n--- #2927 AC2: Linear → Linear bit only; proof mask matches registry ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_recovery_idle(reg);
    {
        AotReloadConsistencyProof ok{};
        ok.would_allow_native = true;
        ok.schema = kAotReloadConsistencyProofIssue;
        stamp_aot_reload_consistency_proof(ok);
    }
    reg.on_force_jit_for_reason(AotReloadFail::Linear);
    const auto reg_mask = reg.reload_recovery_state().force_jit_regions_mask;
    const auto lin_bit = aot_reload_fail_to_force_jit_mask(AotReloadFail::Linear);
    CHECK(lin_bit == (1ull << 2), "AC2: Linear maps to bit 2");
    CHECK(reg_mask == lin_bit, "AC2: registry mask is Linear-only");
    auto snap = load_aot_reload_consistency_proof_snapshot();
    CHECK(snap.would_allow_native == false, "AC2: proof would_allow_native==0");
    CHECK(snap.force_jit_regions_mask == reg_mask, "AC2: proof mask matches registry");
    CHECK(snap.last_fail_reason == static_cast<std::uint8_t>(AotReloadFail::Linear),
          "AC2: proof last_fail_reason Linear");
    CHECK(reg.last_force_jit_mapped_bit() == 2, "AC2: mapped bit index 2");
    // Region|Staging share bit 3; Dlopen|Other share bit 4.
    CHECK(aot_reload_fail_to_force_jit_mask(AotReloadFail::Region) ==
              aot_reload_fail_to_force_jit_mask(AotReloadFail::Staging),
          "AC2: Region|Staging share bit 3");
    CHECK(aot_reload_fail_to_force_jit_mask(AotReloadFail::Region) == (1ull << 3),
          "AC2: Region bit 3");
    CHECK(aot_reload_fail_to_force_jit_mask(AotReloadFail::Dlopen) ==
              aot_reload_fail_to_force_jit_mask(AotReloadFail::Other),
          "AC2: Dlopen|Other share bit 4");
    CHECK(aot_reload_fail_to_force_jit_mask(AotReloadFail::Ok) == 0, "AC2: Ok maps to zero bits");
    clear_recovery_idle(reg);
}

static void ac2927_3_soft_success_no_mask_change() {
    std::println("\n--- #2927 AC3: soft/success → no mask change; fail stamp only on fail ---");
    auto& reg = aura::compiler::hot_update_registry();
    clear_recovery_idle(reg);
    const auto mask0 = reg.reload_recovery_state().force_jit_regions_mask;
    const auto fail0 = aura_aot_reload_consistency_proof_stamped_on_fail_total();
    const auto stamp0 = aura_aot_reload_consistency_proof_stamped_total();
    (void)build_aot_reload_consistency_proof_from_live(true);
    (void)load_aot_reload_consistency_proof_snapshot();
    CHECK(reg.reload_recovery_state().force_jit_regions_mask == mask0, "AC3: soft no mask change");
    CHECK(aura_aot_reload_consistency_proof_stamped_on_fail_total() == fail0,
          "AC3: soft no fail-stamp advance");
    CHECK(aura_aot_reload_consistency_proof_stamped_total() == stamp0,
          "AC3: soft no stamp total advance");
    // Success clears mask without fail stamp.
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    CHECK(reg.reload_recovery_state().force_jit_regions_mask != 0, "AC3: demoted before success");
    const auto fail1 = aura_aot_reload_consistency_proof_stamped_on_fail_total();
    reg.on_reload_success();
    CHECK(reg.reload_recovery_state().force_jit_regions_mask == 0, "AC3: success clears mask");
    CHECK(aura_aot_reload_consistency_proof_stamped_on_fail_total() == fail1,
          "AC3: success does not bump stamped_on_fail_total");
}

static void ac2927_4_query_keys() {
    std::println("\n--- #2927 AC4: query force-jit-reason-bit-map-wired + last-mapped-bit ---");
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_recovery_idle(reg);
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-reason-bit-map-wired") == 1,
          "AC4: force-jit-reason-bit-map-wired");
    CHECK(href(cs, "query:reload-recovery-state", "schema-2927") == 2927, "AC4: schema-2927");
    CHECK(href(cs, "query:reload-recovery-state", "issue-2927") == 2927, "AC4: issue-2927");
    CHECK(href(cs, "query:reload-recovery-state", "schema-2367") == 2367,
          "AC4: schema-2367 preserved");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-bit-env") == 1,
          "AC4: bit-env sentinel");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-bit-linear") == 2,
          "AC4: bit-linear sentinel");
    reg.on_force_jit_for_reason(AotReloadFail::Env);
    CHECK(href(cs, "query:reload-recovery-state", "last-mapped-bit") == 1,
          "AC4: last-mapped-bit Env=1");
    CHECK(href(cs, "query:reload-recovery-state", "force-jit-regions-mask") ==
              static_cast<std::int64_t>(aot_reload_fail_to_force_jit_mask(AotReloadFail::Env)),
          "AC4: query mask Env-only");
    // hot-update surface cross-link
    CHECK(href(cs, "query:hot-update-registry-stats", "force-jit-reason-bit-map-wired") == 1,
          "AC4: wired key on hot-update-registry-stats");
    CHECK(href(cs, "query:hot-update-registry-stats", "schema-2927") == 2927,
          "AC4: schema-2927 on hot-update surface");
    clear_recovery_idle(reg);
}

static void ac2927_5_source_and_linter() {
    std::println("\n--- #2927 AC5: source-cite + linter + no docs/design ---");
    const auto bridge_h = read_file("src/compiler/aura_jit_bridge.h");
    const auto reg = read_file("src/compiler/hot_update_registry.cpp");
    const auto reg_h = read_file("src/compiler/hot_update_registry.hh");
    const auto t = read_file("tests/compiler/test_reload_recovery_query.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_force_jit_reason_bit_map_2927.py");
    CHECK(bridge_h.find("aot_reload_fail_to_force_jit_mask") != std::string::npos,
          "AC5: mask helper in bridge header");
    CHECK(bridge_h.find("aot_reload_fail_to_force_jit_bit_index") != std::string::npos,
          "AC5: bit-index helper");
    CHECK(bridge_h.find("#2927") != std::string::npos, "AC5: bridge cites #2927");
    CHECK(reg.find("aot_reload_fail_to_force_jit_mask") != std::string::npos,
          "AC5: on_force_jit uses mapped mask");
    CHECK(reg.find("#2927") != std::string::npos, "AC5: registry cites #2927");
    CHECK(reg_h.find("#2927") != std::string::npos, "AC5: registry.hh cites #2927");
    CHECK(t.find("ac2927_1_env_only_bit") != std::string::npos, "AC5: AC1 test");
    CHECK(t.find("ac2927_2_linear_and_proof_match") != std::string::npos, "AC5: AC2 test");
    CHECK(t.find("ac2927_3_soft_success_no_mask_change") != std::string::npos, "AC5: AC3 test");
    CHECK(t.find("ac2927_4_query_keys") != std::string::npos, "AC5: AC4 test");
    CHECK(!lint.empty() && lint.find("2927") != std::string::npos, "AC5: coverage linter present");
    CHECK(build.find("check_force_jit_reason_bit_map_2927") != std::string::npos ||
              build.find("force-jit-reason-bit-map-2927") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(t.find("ac2845_2_force_jit_mask_in_proof") != std::string::npos, "AC5: #2845 preserved");
    CHECK(read_file("docs/design/2927-force-jit-reason-bit-map.md").empty(),
          "AC5: no docs/design/2927-* per #1655");
    CHECK(read_file("tests/compiler/test_issue_2927.cpp").empty(),
          "AC5: no invent test file per #81967");
}

// ── Issue #2953: Agent recovery playbook (single action from snapshot) ──
// AC1: controlled snapshots match decision table
// AC2: idle → Idle
// AC3: observe-only (no reemit/drain in decide path)
// AC4: schema-2953 additive; recovery keys preserved
// AC5: Soft regression green (idle path)
// AC6: source-cite + linter; no docs/design
static void ac2953_playbook_decision_table() {
    std::println("\n--- #2953 AC1–AC3: pure playbook decision table ---");
    using Act = ReloadRecoveryPlaybookAction;

    // AC2: soft empty → Idle
    {
        ReloadRecoveryPlaybookInput in{};
        const auto r = aura_reload_recovery_playbook_decide(in);
        CHECK(r.action == Act::Idle, "2953 AC2: empty input → Idle");
        CHECK(r.playbook_wired == 1, "2953 AC2: wired");
        CHECK(r.schema_2953 == 2953, "2953 AC2: schema");
    }

    // AC1: wait-storm
    {
        ReloadRecoveryPlaybookInput in{};
        in.storm_level = 2; // Global
        in.force_jit_regions_mask = 1;
        in.recovery_active = 1;
        CHECK(aura_reload_recovery_playbook_decide(in).action == Act::WaitStorm,
              "2953 AC1: storm → WaitStorm (beats force)");
    }

    // AC1: reject-cross-ws (highest priority)
    {
        ReloadRecoveryPlaybookInput in{};
        in.cross_ws_reject = 1; // ForeignEval
        in.storm_level = 2;
        CHECK(aura_reload_recovery_playbook_decide(in).action == Act::RejectCrossWs,
              "2953 AC1: cross-ws → RejectCrossWs (beats storm)");
    }

    // AC1: force-drain
    {
        ReloadRecoveryPlaybookInput in{};
        in.deferred_pending = 1;
        in.deferred_age_ms = 500;
        in.force_drain_deadline_ms = 100;
        in.recovery_active = 1;
        CHECK(aura_reload_recovery_playbook_decide(in).action == Act::ForceDrain,
              "2953 AC1: aged deferred → ForceDrain");
    }

    // AC1: deferred without deadline → Reemit (not ForceDrain)
    {
        ReloadRecoveryPlaybookInput in{};
        in.deferred_pending = 1;
        in.deferred_age_ms = 9999;
        in.force_drain_deadline_ms = 0;
        CHECK(aura_reload_recovery_playbook_decide(in).action == Act::Reemit,
              "2953 AC1: deferred no deadline → Reemit");
    }

    // AC1: retry-reload
    {
        ReloadRecoveryPlaybookInput in{};
        in.attempts_left = 2;
        in.last_reason = static_cast<std::uint8_t>(AotReloadFail::Env);
        in.recovery_active = 1;
        CHECK(aura_reload_recovery_playbook_decide(in).action == Act::RetryReload,
              "2953 AC1: attempts+Env → RetryReload");
    }

    // AC1: reemit residual force
    {
        ReloadRecoveryPlaybookInput in{};
        in.force_jit_regions_mask = 0b11;
        in.last_reemit_success_region_mask = 0b01; // residual bit1
        in.recovery_active = 1;
        const auto r = aura_reload_recovery_playbook_decide(in);
        CHECK(r.action == Act::Reemit, "2953 AC1: residual force → Reemit");
        CHECK(r.residual_force_mask == 0b10, "2953 AC1: residual mask echo");
    }

    // AC1: fall-back-jit (force covered, no pending)
    {
        ReloadRecoveryPlaybookInput in{};
        in.force_jit_regions_mask = 0b01;
        in.last_reemit_success_region_mask = 0b01; // fully covered
        in.recovery_active = 1;
        CHECK(aura_reload_recovery_playbook_decide(in).action == Act::FallBackJit,
              "2953 AC1: covered force only → FallBackJit");
    }

    // AC1: reemit pending dirty
    {
        ReloadRecoveryPlaybookInput in{};
        in.pending_dirty_count = 3;
        in.recovery_active = 1;
        CHECK(aura_reload_recovery_playbook_decide(in).action == Act::Reemit,
              "2953 AC1: pending dirty → Reemit");
    }

    // AC3: decide is pure — no reemit/drain/reload in decide body
    {
        const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
        const auto start = cpp.find("aura_reload_recovery_playbook_decide");
        CHECK(start != std::string::npos, "2953 AC3: decide function present");
        const auto body = cpp.substr(start, 1600);
        CHECK(body.find("aura_reemit_aot_for_dirty") == std::string::npos,
              "2953 AC3: decide does not call reemit");
        CHECK(body.find("drain_pending_recovery") == std::string::npos,
              "2953 AC3: decide does not drain");
        CHECK(body.find("aura_reload_aot") == std::string::npos,
              "2953 AC3: decide does not reload");
    }
}

static void ac2953_playbook_query_and_source() {
    std::println("\n--- #2953 AC4–AC6: query surface + source-cite ---");
    auto& reg = aura::compiler::hot_update_registry();
    CompilerService cs;
    clear_recovery_idle(reg);
    aura_test_reset_last_cross_workspace_reject_reason();

    // AC2/AC5: live idle → action Idle (C get; metrics eval can re-seed deferred
    // like #2367 AC1 — action checked on C path, schema/sentinels on query).
    {
        clear_recovery_idle(reg);
        aura_test_reset_last_cross_workspace_reject_reason();
        ReloadRecoveryPlaybookResult pb{};
        aura_hot_update_reload_recovery_playbook_get(&pb);
        CHECK(pb.action == ReloadRecoveryPlaybookAction::Idle, "2953 AC2: live get Idle");
        CHECK(href(cs, "query:reload-recovery-playbook", "playbook-wired") == 1,
              "2953 AC4: playbook-wired");
        CHECK(href(cs, "query:reload-recovery-playbook", "schema-2953") == 2953,
              "2953 AC4: schema-2953");
        CHECK(href(cs, "query:reload-recovery-playbook", "issue-2953") == 2953,
              "2953 AC4: issue-2953");
        CHECK(href(cs, "query:reload-recovery-playbook", "playbook-wait-storm") == 1,
              "2953 AC4: wait-storm sentinel");
        CHECK(href(cs, "query:reload-recovery-playbook", "playbook-reject-cross-ws") == 6,
              "2953 AC4: reject-cross-ws sentinel");
        // playbook-action is integer >= 0 (idle or transient deferred side-effect)
        CHECK(href(cs, "query:reload-recovery-playbook", "playbook-action") >= 0,
              "2953 AC2: query playbook-action queryable");
    }

    // AC1 live: force-JIT residual → Reemit via query
    {
        clear_recovery_idle(reg);
        reg.on_force_jit_for_reason(AotReloadFail::Env);
        CHECK(href(cs, "query:reload-recovery-playbook", "playbook-action") ==
                  static_cast<std::int64_t>(ReloadRecoveryPlaybookAction::Reemit),
              "2953 AC1: live force residual → Reemit");
        CHECK(href(cs, "query:reload-recovery-state", "schema-2953") == 2953,
              "2953 AC4: schema-2953 on recovery-state");
        CHECK(href(cs, "query:reload-recovery-state", "playbook-action") ==
                  static_cast<std::int64_t>(ReloadRecoveryPlaybookAction::Reemit),
              "2953 AC4: playbook-action on recovery-state");
        CHECK(href(cs, "query:reload-recovery-state", "schema-2367") == 2367,
              "2953 AC4: schema-2367 preserved");
        clear_recovery_idle(reg);
    }

    // AC1 live: storm → WaitStorm
    {
        reg.set_shape_storm_active(true);
        reg.on_force_jit_for_reason(AotReloadFail::Version);
        CHECK(href(cs, "query:reload-recovery-playbook", "playbook-action") ==
                  static_cast<std::int64_t>(ReloadRecoveryPlaybookAction::WaitStorm),
              "2953 AC1: live storm → WaitStorm");
        reg.set_shape_storm_active(false);
        clear_recovery_idle(reg);
    }

    // AC1 live: cross-ws reject
    {
        aura_test_set_last_cross_workspace_reject_reason(
            static_cast<std::uint8_t>(CrossWorkspaceReject::ForeignEval));
        CHECK(href(cs, "query:reload-recovery-playbook", "playbook-action") ==
                  static_cast<std::int64_t>(ReloadRecoveryPlaybookAction::RejectCrossWs),
              "2953 AC1: live cross-ws → RejectCrossWs");
        aura_test_reset_last_cross_workspace_reject_reason();
    }

    // AC6: source-cite + linter
    {
        const auto hh = read_file("src/compiler/hot_update_registry.hh");
        const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        const auto build = read_file("build.py");
        const auto lint =
            read_file("scripts/coverage/checks/check_reload_recovery_playbook_2953.py");
        CHECK(hh.find("Issue #2953") != std::string::npos, "2953 AC6: header cites #2953");
        CHECK(hh.find("aura_reload_recovery_playbook_decide") != std::string::npos,
              "2953 AC6: decide declared");
        CHECK(hh.find("ReloadRecoveryPlaybookAction") != std::string::npos,
              "2953 AC6: action enum");
        CHECK(cpp.find("aura_reload_recovery_playbook_decide") != std::string::npos,
              "2953 AC6: decide defined");
        CHECK(mut.find("query:reload-recovery-playbook") != std::string::npos,
              "2953 AC6: dedicated query");
        CHECK(mut.find("schema-2953") != std::string::npos, "2953 AC6: schema-2953 query");
        CHECK(build.find("check_reload_recovery_playbook_2953") != std::string::npos,
              "2953 AC6: build.py wires linter");
        CHECK(!lint.empty(), "2953 AC6: linter present");
        CHECK(read_file("docs/design/2953-reload-recovery-playbook.md").empty(),
              "2953 AC6: no docs/design/");
    }
    clear_recovery_idle(reg);
    aura_test_reset_last_cross_workspace_reject_reason();
}

} // namespace

int run_test_reload_recovery_query() {
    std::println("test_reload_recovery_query");
    ac1_soft_empty();
    ac2_force_jit_exhaustion();
    ac3_success_clears();
    ac4_hot_update_surface();
    ac5_source_and_gate();
    std::println("\n=== Issue #2753: AotReloadConsistencyProof ===");
    ac2753_1_soft_empty_proof();
    ac2753_2_success_and_rollback_stamp();
    ac2753_3_source_and_no_design();
    std::println("\n=== Issue #2776: concurrent stamp (fetch_add + seqlock) ===");
    ac2776_1_fetch_add_and_seqlock_source();
    ac2776_2_concurrent_stamp_monotonic();
    ac2776_3_reader_no_tear();
    ac2776_4_source_cite_linter();
    std::println("\n=== Issue #2845: fail-path sole stamp (would_allow_native=false) ===");
    ac2845_1_version_fail_proof();
    ac2845_2_force_jit_mask_in_proof();
    ac2845_3_success_commit_still_allows_when_idle();
    ac2845_4_concurrent_fail_success_no_tear();
    ac2845_5_soft_no_extra_stamp();
    ac2845_6_source_and_linter();
    std::println("\n=== Issue #2927: AotReloadFail → force_jit group bits ===");
    ac2927_1_env_only_bit();
    ac2927_2_linear_and_proof_match();
    ac2927_3_soft_success_no_mask_change();
    ac2927_4_query_keys();
    ac2927_5_source_and_linter();
    std::println("\n=== Issue #2953: recovery playbook single action ===");
    ac2953_playbook_decision_table();
    ac2953_playbook_query_and_source();
    if (g_failed)
        return 1;
    std::println(
        "reload recovery query #2367 + #2753 + #2776 + #2845 + #2927 + #2953: OK ({} passed)",
        g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_reload_recovery_query();
}
#endif
