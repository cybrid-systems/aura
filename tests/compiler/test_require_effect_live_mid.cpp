// @category: unit
// @reason: Issue #2384 — require_effect stamps live mutation_id (not 0)
// so bound grants fire provenance_ok and SecurityEvent joins by mid.
//
//   AC1: Grant Mutate bound_mutation_id=M; require_effect outside → deny
//        + capability_provenance_mismatch_total bumps
//   AC2: Same grant under mid=M → allow
//   AC3: Soft / sandbox off still allows; mid still non-zero when recorded
//   AC4: SecurityEvent on require_effect path has mutation_id != 0
//   AC5: Source-cite + tests + gate registration

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::kEffectFfi;
using aura::compiler::security::kEffectMutate;
using aura::core::bump_mutation_epoch;
using aura::core::current_mutation_epoch;
using aura::core::capability::CapabilityGrant;
using aura::core::capability::g_capability_effect_metrics;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::security_event::g_security_event_ring;
using aura::core::security_event::reset_security_event_ring_for_test;
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

static void reset_all() {
    reset_capability_effects_for_test();
    reset_security_event_ring_for_test();
}

static std::uint64_t last_security_event_mid() {
    const auto& ring = g_security_event_ring();
    const auto seq = ring.seq.load(std::memory_order_relaxed);
    if (seq == 0)
        return 0;
    return ring.ring[(seq - 1) % ring.ring.size()].mutation_id;
}

// AC1: bound mid M, require_effect with live mid != M → deny + mismatch.
static void ac1_bound_mismatch_denies() {
    std::println("\n--- #2384 AC1: bound mid mismatch denies require_effect ---");
    reset_all();
    bump_mutation_epoch(3);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2); // Strict
    ev.set_capability_tenant_id(42);
    constexpr std::uint64_t kBoundMid = 9001;
    // Explicit bound mid (not current epoch) so live stamp diverges.
    ev.grant_effect_capability(42, "mutate-2384", kEffectMutate, kBoundMid);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(42, "mutate-2384", g), "grant installed");
    CHECK(g.bound_mutation_id == kBoundMid, "AC1: grant bound_mutation_id = M");

    const auto mismatch0 =
        g_capability_effect_metrics().capability_provenance_mismatch_total.load();
    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:ac1-mismatch", 0);
    const auto mismatch1 =
        g_capability_effect_metrics().capability_provenance_mismatch_total.load();
    std::println("  require_effect={} live_epoch={} bound={} mismatch {}→{}", ok,
                 current_mutation_epoch(), kBoundMid, mismatch0, mismatch1);
    CHECK(!ok, "AC1: require_effect denies when live mid != bound M");
    CHECK(mismatch1 > mismatch0, "AC1: capability_provenance_mismatch_total bumps");
}

// AC2: grant bound to live epoch; require_effect under same mid → allow.
static void ac2_bound_match_allows() {
    std::println("\n--- #2384 AC2: bound mid match allows require_effect ---");
    reset_all();
    bump_mutation_epoch(2);
    const auto me = current_mutation_epoch();
    CHECK(me != 0, "mutation epoch non-zero");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    ev.set_capability_tenant_id(43);
    // Bind grant to current epoch (same stamp require_effect will use).
    ev.grant_effect_capability(43, "mutate-match", kEffectMutate, me);
    CapabilityGrant g{};
    CHECK(g_capability_registry().find_grant(43, "mutate-match", g), "grant");
    CHECK(g.bound_mutation_id == me, "AC2: bound = live epoch");

    const bool ok =
        ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "test:ac2-match", 0);
    CHECK(ok, "AC2: require_effect allows under matching live mid");
}

// AC3: sandbox off still allows without grant; mid non-zero in SecurityEvent.
static void ac3_soft_off_allows_nonzero_mid() {
    std::println("\n--- #2384 AC3: Off sandbox allows; mid non-zero ---");
    reset_all();
    bump_mutation_epoch(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off
    // No grant — Off path still allows.
    const bool ok = ev.require_effect(static_cast<std::uint16_t>(kEffectFfi), "test:ac3-off", 0);
    CHECK(ok, "AC3: Off sandbox allows require_effect without grant");
    const auto mid = last_security_event_mid();
    std::println("  SecurityEvent.mutation_id={}", mid);
    CHECK(mid != 0, "AC3: SecurityEvent mid non-zero under Off");
}

// AC4: SecurityEvent on require_effect always has mutation_id != 0.
static void ac4_security_event_mid() {
    std::println("\n--- #2384 AC4: SecurityEvent mutation_id != 0 ---");
    reset_all();
    bump_mutation_epoch(1);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(44);
    const auto me = current_mutation_epoch();
    ev.grant_effect_capability(44, "ffi-2384", kEffectFfi, me == 0 ? 1 : me);
    CHECK(ev.require_effect(static_cast<std::uint16_t>(kEffectFfi), "test:ac4-se", 0),
          "effect allowed");
    const auto mid = last_security_event_mid();
    std::println("  SecurityEvent.mutation_id={}", mid);
    CHECK(mid != 0, "AC4: SecurityEvent mutation_id != 0 (not seq-only fallback when epoch avail)");
}

// AC5: source-cite + registration.
static void ac5_source_and_gate() {
    std::println("\n--- #2384 AC5: source-cite + gate ---");
    const auto sec = read_file("src/compiler/evaluator_security.cpp");
    CHECK(!sec.empty(), "evaluator_security.cpp readable");
    CHECK(sec.find("Issue #2384") != std::string::npos, "AC5: cites #2384");
    CHECK(sec.find("require_effect") != std::string::npos, "AC5: require_effect present");
    CHECK(sec.find("current_mutation_epoch()") != std::string::npos,
          "AC5: stamps current_mutation_epoch");
    // Must not hardcode 0 as the only provenance path.
    CHECK(sec.find("/*provenance_mutation_id=*/0") == std::string::npos,
          "AC5: no hardcoded provenance_mutation_id=0 in require_effect");
    // require_effect body passes a mid variable, not literal 0.
    const auto req = sec.find("bool Evaluator::require_effect");
    CHECK(req != std::string::npos, "AC5: require_effect definition");
    if (req != std::string::npos) {
        const auto snip = sec.substr(req, 1200);
        CHECK(snip.find("mid") != std::string::npos ||
                  snip.find("mutation_epoch") != std::string::npos,
              "AC5: require_effect computes live mid");
        CHECK(snip.find("check_and_record_effect") != std::string::npos,
              "AC5: require_effect calls check_and_record_effect");
    }

    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_require_effect_live_mid") != std::string::npos,
          "AC5: CMake registers test");
    const auto build = read_file("build.py");
    CHECK(build.find("check_require_effect_live_mid_2384") != std::string::npos ||
              build.find("cmd_require_effect_live_mid_coverage") != std::string::npos,
          "AC5: build.py gate entry");
    const auto gate = read_file("scripts/coverage/checks/check_require_effect_live_mid_2384.py");
    CHECK(!gate.empty() && gate.find("Issue #2384") != std::string::npos,
          "AC5: coverage linter present");
}

} // namespace

int run_test_require_effect_live_mid() {
    std::println("=== Issue #2384: require_effect live mutation_id provenance ===");
    ac1_bound_mismatch_denies();
    ac2_bound_match_allows();
    ac3_soft_off_allows_nonzero_mid();
    ac4_security_event_mid();
    ac5_source_and_gate();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_require_effect_live_mid();
}
#endif
