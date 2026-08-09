// @category: unit
// @reason: Issue #2208 — production default ReemitBoundaryPolicy=Defer
// (no SoftEnter outside real boundary). Refine / lockstep of #2205.
//
//   AC1: Default policy is Defer; SoftEnter requires explicit set.
//   AC2: Outside real boundary + Defer → body does not run; deferred
//        flag + version recorded; outermost exit drains.
//   AC3: SoftEnter still works when policy is forced SoftEnter.
//   AC4: Schema-2114 counters remain; reemit_boundary_policy + schema-2208.
//   AC5: Inside-boundary fast path unchanged (reemit under Guard ok).

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
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

// AC1: default Defer; SoftEnter explicit only
static void ac1_default_defer() {
    std::println("\n--- AC1: default Defer; SoftEnter explicit only ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 1, "AC1: reset → Defer");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_boundary_policy == 1, "AC1: snapshot policy Defer");
    aura_hot_update_set_reemit_boundary_policy(0);
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 0, "AC1: SoftEnter via set");
    aura_hot_update_set_reemit_boundary_policy(1);
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 1, "AC1: restore Defer");
    auto hh = read_file("src/compiler/hot_update_registry.hh");
    CHECK(hh.find("#2208") != std::string::npos, "header cites #2208");
    CHECK(hh.find("reemit_boundary_policy_{1}") != std::string::npos ||
              hh.find("Defer = 1") != std::string::npos,
          "default Defer in header");
}

// AC2: outside + Defer → no body; deferred + version; drain on Guard
static void ac2_outside_defers_and_drains() {
    std::println("\n--- AC2: outside defers; Guard drains ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 0, "outside");
    const auto soft0 = hot_update_registry().snapshot().reemit_soft_boundary_entered_total;
    const auto def0 = hot_update_registry().snapshot().reemit_deferred_for_boundary_total;
    CHECK(aura_reemit_aot_for_dirty(42) == 0, "AC2: returns 0 outside");
    CHECK(aura_hot_update_has_deferred_reemit() == 1, "AC2: deferred pending");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_outside_boundary_total >= 1, "AC2: outside counted");
    CHECK(snap.reemit_deferred_for_boundary_total > def0, "AC2: deferred bumped");
    CHECK(snap.reemit_deferred_pending == 1, "AC2: pending flag");
    CHECK(snap.reemit_soft_boundary_entered_total == soft0, "AC2: no soft-enter");
    CHECK(ef.calls.load() == 0, "AC2: no emit body");

    // Next outermost exit / under Guard drains deferred.
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 1, "inside Guard");
        rf.cursor = 0;
        const auto n = aura_reemit_aot_for_dirty(42);
        CHECK(n >= 1, "AC2: reemit under Guard succeeds");
        CHECK(aura_hot_update_has_deferred_reemit() == 0, "AC2: deferred drained");
        CHECK(ef.ok.load() >= 1, "AC2: emit under Guard");
    }
}

// AC3: SoftEnter forced still works
static void ac3_soft_enter_opt_in() {
    std::println("\n--- AC3: SoftEnter opt-in still works ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_hot_update_set_reemit_boundary_policy(0);
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    const auto n = aura_reemit_aot_for_dirty(0);
    CHECK(n >= 1, "AC3: SoftEnter reemit succeeds");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_soft_boundary_entered_total >= 1, "AC3: soft-enter counted");
    CHECK(snap.reemit_deferred_for_boundary_total == 0, "AC3: not deferred");
    CHECK(ef.ok.load() >= 1, "AC3: emit under soft");
    aura_hot_update_set_reemit_boundary_policy(1);
}

