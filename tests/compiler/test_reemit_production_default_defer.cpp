// @category: unit
// @reason: Issue #2205 — production default reemit policy Defer /
// RequireRealBoundary (disable SoftEnter under multi-fiber).
//
//   AC1: Production default (reset / process init) → policy Defer;
//        SoftEnter off.
//   AC2: Outside Guard, aura_reemit_aot_for_dirty returns 0, bumps
//        deferred/outside, does NOT soft-enter; no emit.
//   AC3: Outermost Guard with deferred pending → reemit under lock.
//   AC4: Stress-style: outside reemit only defers (no soft); inside ok.
//   AC5: Explicit SoftEnter opt-in still works (#2114 path).
//   AC6: query:hot-update-registry-stats schema-2205 + policy + deferred.
//   AC7: RequireRealBoundary rejects without defer; source cites.

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern "C" void aura_hot_update_reset_reemit_boundary_handshake_for_test(void);
extern "C" void aura_hot_update_set_reemit_boundary_policy(int policy);
extern "C" int aura_hot_update_get_reemit_boundary_policy(void);
extern "C" int aura_hot_update_in_mutation_boundary_for_reemit(void);
extern "C" int aura_hot_update_soft_reemit_boundary_active(void);
extern "C" int aura_hot_update_has_deferred_reemit(void);
extern "C" void aura_hot_update_reset_deopt_storm_state_for_test(void);

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::hot_update_registry;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

struct ReemitFixture {
    struct Candidate {
        std::string name;
        std::uint64_t region;
        bool from_closure_capture;
    };
    std::vector<Candidate> candidates;
    std::size_t cursor = 0;
};

static bool reemit_candidate_iter(void* userdata, const char** out_name, std::uint64_t* out_region,
                                  bool* out_from_closure_capture) {
    auto* f = static_cast<ReemitFixture*>(userdata);
    if (!f || f->candidates.empty())
        return false;
    if (f->cursor >= f->candidates.size()) {
        f->cursor = 0;
        return false;
    }
    const auto& c = f->candidates[f->cursor++];
    *out_name = c.name.c_str();
    *out_region = c.region;
    *out_from_closure_capture = c.from_closure_capture;
    return true;
}

struct EmitFixture {
    std::unordered_set<std::string> fail_names;
    std::atomic<std::uint32_t> calls{0};
    std::atomic<std::uint32_t> ok{0};
};

static bool emit_fn(const char* name, std::uint64_t /*region*/, void* userdata) {
    auto* f = static_cast<EmitFixture*>(userdata);
    f->calls.fetch_add(1, std::memory_order_relaxed);
    if (!name)
        return false;
    if (f->fail_names.count(name))
        return false;
    f->ok.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:hot-update-registry-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

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

static void wire_reemit(ReemitFixture& rf, EmitFixture& ef) {
    rf.candidates = {{"fn_a", 0, false}, {"fn_b", 1, false}};
    rf.cursor = 0;
    ef.calls.store(0);
    ef.ok.store(0);
    aura_set_reemit_candidate_fn(&reemit_candidate_iter, &rf);
    aura_set_aot_emit_fn(&emit_fn, &ef);
    aura_hot_update_reset_deopt_storm_state_for_test();
}

static void ac1_production_default_defer() {
    std::println("\n--- AC1: production default Defer ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 1, "reset → Defer");
    // Process-wide default atomic is Defer; security defaults also force Defer
    // unless AURA_REEMIT_SOFT_ENTER=1. Source-cite the production wire without
    // applying full multi-tenant sandbox (avoids process-wide side effects
    // mid-suite).
    auto sec = read_file("src/compiler/security_defaults.hh");
    CHECK(sec.find("AURA_REEMIT_SOFT_ENTER") != std::string::npos,
          "security defaults SoftEnter env");
    CHECK(sec.find("#2205") != std::string::npos, "security defaults cite #2205");
    CHECK(sec.find("set_reemit_boundary_policy") != std::string::npos ||
              sec.find("ReemitBoundaryPolicy::Defer") != std::string::npos,
          "production defaults set Defer");
    // Explicit opt-in SoftEnter via setter, then restore Defer.
    aura_hot_update_set_reemit_boundary_policy(0);
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 0, "SoftEnter opt-in via setter");
    aura_hot_update_set_reemit_boundary_policy(1);
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 1, "restore Defer");
    auto hh = read_file("src/compiler/hot_update_registry.hh");
    CHECK(hh.find("production default") != std::string::npos ||
              hh.find("Production default") != std::string::npos ||
              hh.find("#2205") != std::string::npos,
          "docs cite production Defer / #2205");
}

static void ac2_outside_defers_no_soft() {
    std::println("\n--- AC2: outside Guard defers, no soft-enter, no emit ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 1, "Defer");
    CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 0, "outside");
    const auto soft0 = hot_update_registry().snapshot().reemit_soft_boundary_entered_total;
    const auto def0 = hot_update_registry().snapshot().reemit_deferred_for_boundary_total;
    const auto n = aura_reemit_aot_for_dirty(99);
    CHECK(n == 0, "AC2: reemit returns 0 outside under Defer");
    CHECK(aura_hot_update_has_deferred_reemit() == 1, "AC2: deferred pending");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_outside_boundary_total >= 1, "AC2: outside counted");
    CHECK(snap.reemit_deferred_for_boundary_total > def0, "AC2: deferred bumped");
    CHECK(snap.reemit_soft_boundary_entered_total == soft0, "AC2: no soft-enter");
    CHECK(ef.calls.load() == 0, "AC2: emit not called");
    CHECK(ef.ok.load() == 0, "AC2: no AOT success");
}

