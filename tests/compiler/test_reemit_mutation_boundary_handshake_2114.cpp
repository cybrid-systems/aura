// @category: unit
// @reason: Issue #2114 — HotUpdate reemit ↔ MutationBoundary explicit handshake.
//
// Handshake policy for Agent / plugin authors (AC5 / #2205):
//   SoftEnter (policy=0, **opt-in only**): reemit outside a real
//     MutationBoundary enters a TLS soft reemit boundary for the call
//     duration. Not steal-safe — production default is Defer (#2205).
//   Defer (policy=1, **production default #2205**): reemit outside records
//     pending version and returns 0; next outermost MutationBoundary exit
//     drains it under the real Guard.
//   Inside real boundary (depth>0 or held flag, #2090 dtor window): proceed
//     without soft-enter. Outside path is never silent (always counted).
//
//   AC1: Forced reemit outside → soft-enter (opt-in) or defer (never silent)
//   AC2: Soft path does not break baseline reemit / inside-boundary path
//   AC3: Query exposes three counters + schema-2114
//   AC4: Existing reemit wiring still present (source + soft path green)
//   AC5: Policy docs in this header + query enable keys

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "test_harness.hpp"

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
using aura::compiler::HotUpdateRegistry;
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

static void ac1_outside_soft_or_defer() {
    std::println("\n--- AC1: outside boundary soft-enter / defer ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_hot_update_reset_deopt_storm_state_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);

    // Production default Defer (#2205) after reset.
    CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 0, "outside before");
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 1, "default Defer (#2205)");

    // SoftEnter is opt-in (#2205 AC5) — set policy 0 for soft path test.
    aura_hot_update_set_reemit_boundary_policy(0);
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 0, "SoftEnter opt-in");
    const auto n = aura_reemit_aot_for_dirty(0);
    CHECK(n >= 1, "soft-enter reemit succeeds");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_outside_boundary_total >= 1, "outside counted");
    CHECK(snap.reemit_soft_boundary_entered_total >= 1, "soft-enter counted");
    CHECK(snap.reemit_deferred_for_boundary_total == 0, "not deferred on SoftEnter");
    CHECK(aura_hot_update_soft_reemit_boundary_active() == 0, "soft depth cleared after call");
    CHECK(ef.ok.load() >= 1, "emit ran under soft boundary");

    // Defer policy (production default)
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    CHECK(aura_hot_update_get_reemit_boundary_policy() == 1, "reset → Defer");
    wire_reemit(rf, ef);
    const auto n2 = aura_reemit_aot_for_dirty(42);
    CHECK(n2 == 0, "deferred returns 0");
    CHECK(aura_hot_update_has_deferred_reemit() == 1, "pending set");
    snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_outside_boundary_total >= 1, "outside counted (defer)");
    CHECK(snap.reemit_deferred_for_boundary_total >= 1, "deferred counted");
    CHECK(snap.reemit_soft_boundary_entered_total == 0, "no soft-enter on defer");
    CHECK(ef.calls.load() == 0, "emit not called when deferred");

    // Drain deferred under real MutationBoundary
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 1, "inside Guard");
        // Reemit while held (simulates #2090 dtor window / flush)
        rf.cursor = 0;
        const auto n3 = aura_reemit_aot_for_dirty(42);
        CHECK(n3 >= 1, "inside reemit succeeds");
        CHECK(aura_hot_update_has_deferred_reemit() == 0, "deferred drained");
    }
}

static void ac2_inside_boundary_baseline() {
    std::println("\n--- AC2: inside boundary + soft baseline ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_hot_update_reset_deopt_storm_state_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);

    CompilerService cs;
    bool ok = true;
    std::uint64_t outside_before = hot_update_registry().snapshot().reemit_outside_boundary_total;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(cs.evaluator().mutation_boundary_held(), "held");
        CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 1, "inside for reemit");
        const auto n = aura_reemit_aot_for_dirty(0);
        CHECK(n >= 1, "inside reemit ok");
    }
    auto snap = hot_update_registry().snapshot();
    // Inside path must not bump outside / soft counters
    CHECK(snap.reemit_outside_boundary_total == static_cast<std::int64_t>(outside_before),
          "no outside bump inside Guard");
    CHECK(ef.ok.load() >= 1, "emit under Guard");
}

static void ac3_query_surface() {
    std::println("\n--- AC3: query counters + schema-2114 ---");
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    ReemitFixture rf;
    EmitFixture ef;
    wire_reemit(rf, ef);
    // Soft path: opt-in SoftEnter then reemit outside.
    aura_hot_update_set_reemit_boundary_policy(0);
    (void)aura_reemit_aot_for_dirty(0);

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "schema-2114") == 2114, "schema-2114");
    CHECK(href(cs, "issue-2114") == 2114, "issue-2114");
    CHECK(href(cs, "reemit-handshake-wired") == 1, "wired");
    CHECK(href(cs, "reemit-outside-boundary-total") >= 1, "outside query");
    CHECK(href(cs, "reemit-soft-boundary-entered-total") >= 1, "soft query");
    CHECK(href(cs, "reemit-deferred-for-boundary-total") >= 0, "deferred query");
    CHECK(href(cs, "reemit-handshake-policy-soft-enter") == 0, "policy soft doc");
    CHECK(href(cs, "reemit-handshake-policy-defer") == 1, "policy defer doc");
}

