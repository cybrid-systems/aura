// @category: unit
// @reason: Issue #3373 — production CompilerService wires
// aura_set_reemit_candidate_fn so aura_reemit_aot_for_dirty sees the
// dirty set under production. Without this, the production host
// binary's reemit body never sees a candidate iterator and
// decide_and_reemit is a skeleton no-op (counter / facade closed
// loop, not native-replace closed loop). Single-workspace MVP; no
// BFS / cross-COW; non-duplicative to #3345/#3150/#1952.
//
//   AC1: aura_install_production_dirty_iterator() under production
//        flips reemit_provider_wired to 1; production snapshot +
//        hot-update-registry-stats query reflect the wired state.
//   AC2: push to ring + MutationBoundary + aura_reemit_aot_for_dirty
//        walks the production iterator (popped_total > 0,
//        last_reemit_dirty_count > 0) — the closed loop is now
//        candidate-driven, not skeleton-driven.
//   AC3: Soft / Off: install is no-op (returns 0); reemit_provider_wired
//        stays 0 (zero-cost contract preserved).
//   AC4: manual aura_set_reemit_candidate_fn from a test fixture
//        still wins — production install is overridable, no fight
//        with the existing test pattern.
//   AC5: production dirty ring counters + iter + push wired; the
//        bridge / header / stub / ctor / service_dirty paths all
//        reference #3373. No docs/design/3373-* per #1655.

#include "compiler/aura_jit_bridge.h"
#include "compiler/hot_update_registry.hh"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

extern "C" int aura_hot_update_in_mutation_boundary_for_reemit(void);
extern "C" void aura_hot_update_reset_reemit_boundary_handshake_for_test(void);
extern "C" std::uint64_t aura_reemit_dirty_count(void);
extern "C" std::uint64_t aura_reemit_success_count(void);
extern "C" int aura_install_production_dirty_iterator(void);
extern "C" int aura_production_dirty_ring_push(const char* name, std::uint64_t region,
                                               int from_closure_capture);
extern "C" void aura_production_dirty_ring_reset_for_test(void);
extern "C" std::uint64_t aura_production_dirty_ring_pushed_total(void);
extern "C" std::uint64_t aura_production_dirty_ring_dropped_total(void);
extern "C" std::uint64_t aura_production_dirty_ring_popped_total(void);
extern "C" std::uint64_t aura_production_dirty_ring_depth(void);

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

// Test helper: set production_defaults_active directly via the atomic
// (avoids pulling in apply_production_security_defaults which touches
// many other surfaces). Mirrors test_reemit_production_default_defer_v2.cpp.
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:hot-update-registry-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// AC1: under production, install flips reemit_provider_wired to 1.
static void ac1_production_install_wires() {
    std::println("\n--- AC1: production install wires iterator ---");
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_production_dirty_ring_reset_for_test();
    CHECK(aura_hot_update_reemit_provider_wired() == 0, "AC1: pre-install unwired");
    {
        ProdLockTestGuard prod(true);
        const int rc = aura_install_production_dirty_iterator();
        CHECK(rc == 1, "AC1: install returns 1 under production");
    }
    CHECK(aura_hot_update_reemit_provider_wired() == 1, "AC1: reemit_provider_wired == 1");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_provider_wired == 1, "AC1: snapshot.reemit_provider_wired == 1");
    // query:hot-update-registry-stats reflects the wired state.
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_production_dirty_ring_reset_for_test();
    {
        ProdLockTestGuard prod(true);
        (void)aura_install_production_dirty_iterator();
    }
    CompilerService cs;
    CHECK(href(cs, "reemit-provider-wired") == 1, "AC1: query reemit-provider-wired == 1");
}