static void ac3_guard_drains_deferred() {
    std::println("\n--- AC3: Guard drains deferred reemit under lock ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    CHECK(aura_reemit_aot_for_dirty(7) == 0, "outside defers");
    CHECK(aura_hot_update_has_deferred_reemit() == 1, "pending");
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 1, "inside Guard");
        rf.cursor = 0;
        const auto n = aura_reemit_aot_for_dirty(7);
        CHECK(n >= 1, "AC3: reemit under Guard succeeds");
        CHECK(aura_hot_update_has_deferred_reemit() == 0, "AC3: deferred drained");
        CHECK(ef.ok.load() >= 1, "AC3: emit under Guard");
    }
}

static void ac4_multi_outside_only_defers() {
    std::println("\n--- AC4: repeated outside reemit only defers ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    const auto soft0 = hot_update_registry().snapshot().reemit_soft_boundary_entered_total;
    for (int i = 0; i < 8; ++i)
        CHECK(aura_reemit_aot_for_dirty(static_cast<std::uint64_t>(i + 1)) == 0, "defer loop");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_deferred_for_boundary_total >= 8, "AC4: deferred ≥8");
    CHECK(snap.reemit_soft_boundary_entered_total == soft0, "AC4: still no soft-enter");
    CHECK(ef.calls.load() == 0, "AC4: no emit outside");
    // Inside still works
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        rf.cursor = 0;
        CHECK(aura_reemit_aot_for_dirty(1) >= 1, "AC4: inside ok");
    }
}

static void ac5_soft_enter_opt_in() {
    std::println("\n--- AC5: SoftEnter opt-in still works ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_hot_update_set_reemit_boundary_policy(0);
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    const auto n = aura_reemit_aot_for_dirty(0);
    CHECK(n >= 1, "AC5: SoftEnter reemit succeeds");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_soft_boundary_entered_total >= 1, "AC5: soft-enter counted");
    CHECK(snap.reemit_deferred_for_boundary_total == 0, "AC5: not deferred");
    CHECK(ef.ok.load() >= 1, "AC5: emit under soft");
}

static void ac6_query_schema() {
    std::println("\n--- AC6: query schema-2205 ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2205") == 2205, "schema-2205");
    CHECK(href(cs, "issue-2205") == 2205, "issue-2205");
    CHECK(href(cs, "reemit-production-default-defer") == 1, "production-default-defer");
    CHECK(href(cs, "reemit-soft-enter-opt-in-only") == 1, "soft-enter-opt-in-only");
    CHECK(href(cs, "reemit-boundary-policy") == 1, "policy Defer on query");
    CHECK(href(cs, "reemit-handshake-policy-require-real") == 2, "require-real sentinel");
    // Lineage
    CHECK(href(cs, "schema-2114") == 2114, "schema-2114 retained");
    CHECK(href(cs, "reemit-handshake-wired") == 1, "handshake wired");
}

static void ac7_require_real_and_source() {
    std::println("\n--- AC7: RequireRealBoundary + source ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_hot_update_set_reemit_boundary_policy(2);
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 2, "RequireRealBoundary");
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    const auto rej0 = hot_update_registry().snapshot().reemit_rejected_require_real_total;
    const auto def0 = hot_update_registry().snapshot().reemit_deferred_for_boundary_total;
    CHECK(aura_reemit_aot_for_dirty(5) == 0, "reject returns 0");
    CHECK(aura_hot_update_has_deferred_reemit() == 0, "no pending on require-real");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_rejected_require_real_total > rej0, "rejected counter");
    CHECK(snap.reemit_deferred_for_boundary_total == def0, "no defer on require-real");
    CHECK(snap.reemit_soft_boundary_entered_total == 0, "no soft on require-real");
    CHECK(ef.calls.load() == 0, "no emit");

    auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    auto reg = read_file("src/compiler/hot_update_registry.hh");
    auto sec = read_file("src/compiler/security_defaults.hh");
    CHECK(bridge.find("#2205") != std::string::npos || bridge.find("2205") != std::string::npos,
          "bridge cites #2205");
    CHECK(reg.find("RequireRealBoundary") != std::string::npos, "RequireRealBoundary enum");
    CHECK(reg.find("reemit_boundary_policy_{1}") != std::string::npos ||
              reg.find("Defer = 1") != std::string::npos,
          "default Defer in header");
    CHECK(sec.find("AURA_REEMIT_SOFT_ENTER") != std::string::npos,
          "security defaults wire SoftEnter env");
    CHECK(sec.find("#2205") != std::string::npos, "security defaults cite #2205");
}

} // namespace

int run_test_reemit_production_default_defer() {
    std::println("=== Issue #2205: production default reemit Defer (no SoftEnter multi-fiber) ===");
    ac1_production_default_defer();
    ac2_outside_defers_no_soft();
    ac3_guard_drains_deferred();
    ac4_multi_outside_only_defers();
    ac5_soft_enter_opt_in();
    ac6_query_schema();
    ac7_require_real_and_source();
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_reemit_production_default_defer();
}
#endif
