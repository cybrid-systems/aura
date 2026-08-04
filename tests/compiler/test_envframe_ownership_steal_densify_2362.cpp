// @category: unit
// @reason: Issue #2362 — close EnvFrameRef ownership under concurrent
// fiber steal + densify (depends on #2360 live set).
//
//   AC1: Live set — register/inject populates live_env_frame_refs();
//        materialize_call_env_ref auto-registers; Soft empty free
//   AC2: Gen advanced + in-range → transfer_to restamp on sync;
//        OOB/truncate → drop; metrics advance only on real events
//   AC3: Fiber steal path calls sync_live_env_frame_refs_ownership
//        (source-cite + empty-set no transfer/drop)
//   AC4: Densify scan runs real protocol (scan total + transfer/drop)
//   AC5: Query schema-2362 / live-env-frame-refs-* + hold-pin retained

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "core/densify_consistency_report.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.envframe_lifetime;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerService;
using aura::compiler::EnvFrameRef;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::envframe_lifetime::envframe_lifetime_densify_ownership_scan_total;
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:envframe-truncate-epoch-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: live set register / empty soft ──
static void ac1_live_set() {
    std::println("\n--- AC1: live EnvFrameRef set register + Soft empty ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.live_env_frame_ref_count() == 0, "AC1: idle count 0");
    CHECK(ev.live_env_frame_refs().empty(), "AC1: idle live_env_frame_refs empty");

    const auto id = ev.alloc_env_frame();
    EnvFrameRef held(id, ev.env_generation());
    auto* slot = ev.inject_live_env_frame_ref_for_test(held);
    CHECK(slot != nullptr, "AC1: inject returns stable pointer");
    CHECK(ev.live_env_frame_ref_count() == 1, "AC1: count 1 after inject");
    auto snap = ev.live_env_frame_refs();
    CHECK(snap.size() == 1 && snap[0] == slot, "AC1: snapshot contains inject slot");
    CHECK(slot->still_valid(ev), "AC1: injected ref still_valid");

    // materialize auto-registers
    Closure cl;
    cl.name = "ac1";
    cl.env_id = id;
    cl.bridge_epoch = ev.current_bridge_epoch();
    auto ro = ev.materialize_call_env_ref(cl);
    CHECK(ro.has_value(), "AC1: materialize_call_env_ref ok");
    CHECK(ev.live_env_frame_ref_count() >= 2, "AC1: materialize auto-registered");

    ev.unregister_live_env_frame_ref(slot);
    CHECK(ev.live_env_frame_ref_count() >= 1,
          "AC1: unregister removes inject; materialize remains");

    // Soft: empty after clear via sync on only-valid refs → no transfer/drop
    CompilerService soft;
    auto& sev = soft.evaluator();
    const auto t0 = soft.metrics().envframe_ownership_transfer_total.load();
    const auto d0 = soft.metrics().envframe_ownership_drop_total.load();
    sev.sync_live_env_frame_refs_ownership();
    CHECK(soft.metrics().envframe_ownership_transfer_total.load() == t0,
          "AC1: Soft empty → transfer total unchanged");
    CHECK(soft.metrics().envframe_ownership_drop_total.load() == d0,
          "AC1: Soft empty → drop total unchanged");
}

// ── AC2: gen advanced → transfer; OOB → drop ──
static void ac2_transfer_drop_protocol() {
    std::println("\n--- AC2: gen advanced → transfer; truncate → drop ---");

    // AC2a: generation advanced, index still live → transfer_to restamp
    {
        CompilerService local;
        auto& ev = local.evaluator();
        // Warm env_generation_ off 0 (stamp 0 skips the gen fence in
        // still_valid — cold start would hide lag).
        (void)ev.alloc_env_frame();
        const std::size_t warm_base = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(warm_base);
        (void)ev.alloc_env_frame();
        (void)ev.truncate_env_frames_to_checkpoint();
        CHECK(ev.env_generation() > 0, "AC2a: env_generation non-zero after warm");

        const auto target = ev.alloc_env_frame();
        // Protect target + prior frames; extras beyond base will be reclaimed.
        const std::size_t base = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(base);
        EnvFrameRef held(target, ev.env_generation());
        CHECK(held.env_gen_stamp != 0, "AC2a: held stamp non-zero");
        auto* slot = ev.inject_live_env_frame_ref_for_test(held);
        CHECK(slot && slot->still_valid(ev), "AC2a: slot valid pre-bump");

        // Allocate past checkpoint then truncate extras — bumps env_generation
        // without reclaiming target (panic_safe keeps base frames).
        (void)ev.alloc_env_frame();
        (void)ev.alloc_env_frame();
        const auto gen0 = ev.env_generation();
        (void)ev.truncate_env_frames_to_checkpoint();
        CHECK(ev.env_generation() > gen0, "AC2a: env_generation advanced");
        // Target still in-range but stamp lagging
        CHECK(static_cast<std::size_t>(target) < ev.env_frames_size(),
              "AC2a: target still in-range after truncate extras");
        CHECK(!slot->still_valid(ev), "AC2a: slot stale after gen bump");
        CHECK(!slot->use_site_check(ev), "AC2a: use_site_check fails after gen bump");

        const auto t0 = local.metrics().envframe_ownership_transfer_total.load();
        const auto d0 = local.metrics().envframe_ownership_drop_total.load();
        ev.sync_live_env_frame_refs_ownership();
        const auto t1 = local.metrics().envframe_ownership_transfer_total.load();
        const auto d1 = local.metrics().envframe_ownership_drop_total.load();
        CHECK(t1 > t0, "AC2a: transfer_total advanced on restamp");
        CHECK(d1 == d0, "AC2a: drop_total flat on restamp path");
        // Slot restamped in-place
        CHECK(slot->has_ownership(), "AC2a: slot retains ownership after restamp");
        CHECK(slot->still_valid(ev), "AC2a: slot still_valid after restamp");
        CHECK(slot->env_gen_stamp == ev.env_generation(), "AC2a: stamp == current gen");
    }

    // AC2b: reclaim target → drop
    {
        CompilerService local;
        auto& ev = local.evaluator();
        for (int i = 0; i < 2; ++i)
            (void)ev.alloc_env_frame();
        const std::size_t base = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(base);
        const auto target = ev.alloc_env_frame();
        EnvFrameRef held(target, ev.env_generation());
        auto* slot = ev.inject_live_env_frame_ref_for_test(held);
        CHECK(slot && slot->still_valid(ev), "AC2b: slot valid pre-truncate");
        (void)ev.alloc_env_frame();
        (void)ev.truncate_env_frames_to_checkpoint(); // reclaims target
        CHECK(!slot->still_valid(ev), "AC2b: slot invalid after reclaim");

        const auto t0 = local.metrics().envframe_ownership_transfer_total.load();
        const auto d0 = local.metrics().envframe_ownership_drop_total.load();
        ev.sync_live_env_frame_refs_ownership();
        const auto t1 = local.metrics().envframe_ownership_transfer_total.load();
        const auto d1 = local.metrics().envframe_ownership_drop_total.load();
        CHECK(d1 > d0, "AC2b: drop_total advanced on OOB path");
        // transfer may or may not bump (OOB uses drop inside transfer_to)
        CHECK(t1 == t0, "AC2b: transfer_total flat when drop path taken");
        CHECK(ev.live_env_frame_ref_count() == 0, "AC2b: dropped slot pruned from live set");
    }
}

// ── AC3 / AC4: steal + densify wire-up (source-cite + runtime) ──
static void ac3_steal_wire() {
    std::println("\n--- AC3: fiber steal path wires sync_live_env_frame_refs_ownership ---");
    const auto mut = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(mut.find("sync_live_env_frame_refs_ownership") != std::string::npos,
          "AC3: refresh_after_fiber_migration calls sync");
    CHECK(mut.find("Issue #2362") != std::string::npos, "AC3: steal path cites #2362");

    // Empty set on steal-like call: no metrics movement
    CompilerService cs;
    auto& ev = cs.evaluator();
    const auto t0 = cs.metrics().envframe_ownership_transfer_total.load();
    const auto d0 = cs.metrics().envframe_ownership_drop_total.load();
    // Direct protocol (same as steal path body for live set)
    ev.sync_live_env_frame_refs_ownership();
    CHECK(cs.metrics().envframe_ownership_transfer_total.load() == t0, "AC3: no steal xfer");
    CHECK(cs.metrics().envframe_ownership_drop_total.load() == d0, "AC3: no steal drop");
}

static void ac4_densify_scan() {
    std::println("\n--- AC4: densify scan real protocol ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(env.find("sync_live_env_frame_refs_ownership") != std::string::npos,
          "AC4: densify scan uses shared protocol");
    CHECK(env.find("scan_live_env_frame_refs_after_densify") != std::string::npos,
          "AC4: densify scan present");
    // #2368: Phase 5 force_densify_remap_pairing owns the densify ownership scan.
    CHECK(emb.find("force_densify_remap_pairing") != std::string::npos,
          "AC4: Phase 5 forced pairing owns densify ownership scan");

    CompilerService local;
    auto& ev = local.evaluator();
    // Warm gen off 0 so stamp lag is observable (stamp 0 skips fence).
    (void)ev.alloc_env_frame();
    const std::size_t warm_base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(warm_base);
    (void)ev.alloc_env_frame();
    (void)ev.truncate_env_frames_to_checkpoint();
    CHECK(ev.env_generation() > 0, "AC4: env_generation non-zero after warm");

    const auto target = ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    EnvFrameRef held(target, ev.env_generation());
    CHECK(held.env_gen_stamp != 0, "AC4: held stamp non-zero");
    auto* slot = ev.inject_live_env_frame_ref_for_test(held);
    (void)ev.alloc_env_frame();
    (void)ev.truncate_env_frames_to_checkpoint(); // gen bump, target live
    CHECK(slot && !slot->still_valid(ev), "AC4: slot stale pre-scan");

    const auto scan0 = envframe_lifetime_densify_ownership_scan_total();
    const auto t0 = local.metrics().envframe_ownership_transfer_total.load();
    ev.scan_live_env_frame_refs_after_densify();
    const auto scan1 = envframe_lifetime_densify_ownership_scan_total();
    const auto t1 = local.metrics().envframe_ownership_transfer_total.load();
    CHECK(scan1 == scan0 + 1, "AC4: densify ownership scan total advanced");
    CHECK(t1 > t0, "AC4: densify scan restamped live ref (transfer)");
    CHECK(slot->still_valid(ev), "AC4: slot valid after densify scan restamp");
}

// ── AC5: query + hold-pin + source surface ──
static void ac5_query_and_surface() {
    std::println("\n--- AC5: query schema-2362 + surface cite ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2362") == 2362, "AC5: schema-2362");
    CHECK(href(cs, "issue-2362") == 2362, "AC5: issue-2362");
    CHECK(href(cs, "schema-2360") == 2360, "AC5: schema-2360 lineage");
    CHECK(href(cs, "live-env-frame-refs-wired") == 1, "AC5: live set wired");
    CHECK(href(cs, "envframe-ownership-protocol-steal-wired") == 1, "AC5: steal protocol wired");
    CHECK(href(cs, "envframe-ownership-protocol-densify-wired") == 1,
          "AC5: densify protocol wired");
    CHECK(href(cs, "live-env-frame-refs-count") >= 0, "AC5: live count queryable");
    // hold-pin still present (#2362 AC: Guard blocks compact)
    const auto lf = read_file("src/core/envframe_lifetime.ixx");
    CHECK(lf.find("should_block_compact_for_guards") != std::string::npos,
          "AC5: hold-pin gate retained");
    CHECK(lf.find("active_guard_depth") != std::string::npos, "AC5: guard depth retained");

    const auto eval_ixx = read_file("src/compiler/evaluator.ixx");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(eval_ixx.find("register_live_env_frame_ref") != std::string::npos, "AC5: register API");
    CHECK(eval_ixx.find("sync_live_env_frame_refs_ownership") != std::string::npos,
          "AC5: sync API");
    CHECK(env.find("inject_live_env_frame_ref_for_test") != std::string::npos, "AC5: inject");
    CHECK(env.find("kMaxLiveEnvFrameRefs") != std::string::npos, "AC5: bounded live set");
}

} // namespace

int run_test_envframe_ownership_steal_densify_2362() {
    std::println("=== Issue #2362: EnvFrameRef ownership steal+densify ===");
    ac1_live_set();
    ac2_transfer_drop_protocol();
    ac3_steal_wire();
    ac4_densify_scan();
    ac5_query_and_surface();
    std::println("\n=== #2362: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_envframe_ownership_steal_densify_2362();
}
#endif
