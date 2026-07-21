// @category: unit
// @reason: Issue #1889 — truncate/compact dual-epoch + Guard consistency
// Issue #1842/#1889 (#1978 renamed): issue# moved from filename to header.
//
// AC1: truncate drops frames → bridge_epoch advances + metric
// AC2: Closure with post-checkpoint env_id is is_bridge_stale after truncate
// AC3: doomed closures get bridge_epoch=0 (defense-in-depth)
// AC4: query:envframe-truncate-epoch-stats schema 1889
// AC5: evaluator:compact-env-frames still Guard-wrapped (#1842/#1889)
// AC6: no-op truncate does not bump epoch / truncate metric

#include "test_harness.hpp"
#include "compiler/aura_jit_bridge.h"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:envframe-truncate-epoch-stats\") '{}')", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void ac1_truncate_bumps_epoch() {
    std::println("\n--- AC1: truncate drops → epoch + bump-on-truncate metric ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 4; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    for (int i = 0; i < 6; ++i)
        (void)ev.alloc_env_frame();

    const auto e0 = ev.current_bridge_epoch();
    const auto t0 =
        cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed);
    const auto b0 = cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed);

    CHECK(ev.truncate_env_frames_to_checkpoint() == 6, "dropped 6");
    CHECK(ev.current_bridge_epoch() == e0 + 1, "epoch +1");
    CHECK(cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed) ==
              t0 + 1,
          "bridge_epoch_bump_on_truncate_total +1");
    CHECK(cs.metrics().bridge_epoch_bumps_total.load(std::memory_order_relaxed) == b0 + 1,
          "bridge_epoch_bumps_total +1");
}

void ac2_stale_after_truncate() {
    std::println("\n--- AC2: post-checkpoint closure is bridge-stale after truncate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 3; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);

    // Allocate post-checkpoint frames and stamp a Closure as if captured then.
    const auto doomed_env = ev.alloc_env_frame();
    CHECK(doomed_env >= base, "env past checkpoint");

    Closure cl;
    cl.name = "cross-cow";
    cl.env_id = doomed_env;
    cl.bridge_epoch = ev.current_bridge_epoch(); // pre-truncate stamp
    CHECK(cl.bridge_epoch != 0 || ev.current_bridge_epoch() == 0, "epoch stamped or inactive");

    // Force tracking active by ensuring epoch can advance.
    const auto pre = cl.bridge_epoch;
    (void)ev.truncate_env_frames_to_checkpoint();
    const auto cur = ev.current_bridge_epoch();
    CHECK(cur > pre || (pre == 0 && cur == 0), "epoch advanced or both zero");
    // When service is bound, epoch is active (non-zero after first bump).
    if (cur != 0) {
        CHECK(ev.is_bridge_stale(pre, cur), "pre-truncate stamp is stale vs current");
        CHECK(ev.closure_is_epoch_or_env_stale(cl), "dual-check stale (epoch and/or OOB env)");
    }
    // OOB env_id must not resolve after truncate.
    CHECK(ev.resolve_env_frame(doomed_env) == nullptr, "doomed env OOB after truncate");
}

void ac3_doomed_closure_zeroed() {
    std::println("\n--- AC3: register doomed closure → bridge_epoch forced 0 ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 2; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    const auto doomed_env = ev.alloc_env_frame();

    Closure cl;
    cl.name = "doomed";
    cl.env_id = doomed_env;
    cl.bridge_epoch = 42; // non-zero pre-stamp
    const auto id = ev.register_active_closure(std::move(cl));

    const auto d0 =
        cs.metrics().envframe_truncate_doomed_closures_total.load(std::memory_order_relaxed);
    CHECK(ev.truncate_env_frames_to_checkpoint() >= 1, "dropped >=1");
    auto snap = ev.find_active_closure(id);
    CHECK(snap.has_value(), "closure still registered");
    CHECK(snap->bridge_epoch == 0, "doomed bridge_epoch forced 0");
    CHECK(cs.metrics().envframe_truncate_doomed_closures_total.load(std::memory_order_relaxed) >=
              d0 + 1,
          "doomed metric");
    CHECK(ev.is_bridge_stale(snap->bridge_epoch, ev.current_bridge_epoch()),
          "zero epoch is stale under active tracking");
}

void ac4_query(CompilerService& cs) {
    std::println("\n--- AC4: query:envframe-truncate-epoch-stats ---");
    auto h = cs.eval("(engine:metrics \"query:envframe-truncate-epoch-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1889, "schema 1889");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "truncate-bumps-bridge-epoch") == 1, "truncate-bumps-bridge-epoch");
    CHECK(href(cs, "compact-primitive-guarded") == 1, "compact-primitive-guarded");
    CHECK(href(cs, "bridge-epoch-bump-on-truncate") >= 0, "metric key");
}

void ac5_compact_guard_source() {
    std::println("\n--- AC5: compact primitive Guard (#1842/#1889) ---");
    std::string src;
    for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                          "../src/compiler/evaluator_primitives_compile.cpp"}) {
        src = read_file(p);
        if (!src.empty())
            break;
    }
    CHECK(!src.empty(), "read compile prims");
    // Prefer add("...") site — a doc comment also names the primitive earlier.
    auto pos = src.find("add(\"evaluator:compact-env-frames\"");
    if (pos == std::string::npos)
        pos = src.find("evaluator:compact-env-frames");
    CHECK(pos != std::string::npos, "found primitive");
    auto win = src.substr(pos, 800);
    CHECK(win.find("MutationBoundaryGuard") != std::string::npos ||
              win.find("run_under_mutation_guard") != std::string::npos,
          "Guard present");
    // Nearby comment cites #1842 / #1889 (search a bit earlier for the block).
    auto cite = src.substr(pos > 600 ? pos - 600 : 0, 900);
    CHECK(cite.find("#1842") != std::string::npos || cite.find("#1889") != std::string::npos,
          "cites Guard issue");
}

void ac6_noop() {
    std::println("\n--- AC6: no-op truncate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 3; ++i)
        (void)ev.alloc_env_frame();
    ev.set_panic_safe_env_frames_size_for_test(ev.env_frames_size());
    const auto e0 = ev.current_bridge_epoch();
    const auto t0 =
        cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed);
    CHECK(ev.truncate_env_frames_to_checkpoint() == 0, "no-op");
    CHECK(ev.current_bridge_epoch() == e0, "no epoch bump");
    CHECK(cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed) == t0,
          "no truncate metric bump");
}

} // namespace

int main() {
    std::println("=== Issue #1889: envframe truncate dual-epoch / Guard ===");
    ac1_truncate_bumps_epoch();
    ac2_stale_after_truncate();
    ac3_doomed_closure_zeroed();
    {
        CompilerService cs;
        ac4_query(cs);
    }
    ac5_compact_guard_source();
    ac6_noop();
    std::println("\n=== #1889: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
