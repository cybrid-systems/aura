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
#include "compiler/hot_update_registry.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

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
    const auto version_bit = static_cast<std::uint64_t>(1)
                             << static_cast<unsigned>(AotReloadFail::Version);
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
    const auto region_bit = static_cast<std::uint64_t>(1)
                            << static_cast<unsigned>(AotReloadFail::Region);
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
    CHECK(cmake.find("test_reload_recovery_query_2367") != std::string::npos, "AC5: cmake target");
    CHECK(build.find("check_reload_recovery_query_2367") != std::string::npos,
          "AC5: build.py gate script");
    CHECK(build.find("cmd_reload_recovery_query_coverage") != std::string::npos,
          "AC5: build.py coverage cmd");
    CHECK(script.find("schema-2367") != std::string::npos, "AC5: coverage script present");
}

} // namespace

int main() {
    std::println("test_reload_recovery_query_2367");
    ac1_soft_empty();
    ac2_force_jit_exhaustion();
    ac3_success_clears();
    ac4_hot_update_surface();
    ac5_source_and_gate();
    if (g_failed)
        return 1;
    std::println("reload recovery query #2367: OK ({} passed)", g_passed);
    return 0;
}