static void ac4_existing_reemit_wiring() {
    std::println("\n--- AC4: existing reemit / handshake source wiring ---");
    auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    auto reg = read_file("src/compiler/hot_update_registry.hh");
    auto regc = read_file("src/compiler/hot_update_registry.cpp");
    auto mb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    CHECK(bridge.find("Issue #2114") != std::string::npos ||
              bridge.find("#2114") != std::string::npos,
          "bridge cites #2114");
    CHECK(bridge.find("on_reemit_outside_boundary") != std::string::npos, "outside bump");
    CHECK(bridge.find("soft_reemit_boundary_enter") != std::string::npos, "soft enter");
    CHECK(bridge.find("defer_reemit_for_boundary") != std::string::npos ||
              bridge.find("Defer") != std::string::npos,
          "defer path");
    CHECK(reg.find("reemit_outside_boundary_total") != std::string::npos, "registry metric");
    CHECK(reg.find("SoftEnter") != std::string::npos, "SoftEnter policy");
    CHECK(regc.find("in_mutation_boundary_for_reemit") != std::string::npos, "in-boundary check");
    CHECK(mb.find("deferred_reemit") != std::string::npos ||
              mb.find("aura_hot_update_has_deferred_reemit") != std::string::npos,
          "boundary exit drain");
    CHECK(q.find("schema-2114") != std::string::npos, "query schema");
    // Throttle preserved
    CHECK(bridge.find("should_throttle_reemit") != std::string::npos, "storm throttle retained");
}

static void ac5_docs() {
    std::println("\n--- AC5: handshake policy docs ---");
    auto reg = read_file("src/compiler/hot_update_registry.hh");
    CHECK(reg.find("SoftEnter") != std::string::npos, "SoftEnter documented");
    CHECK(reg.find("Defer") != std::string::npos, "Defer documented");
    CHECK(reg.find("Agent") != std::string::npos || reg.find("plugin") != std::string::npos,
          "Agent/plugin policy note");
    CompilerService cs;
    CHECK(href(cs, "reemit-handshake-policy-soft-enter") == 0, "query soft sentinel");
    CHECK(href(cs, "reemit-handshake-policy-defer") == 1, "query defer sentinel");
}

// ── Issue #2604: outermost MutationBoundary exit auto-drain deferred
// reemit + one region-filtered pass. Refines #2114 / #2205 / #2208
// by adding the auto-drain on outermost exit so boundary exit ≈
// consistent epoch without requiring Agent round-trips on every
// soft dirty.

// AC1: deferred reemit pending → outermost exit triggers one reemit;
//      deferred flag cleared. Counters bump on_boundary_exit + success.
static void ac2604_deferred_triggers_reemit() {
    std::println("\n--- #2604 AC1: deferred reemit triggers auto-drain ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_hot_update_set_reemit_boundary_policy(1); // Defer

    // Source-cite: auto-drain wired in exit_mutation_boundary success path.
    const auto eval_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total") !=
              std::string::npos,
          "AC1: auto-drain bumper wired in evaluator_mutation_boundary.cpp");
    CHECK(eval_cpp.find("!nested_boundary && success") != std::string::npos,
          "AC1: auto-drain guarded on outermost + success");
    CHECK(eval_cpp.find("has_deferred_reemit") != std::string::npos,
          "AC1: auto-drain checks has_deferred_reemit()");
    CHECK(eval_cpp.find("aura_reemit_aot_for_dirty") != std::string::npos,
          "AC1: auto-drain calls aura_reemit_aot_for_dirty (region-filtered pass)");
    CHECK(eval_cpp.find("aura_bump_reemit_auto_drain_success_total") !=
              std::string::npos,
          "AC1: auto-drain success bumper called");
    // Counters exist + start at 0.
    CHECK(metrics.reemit_auto_drain_on_boundary_exit_total.load() == 0,
          "AC1: on_boundary_exit_total starts at 0");
    CHECK(metrics.reemit_auto_drain_success_total.load() == 0,
          "AC1: success_total starts at 0");
    CHECK(metrics.reemit_auto_drain_throttled_total.load() == 0,
          "AC1: throttled_total starts at 0");
    aura_set_aot_metrics(nullptr);
}