// AC2: push to ring + Guard + aura_reemit_aot_for_dirty walks the
// production iterator. The candidate count bumps; popped_total > 0.
static void ac2_push_reemit_walks_iterator() {
    std::println("\n--- AC2: push + Guard + reemit walks production iterator ---");
    aura_production_dirty_ring_reset_for_test();
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    {
        ProdLockTestGuard prod(true);
        (void)aura_install_production_dirty_iterator();
    }
    CHECK(aura_hot_update_reemit_provider_wired() == 1, "AC2: iterator wired");
    // Region 0 = Default (always eligible). 1ULL<<1 == 2 is Evolution
    // and aura_reemit_aot_for_dirty permanently skips that region.
    CHECK(aura_production_dirty_ring_push("alpha_3373", 0, 0) == 1, "AC2: push alpha");
    CHECK(aura_production_dirty_ring_push("beta_3373", 0, 0) == 1, "AC2: push beta");
    CHECK(aura_production_dirty_ring_push("gamma_3373", 0, 0) == 1, "AC2: push gamma");
    CHECK(aura_production_dirty_ring_depth() == 3, "AC2: depth 3");
    CHECK(aura_production_dirty_ring_pushed_total() == 3, "AC2: pushed_total 3");
    const auto dirty_before = aura_reemit_dirty_count();
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(aura_hot_update_in_mutation_boundary_for_reemit() == 1, "AC2: inside Guard");
        // Reemit may return 0 (no aot_emit_fn wired under light test
        // harness) — but the iterator must walk and bump last_reemit_dirty_count.
        // The actual success path is gated on aot_emit_fn (separate
        // #1952 surface); #3373 closes the candidate-hose gap.
        const auto n = aura_reemit_aot_for_dirty(7);
        (void)n; // 0 without emit fn; check the iterator walked below.
        const auto dirty_after = aura_reemit_dirty_count();
        CHECK(dirty_after > dirty_before, "AC2: reemit walked candidates (dirty count bumped)");
    }
    CHECK(aura_production_dirty_ring_popped_total() >= 3, "AC2: ring popped all 3 candidates");
    CHECK(aura_production_dirty_ring_depth() == 0, "AC2: ring drained");
}

// AC3: Soft / Off: install is no-op (returns 0); reemit_provider_wired
// stays 0 (zero-cost contract preserved).
static void ac3_soft_off_install_noop() {
    std::println("\n--- AC3: Soft/Off install is no-op ---");
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_production_dirty_ring_reset_for_test();
    CHECK(aura_hot_update_reemit_provider_wired() == 0, "AC3: pre-install unwired");
    {
        ProdLockTestGuard prod(false);
        const int rc = aura_install_production_dirty_iterator();
        CHECK(rc == 0, "AC3: install returns 0 under Soft/Off");
    }
    CHECK(aura_hot_update_reemit_provider_wired() == 0, "AC3: reemit_provider_wired stays 0");
    auto snap = hot_update_registry().snapshot();
    CHECK(snap.reemit_provider_wired == 0, "AC3: snapshot still 0");
}

// AC4: manual aura_set_reemit_candidate_fn from a test fixture
// still wins — production install is overridable.
static void ac4_manual_set_wins() {
    std::println("\n--- AC4: manual set still wins ---");
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_production_dirty_ring_reset_for_test();
    {
        ProdLockTestGuard prod(true);
        (void)aura_install_production_dirty_iterator();
        CHECK(aura_hot_update_reemit_provider_wired() == 1, "AC4: production install wires");
    }
    // Test-side override: a static C ABI function that returns
    // a hardcoded candidate then null. Mirrors the existing test pattern.
    struct ManualFixture {
        std::size_t cursor = 0;
    } mf;
    static ManualFixture g_mf;
    g_mf.cursor = 0;
    auto manual_iter = +[](void* /*ud*/, const char** out_name, std::uint64_t* out_region,
                           bool* out_from) -> bool {
        if (g_mf.cursor > 0)
            return false;
        *out_name = "manual_fn_3373";
        *out_region = 0;
        *out_from = false;
        g_mf.cursor = 1;
        return true;
    };
    aura_set_reemit_candidate_fn(manual_iter, nullptr);
    CHECK(aura_hot_update_reemit_provider_wired() == 1, "AC4: wired stays 1 after manual set");
    // Production ring should NOT be popped by the manual iterator.
    aura_production_dirty_ring_reset_for_test();
    CHECK(aura_production_dirty_ring_depth() == 0, "AC4: production ring empty pre-reemit");
    aura_production_dirty_ring_push("ignored_3373", 1ULL << 1, 0);
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    CompilerService cs;
    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        g_mf.cursor = 0;
        const auto n = aura_reemit_aot_for_dirty(0);
        (void)n;
    }
    CHECK(aura_production_dirty_ring_popped_total() == 0,
          "AC4: manual iterator did not pop production ring (override works)");
}