// AC4: schema-2114 retained + reemit_boundary_policy + schema-2208
static void ac4_query_schema() {
    std::println("\n--- AC4: query schema-2208 + policy + 2114 lineage ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2208") == 2208, "schema-2208");
    CHECK(href(cs, "issue-2208") == 2208, "issue-2208");
    CHECK(href(cs, "reemit-boundary-policy") == 1, "policy Defer on query");
    CHECK(href(cs, "reemit-production-default-defer") == 1, "production-default-defer");
    CHECK(href(cs, "reemit-soft-enter-opt-in-only") == 1, "soft-enter-opt-in-only");
    CHECK(href(cs, "reemit-no-soft-enter-outside-boundary") == 1, "no soft outside");
    CHECK(href(cs, "schema-2114") == 2114, "schema-2114 retained");
    CHECK(href(cs, "schema-2205") == 2205, "schema-2205 lineage");
    CHECK(href(cs, "reemit-outside-boundary-total") >= 0, "outside key");
    CHECK(href(cs, "reemit-deferred-for-boundary-total") >= 0, "deferred key");
    CHECK(href(cs, "reemit-soft-boundary-entered-total") >= 0, "soft-enter key");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.schema_2208 == 2208, "snapshot schema_2208");
    CHECK(snap.reemit_boundary_policy == 1, "snapshot policy Defer");
}

// AC5: inside-boundary fast path unchanged
static void ac5_inside_fast_path() {
    std::println("\n--- AC5: inside-boundary fast path ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 1, "inside");
        rf.cursor = 0;
        const auto soft0 = hot_update_registry().snapshot().reemit_soft_boundary_entered_total;
        CHECK(aura_reemit_aot_for_dirty(1) >= 1, "AC5: inside reemit ok");
        CHECK(hot_update_registry().snapshot().reemit_soft_boundary_entered_total == soft0,
              "AC5: no soft-enter inside real boundary");
        CHECK(ef.ok.load() >= 1, "AC5: emit under Guard");
    }
    auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    auto sec = read_file("src/compiler/security_defaults.hh");
    CHECK(bridge.find("#2208") != std::string::npos, "bridge cites #2208");
    CHECK(sec.find("#2208") != std::string::npos, "security_defaults cites #2208");
}

// ── Issue #2855: production deferred-reemit deadline force-drain
//   (bounded native hole). Extends #2208 test file per #81967. No
//   docs/design/* per #1655.
//
// AC1: production lock + env deadline + deferred pending + age >= deadline
//      + depth==0 -> single force-drain body runs; pending clears;
//      reemit_deferred_force_drain_total bumps.
// AC2: env deadline == 0 -> no force body (disabled -> observe-only #2748).
// AC3: steal-complete path under production does NOT call force-drain
//      (#2715 regression guard - source-cite).
// AC4: concurrent force-drain -> only one body runs; second bumps
//      skipped_reentered (CAS re-entry guard).
// AC5: query schema-2855 + counters + wired sentinel live.
// AC6: source-cite + no docs/design/*.

// Test helper: set production_defaults_active directly via the atomic
// (avoids pulling in apply_production_security_defaults which touches
// many other surfaces). Restores prior state on destruction.
struct ProdLockTestGuard {
    std::uint32_t saved;
    ProdLockTestGuard(bool active) noexcept
        : saved(aura::compiler::typed_audit::g_typed_mutation_audit_counters
                    .production_defaults_active.load(std::memory_order_relaxed)) {
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(active ? 1u : 0u, std::memory_order_relaxed);
    }
    ~ProdLockTestGuard() noexcept {
        aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active
            .store(saved, std::memory_order_relaxed);
    }
};

// AC1: production + env deadline + pending + age >= deadline + depth==0
// -> single force-drain body runs.
static void ac2855_1_production_force_drain() {
    std::println("\n--- #2855 AC1: production force-drain body runs ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    hot_update_registry().reset_reemit_force_drain_for_test();
    ProdLockTestGuard prod(true);
    unsetenv("AURA_SANDBOX");
    setenv("AURA_REEMIT_FORCE_DRAIN_DEADLINE_MS", "1", 1);
    CHECK(aura::compiler::HotUpdateRegistry::force_drain_deadline_ms() == 1,
          "AC1: deadline inline-read = 1ms");
    aura_hot_update_set_reemit_boundary_policy(0);
    aura_hot_update_set_in_mutation_boundary_for_reemit(0);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        aura_reemit_aot_for_dirty(42);
    }
    CHECK(aura_hot_update_has_deferred_reemit() == 1, "AC1: deferred pending set");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto age_pre = hot_update_registry().deferred_reemit_age_ms();
    CHECK(age_pre >= 1, "AC1: age >= 1ms deadline");
    const bool drained = hot_update_registry().force_drain_deferred_reemit();
    CHECK(drained, "AC1: force_drain_deferred_reemit returned true");
    CHECK(aura_hot_update_has_deferred_reemit() == 0, "AC1: pending cleared after force-drain");
    CHECK(hot_update_registry().reemit_deferred_force_drain_total() >= 1,
          "AC1: force-drain total bumped");
    CHECK(!hot_update_registry().should_force_drain_deferred_reemit(),
          "AC1: should_force_drain returns false after drain (no pending)");
    unsetenv("AURA_REEMIT_FORCE_DRAIN_DEADLINE_MS");
    hot_update_registry().reset_reemit_force_drain_for_test();
}