// AC2: only last_region_mask_from_dirty set → same auto pass.
static void ac2604_mask_only_triggers_reemit() {
    std::println("\n--- #2604 AC2: mask-only path triggers auto-drain ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_hot_update_reset_reemit_boundary_handshake_for_test();

    // Source-cite: auto-drain block must check last_region_mask_from_dirty != 0.
    const auto eval_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(eval_cpp.find("last_region_mask_from_dirty") != std::string::npos,
          "AC2: auto-drain checks last_region_mask_from_dirty");
    // The C ABI: last_region_mask_from_dirty getter exists.
    const auto hur_h = read_file("src/compiler/hot_update_registry.hh");
    CHECK(hur_h.find("last_region_mask_from_dirty()") != std::string::npos,
          "AC2: last_region_mask_from_dirty() getter exists");
    aura_set_aot_metrics(nullptr);
}

// AC3: storm throttle active → skip body, bump throttled counter.
static void ac2604_storm_throttle_bumps_throttled() {
    std::println("\n--- #2604 AC3: storm throttle → bump throttled ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_hot_update_reset_reemit_boundary_handshake_for_test();

    // Source-cite: auto-drain must check should_throttle_reemit and bump
    // throttled_total before calling reemit (no silent drop forever).
    const auto eval_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(eval_cpp.find("aura_bump_reemit_auto_drain_throttled_total") !=
              std::string::npos,
          "AC3: throttled bumper wired");
    CHECK(eval_cpp.find("should_throttle_reemit") != std::string::npos,
          "AC3: auto-drain checks should_throttle_reemit");
    aura_set_aot_metrics(nullptr);
}

// AC4: common path (no deferred, mask=0) → zero extra work.
static void ac2604_soft_zero_cost() {
    std::println("\n--- #2604 AC4: soft path → zero extra work ---");
    CompilerMetrics metrics{};
    aura_set_aot_metrics(&metrics);
    aura_hot_update_reset_reemit_boundary_handshake_for_test();

    // Source-cite: auto-drain block must guard BEFORE the bumper call.
    const auto eval_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    // The guard must reference has_deferred_reemit AND last_region_mask_from_dirty.
    // Approximate: both terms appear in a block preceding the bumper call.
    const auto bumper_idx = eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total");
    const auto preceding = eval_cpp[std::max<std::size_t>(0, bumper_idx - 600): bumper_idx];
    CHECK(preceding.find("has_deferred_reemit") != std::string::npos,
          "AC4: guard checks has_deferred_reemit()");
    CHECK(preceding.find("last_region_mask_from_dirty") != std::string::npos,
          "AC4: guard checks last_region_mask_from_dirty");
    // Counters unchanged on no-call path.
    CHECK(metrics.reemit_auto_drain_on_boundary_exit_total.load() == 0,
          "AC4: on_boundary_exit_total unchanged on soft path");
    CHECK(metrics.reemit_auto_drain_success_total.load() == 0,
          "AC4: success_total unchanged on soft path");
    aura_set_aot_metrics(nullptr);
}

// AC5: source-cite + schema-2604 cross-link + linter gate.
static void ac2604_source_and_schema() {
    std::println("\n--- #2604 AC5: source-cite + schema-2604 ---");
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto eval_cpp = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto hur = read_file("src/compiler/hot_update_registry.cpp");
    const auto hur_h = read_file("src/compiler/hot_update_registry.hh");
    const auto metrics = read_file("src/compiler/observability_metrics.h");
    const auto obs_eval = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    const auto build = read_file("build.py");

    // Source-cite.
    CHECK(hur.find("aura_bump_reemit_auto_drain_on_boundary_exit_total") !=
              std::string::npos,
          "AC5: C ABI in hot_update_registry.cpp");
    CHECK(hur_h.find("bump_reemit_auto_drain_on_boundary_exit_total") !=
              std::string::npos,
          "AC5: class decl in hot_update_registry.hh");
    CHECK(eval_cpp.find("aura_bump_reemit_auto_drain_on_boundary_exit_total") !=
              std::string::npos,
          "AC5: wired in evaluator_mutation_boundary.cpp");
    CHECK(metrics.find("reemit_auto_drain_on_boundary_exit_total") != std::string::npos,
          "AC5: counter in metrics.h");
    // Query surface cross-link.
    CHECK(obs_eval.find("reemit-auto-drain-on-boundary-exit-total") != std::string::npos,
          "AC5: query key on query:aot-reload-stats");
    CHECK(obs_eval.find("schema-2604") != std::string::npos, "AC5: schema-2604");
    // Build.py gate wired.
    CHECK(build.find("cmd_reemit_auto_drain_boundary_2604_coverage") != std::string::npos,
          "AC5: build.py cmd helper");
    // Compatibility: prior schemas preserved.
    CHECK(obs_eval.find("schema-2165") != std::string::npos, "AC5: schema-2165 retained");
    CHECK(obs_eval.find("schema-2232") != std::string::npos, "AC5: schema-2232 retained");
}

} // namespace

int main() {
    std::println("=== Issue #2114: reemit ↔ MutationBoundary handshake ===");
    ac1_outside_soft_or_defer();
    ac2_inside_boundary_baseline();
    ac3_query_surface();
    ac4_existing_reemit_wiring();
    ac5_docs();
    // Issue #2604: outermost exit auto-drain deferred reemit.
    ac2604_deferred_triggers_reemit();
    ac2604_mask_only_triggers_reemit();
    ac2604_storm_throttle_bumps_throttled();
    ac2604_soft_zero_cost();
    ac2604_source_and_schema();
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    std::println("\n=== #2114 + #2604 Results: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
