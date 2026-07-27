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

} // namespace

int main() {
    std::println("=== Issue #2208: production default reemit Defer (refine #2205) ===");
    ac1_default_defer();
    ac2_outside_defers_and_drains();
    ac3_soft_enter_opt_in();
    ac4_query_schema();
    ac5_inside_fast_path();
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_set_aot_emit_fn(nullptr, nullptr);
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