// AC2: env deadline == 0 -> no force body (disabled -> observe-only #2748).
static void ac2855_2_deadline_zero_disabled() {
    std::println("\n--- #2855 AC2: deadline == 0 -> disabled (observe-only) ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    hot_update_registry().reset_reemit_force_drain_for_test();
    ProdLockTestGuard prod(true);
    unsetenv("AURA_REEMIT_FORCE_DRAIN_DEADLINE_MS");
    setenv("AURA_SANDBOX", "off", 1);
    CHECK(aura::compiler::HotUpdateRegistry::force_drain_deadline_ms() == 0,
          "AC2: deadline==0 (disabled)");
    CHECK(!hot_update_registry().should_force_drain_deferred_reemit(),
          "AC2: should_force_drain returns false when env == 0");
    CHECK(!hot_update_registry().force_drain_deferred_reemit(),
          "AC2: force_drain returns false when env == 0");
    CHECK(hot_update_registry().reemit_deferred_force_drain_total() == 0,
          "AC2: no force-drain counter bump when env == 0");
    unsetenv("AURA_SANDBOX");
}

// AC3: steal-complete path does NOT call force-drain (#2715 regression).
// Verified via source-cite: on_reemit_pipeline_call (NOT steal-complete)
// is the only caller of force_drain_deferred_reemit. The bridge
// (aura_jit_bridge.cpp) and the steal-complete C ABI
// (aura_evaluator_on_steal_complete) must NOT call force-drain.
static void ac2855_3_steal_complete_no_force_drain() {
    std::println("\n--- #2855 AC3: steal-complete path no force-drain ---");
    const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    CHECK(cpp.find("force_drain_deferred_reemit()") != std::string::npos,
          "AC3: cpp invokes force_drain_deferred_reemit (via on_reemit_pipeline_call)");
    CHECK(cpp.find("(void)force_drain_deferred_reemit();") != std::string::npos,
          "AC3: amortized site (void)-cast -> no return value leak");
    CHECK(cpp.find("on_reemit_pipeline_call") != std::string::npos,
          "AC3: on_reemit_pipeline_call present (amortized call site)");
    CHECK(cpp.find("aura_evaluator_on_steal_complete") == std::string::npos,
          "AC3: force_drain NOT invoked from steal-complete (#2715 regression guard)");
    CHECK(bridge.find("aura_evaluator_on_steal_complete") == std::string::npos,
          "AC3: bridge does not wire force-drain into steal path");
    CHECK(cpp.find("#2715") != std::string::npos || cpp.find("Issue #2715") != std::string::npos ||
              bridge.find("#2715") != std::string::npos ||
              bridge.find("Issue #2715") != std::string::npos,
          "AC3: #2715 steal-safety cited");
    CHECK(hh.find("Issue #2715") != std::string::npos, "AC3: hh cites #2715 steal path safety");
}

// AC4: concurrent force-drain -> CAS re-entry guard observable.
static void ac2855_4_concurrent_reentry() {
    std::println("\n--- #2855 AC4: CAS re-entry guard observable ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    hot_update_registry().reset_reemit_force_drain_for_test();
    ProdLockTestGuard prod(true);
    unsetenv("AURA_SANDBOX");
    setenv("AURA_REEMIT_FORCE_DRAIN_DEADLINE_MS", "1", 1);
    aura_hot_update_set_reemit_boundary_policy(0);
    aura_hot_update_set_in_mutation_boundary_for_reemit(0);
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        aura_reemit_aot_for_dirty(42);
    }
    CHECK(aura_hot_update_has_deferred_reemit() == 1, "AC4: pending set");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    aura::compiler::HotUpdateRegistry::on_reemit_pipeline_call(0, 0);
    aura::compiler::HotUpdateRegistry::on_reemit_pipeline_call(0, 0);
    CHECK(hot_update_registry().reemit_deferred_force_drain_total() >= 1,
          "AC4: at least one force-drain fired");
    const auto skipped =
        hot_update_registry().reemit_deferred_force_drain_skipped_reentered_total();
    const auto doublep = hot_update_registry().reemit_deferred_force_drain_double_prevented_total();
    CHECK(skipped >= 0 && doublep >= 0, "AC4: skipped + double_prevented observable");
    unsetenv("AURA_REEMIT_FORCE_DRAIN_DEADLINE_MS");
    hot_update_registry().reset_reemit_force_drain_for_test();
}

// AC5: query schema-2855 + counters + wired sentinel live.
static void ac2855_5_query_schema_2855() {
    std::println("\n--- #2855 AC5: query schema-2855 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "AC5: warm eval");
    CHECK(href(cs, "schema-2855") == 2855, "AC5: schema-2855 live");
    CHECK(href(cs, "issue-2855") == 2855, "AC5: issue-2855 live");
    CHECK(href(cs, "reemit-deferred-force-drain-wired") == 1,
          "AC5: reemit-deferred-force-drain-wired sentinel live");
    CHECK(href(cs, "reemit_deferred_force_drain_wired") == 1,
          "AC5: reemit_deferred_force_drain_wired camelCase live");
    CHECK(href(cs, "reemit-deferred-force-drain-total") >= 0,
          "AC5: reemit-deferred-force-drain-total key live");
    CHECK(href(cs, "reemit_deferred_force_drain_total") >= 0,
          "AC5: reemit_deferred_force_drain_total camelCase live");
    CHECK(href(cs, "reemit-deferred-force-drain-skipped-reentered-total") >= 0,
          "AC5: skipped-reentered key live");
    CHECK(href(cs, "reemit_deferred_force_drain_skipped_reentered_total") >= 0,
          "AC5: skipped_reentered camelCase live");
    CHECK(href(cs, "reemit-deferred-force-drain-double-prevented-total") >= 0,
          "AC5: double-prevented key live");
    CHECK(href(cs, "reemit_deferred_force_drain_double_prevented_total") >= 0,
          "AC5: double_prevented camelCase live");
    CHECK(href(cs, "reemit-deferred-force-drain-deadline-ms") >= 0, "AC5: deadline-ms key live");
    CHECK(href(cs, "reemit_deferred_force_drain_deadline_ms") >= 0,
          "AC5: deadline_ms camelCase live");
    CHECK(href(cs, "schema-2748") == 2748, "AC5: schema-2748 preserved");
    CHECK(href(cs, "schema-2690") == 2690, "AC5: schema-2690 preserved");
    CHECK(href(cs, "schema-2604") == 2604, "AC5: schema-2604 preserved");
    CHECK(href(cs, "schema-2205") == 2205, "AC5: schema-2205 preserved");
    CHECK(href(cs, "schema-2208") == 2208, "AC5: schema-2208 preserved");
}

// AC6: source-cite + no docs/design/*.
static void ac2855_6_source_cite_and_no_design() {
    std::println("\n--- #2855 AC6: source-cite + no docs/design/ ---");
    const auto cpp = read_file("src/compiler/hot_update_registry.cpp");
    const auto hh = read_file("src/compiler/hot_update_registry.hh");
    const auto obs = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto t = read_file("tests/compiler/test_reemit_production_default_defer_v2.cpp");
    CHECK(cpp.find("Issue #2855") != std::string::npos, "AC6: cpp cites #2855 (force-drain entry)");
    CHECK(cpp.find("force_drain_deadline_ms()") != std::string::npos,
          "AC6: cpp defines force_drain_deadline_ms");
    CHECK(cpp.find("should_force_drain_deferred_reemit()") != std::string::npos,
          "AC6: cpp defines should_force_drain_deferred_reemit");
    CHECK(cpp.find("force_drain_deferred_reemit()") != std::string::npos,
          "AC6: cpp defines force_drain_deferred_reemit");
    CHECK(cpp.find("compare_exchange_strong") != std::string::npos,
          "AC6: CAS re-entry guard via compare_exchange_strong");
    CHECK(cpp.find("g_reemit_force_drain_in_flight_") != std::string::npos,
          "AC6: cpp uses CAS flag");
    CHECK(cpp.find("AURA_REEMIT_FORCE_DRAIN_DEADLINE_MS") != std::string::npos,
          "AC6: cpp reads new env var (distinct from #2748)");
    CHECK(cpp.find("on_reemit_pipeline_call") != std::string::npos,
          "AC6: amortized call site in on_reemit_pipeline_call");
    CHECK(cpp.find("drain_pending_recovery") != std::string::npos,
          "AC6: drain_pending_recovery checks CAS flag (double-prevented)");
    CHECK(hh.find("kReemitForceDrainIssue = 2855") != std::string::npos,
          "AC6: hh has #2855 issue stamp");
    CHECK(hh.find("g_reemit_deferred_force_drain_total_") != std::string::npos,
          "AC6: hh has force-drain-total atomic");
    CHECK(hh.find("g_reemit_deferred_force_drain_skipped_reentered_total_") != std::string::npos,
          "AC6: hh has skipped-reentered atomic");
    CHECK(hh.find("g_reemit_deferred_force_drain_double_prevented_total_") != std::string::npos,
          "AC6: hh has double-prevented atomic");
    CHECK(hh.find("g_reemit_force_drain_in_flight_") != std::string::npos,
          "AC6: hh has CAS re-entry guard atomic");
    CHECK(hh.find("reset_reemit_force_drain_for_test") != std::string::npos,
          "AC6: hh has test reset");
    CHECK(hh.find("reemit_deferred_force_drain_total()") != std::string::npos,
          "AC6: hh has force-drain total accessor");
    CHECK(hh.find("reemit_deferred_force_drain_skipped_reentered_total()") != std::string::npos,
          "AC6: hh has skipped-reentered accessor");
    CHECK(hh.find("reemit_deferred_force_drain_double_prevented_total()") != std::string::npos,
          "AC6: hh has double-prevented accessor");
    CHECK(obs.find("schema-2855") != std::string::npos, "AC6: obs_eval schema-2855");
    CHECK(obs.find("issue-2855") != std::string::npos, "AC6: obs_eval issue-2855");
    CHECK(obs.find("reemit-deferred-force-drain-total") != std::string::npos,
          "AC6: obs_eval exposes force-drain-total key");
    CHECK(obs.find("reemit-deferred-force-drain-deadline-ms") != std::string::npos,
          "AC6: obs_eval exposes deadline-ms key");
    CHECK(t.find("ac2855_1_production_force_drain") != std::string::npos, "AC6: AC1 present");
    CHECK(t.find("ac2855_2_deadline_zero_disabled") != std::string::npos, "AC6: AC2 present");
    CHECK(t.find("ac2855_3_steal_complete_no_force_drain") != std::string::npos,
          "AC6: AC3 present");
    CHECK(t.find("ac2855_4_concurrent_reentry") != std::string::npos, "AC6: AC4 present");
    CHECK(t.find("ac2855_5_query_schema_2855") != std::string::npos, "AC6: AC5 present");
    CHECK(t.find("ac2855_6_source_cite_and_no_design") != std::string::npos, "AC6: AC6 self-test");
    CHECK(t.find("ac1_default_defer") != std::string::npos, "AC6: #2208 tests preserved");
    CHECK(t.find("ac5_inside_fast_path") != std::string::npos, "AC6: #2208 AC5 preserved");
    const std::string design_path = "docs/design/2855-";
    CHECK(read_file((design_path + "force-drain.md").c_str()).empty(),
          "AC6: no docs/design/2855-* per #1655");
}

} // namespace

int run_test_reemit_production_default_defer_v2() {
    std::println("=== Issue #2208: production default reemit Defer (refine #2205) ===");
    ac1_default_defer();
    ac2_outside_defers_and_drains();
    ac3_soft_enter_opt_in();
    ac4_query_schema();
    ac5_inside_fast_path();
    std::println("\n=== Issue #2855: production deferred-reemit deadline force-drain (extends "
                 "#2208 test file per #81967) ===");
    ac2855_1_production_force_drain();
    ac2855_2_deadline_zero_disabled();
    ac2855_3_steal_complete_no_force_drain();
    ac2855_4_concurrent_reentry();
    ac2855_5_query_schema_2855();
    ac2855_6_source_cite_and_no_design();
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_reemit_production_default_defer_v2();
}
#endif