// AC5: production dirty ring counters + iter + push wired; the
// bridge / header / stub / ctor / service_dirty paths all reference
// #3373. No docs/design/3373-* per #1655.
static void ac5_query_schema_and_source() {
    std::println("\n--- AC5: source-cite + ring counters + no docs/design/ ---");
    aura_production_dirty_ring_reset_for_test();
    CHECK(aura_production_dirty_ring_pushed_total() == 0, "AC5: pushed_total after reset");
    aura_production_dirty_ring_push("schema_3373", 1ULL << 1, 0);
    aura_production_dirty_ring_push("schema_3373", 1ULL << 1, 0);
    CHECK(aura_production_dirty_ring_pushed_total() == 2, "AC5: pushed_total == 2");
    CHECK(aura_production_dirty_ring_depth() == 2, "AC5: depth == 2");
    // Drop oldest on overflow test: push 256 more to force drop.
    for (int i = 0; i < 256; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "overflow_%d", i);
        aura_production_dirty_ring_push(buf, 1ULL << 1, 0);
    }
    CHECK(aura_production_dirty_ring_dropped_total() >= 1,
          "AC5: dropped_total bumps on overflow (cap 256, drop-oldest)");
    aura_production_dirty_ring_reset_for_test();
    const auto bridge = read_file("src/compiler/aura_jit_bridge.cpp");
    const auto hh = read_file("src/compiler/aura_jit_bridge.h");
    const auto stub = read_file("src/compiler/aura_jit_bridge_stub.cpp");
    const auto svc = read_file("src/compiler/service.ixx");
    const auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(bridge.find("#3373") != std::string::npos, "AC5: bridge cites #3373");
    CHECK(bridge.find("aura_install_production_dirty_iterator") != std::string::npos,
          "AC5: bridge defines install");
    CHECK(bridge.find("aura_production_dirty_ring_push") != std::string::npos,
          "AC5: bridge defines push");
    CHECK(bridge.find("production_dirty_candidate_iter") != std::string::npos,
          "AC5: bridge defines candidate iter");
    CHECK(bridge.find("kProductionDirtyRingCap") != std::string::npos,
          "AC5: bridge defines ring cap constant");
    CHECK(hh.find("#3373") != std::string::npos, "AC5: header cites #3373");
    CHECK(hh.find("aura_install_production_dirty_iterator") != std::string::npos,
          "AC5: header declares install");
    CHECK(stub.find("aura_install_production_dirty_iterator") != std::string::npos,
          "AC5: stub provides install");
    CHECK(stub.find("aura_production_dirty_ring_push") != std::string::npos,
          "AC5: stub provides push");
    CHECK(svc.find("aura_install_production_dirty_iterator") != std::string::npos,
          "AC5: ctor calls install");
    CHECK(svc.find("#3373") != std::string::npos, "AC5: ctor cites #3373");
    CHECK(dirty.find("aura_production_dirty_ring_push") != std::string::npos,
          "AC5: dirty path pushes to ring");
    CHECK(dirty.find("#3373") != std::string::npos, "AC5: dirty path cites #3373");
    const std::string design_path = "docs/design/3373-";
    CHECK(read_file((design_path + "production-dirty-ring.md").c_str()).empty(),
          "AC5: no docs/design/3373-* per #1655");
}

} // namespace

int run_test_reemit_production_provider_wired() {
    std::println("=== Issue #3373: production CompilerService wires reemit candidate iterator ===");
    ac1_production_install_wires();
    ac2_push_reemit_walks_iterator();
    ac3_soft_off_install_noop();
    ac4_manual_set_wins();
    ac5_query_schema_and_source();
    aura_set_reemit_candidate_fn(nullptr, nullptr);
    aura_production_dirty_ring_reset_for_test();
    aura_hot_update_reset_reemit_boundary_handshake_for_test();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_reemit_production_provider_wired();
}
#endif
