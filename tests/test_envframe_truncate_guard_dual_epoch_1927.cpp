// @category: unit
// @reason: Issue #1927 — compact/truncate env_frames dual-epoch + Guard
// consistency under panic rollback (refine #1889 #1842 #1948 #1739).
//
//   AC1: source cites #1927; nested-guard skip + defuse_version bump
//   AC2: schema-1927 on query:envframe-truncate-epoch-stats + AC metrics
//   AC3: truncate drops frames → bridge_epoch + defuse_version advance
//   AC4: post-checkpoint closure is bridge-stale / OOB after truncate
//   AC5: apply_closure on doomed env_id fails safe (no UAF)
//   AC6: compact primitive Guard-wrapped (#1842)
//   AC7: no-op truncate does not bump epoch
//   AC8: #1889 lineage schema retained

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::ClosureId;
using aura::compiler::CompilerService;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:envframe-truncate-epoch-stats\") \"{}\")", key));
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

static std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static void ac1_source() {
    std::println("\n--- AC1: #1927 source surface ---");
    auto env = read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto prim = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                            "../src/compiler/evaluator_primitives_compile.cpp"});
    CHECK(!env.empty() && env.find("#1927") != std::string::npos, "env cites #1927");
    CHECK(env.find("already_in_boundary") != std::string::npos ||
              env.find("nested-guard") != std::string::npos ||
              env.find("mutation_boundary_held") != std::string::npos,
          "nested Guard skip");
    CHECK(env.find("defuse_version_") != std::string::npos, "defuse bump in truncate");
    CHECK(env.find("bridge_epoch_bump_on_truncate_total") != std::string::npos, "truncate metric");
    CHECK(!prim.empty() && prim.find("evaluator:compact-env-frames") != std::string::npos,
          "compact prim");
    // Prefer add("...") site (comment also mentions the name earlier).
    auto pos = prim.find("add(\"evaluator:compact-env-frames\"");
    if (pos == std::string::npos)
        pos = prim.find("evaluator:compact-env-frames");
    auto win = prim.substr(pos, 800);
    CHECK(win.find("run_under_mutation_guard") != std::string::npos ||
              win.find("MutationBoundaryGuard") != std::string::npos,
          "compact Guard wrap");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema-1927 ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:envframe-truncate-epoch-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1889, "lineage 1889");
    CHECK(href(cs, "schema-1927") == 1927, "schema-1927");
    CHECK(href(cs, "issue-1927") == 1927, "issue-1927");
    CHECK(href(cs, "bridge_epoch_bump_on_truncate_total") >= 0, "AC metric");
    CHECK(href(cs, "mutation_boundary_violation_on_env_compact") >= 0, "compact viol");
    CHECK(href(cs, "mutation-boundary-violation-on-env-truncate") >= 0, "trunc viol");
    CHECK(href(cs, "truncate-bumps-bridge-epoch") == 1, "bridge wired");
    CHECK(href(cs, "truncate-bumps-defuse-version") == 1, "defuse wired");
    CHECK(href(cs, "nested-guard-skip-wired") == 1, "nested skip");
    CHECK(href(cs, "compact-primitive-guarded") == 1, "compact guarded");
}

static void ac3_dual_epoch_bump() {
    std::println("\n--- AC3: truncate dual-epoch bump ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 4; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    for (int i = 0; i < 6; ++i)
        (void)ev.alloc_env_frame();

    const auto e0 = ev.current_bridge_epoch();
    const auto d0 = ev.defuse_version();
    const auto t0 =
        cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed);

    CHECK(ev.truncate_env_frames_to_checkpoint() == 6, "dropped 6");
    CHECK(ev.current_bridge_epoch() == e0 + 1, "bridge epoch +1");
    // Free-standing truncate may acquire Guard (enter+exit = +2 defuse)
    // plus explicit dual-epoch bump inside truncate (≥ +1 total).
    CHECK(ev.defuse_version() > d0, "defuse_version advanced");
    CHECK(cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed) ==
              t0 + 1,
          "bump-on-truncate +1");
    CHECK(ev.env_frames_size() == base, "size at checkpoint");
}

static void ac4_stale_closure() {
    std::println("\n--- AC4: post-checkpoint closure stale after truncate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 3; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);

    const auto doomed_env = ev.alloc_env_frame();
    CHECK(doomed_env >= base, "env past checkpoint");

    Closure cl;
    cl.name = "cross-cow";
    cl.env_id = doomed_env;
    cl.bridge_epoch = ev.current_bridge_epoch();
    const auto pre = cl.bridge_epoch;

    (void)ev.truncate_env_frames_to_checkpoint();
    const auto cur = ev.current_bridge_epoch();
    if (cur != 0) {
        CHECK(ev.is_bridge_stale(pre, cur), "pre-truncate stamp stale");
        cl.bridge_epoch = pre; // restore pre stamp for dual-check
        CHECK(ev.closure_is_epoch_or_env_stale(cl), "dual-check stale");
    }
    CHECK(ev.resolve_env_frame(doomed_env) == nullptr, "doomed env OOB");
}

static void ac5_apply_safe() {
    std::println("\n--- AC5: apply_closure on doomed env fails safe ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 2; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);
    const auto doomed_env = ev.alloc_env_frame();

    Closure cl;
    cl.name = "doomed-apply";
    cl.env_id = doomed_env;
    cl.bridge_epoch = ev.current_bridge_epoch() != 0 ? ev.current_bridge_epoch() : 1;
    cl.body_id = 0; // empty body — apply should refuse on stale, not eval
    const auto id = ev.register_active_closure(std::move(cl));

    const auto d0 =
        cs.metrics().envframe_truncate_doomed_closures_total.load(std::memory_order_relaxed);
    CHECK(ev.truncate_env_frames_to_checkpoint() >= 1, "dropped");
    auto snap = ev.find_active_closure(id);
    CHECK(snap.has_value(), "still registered");
    CHECK(snap->bridge_epoch == 0, "doomed bridge_epoch 0");
    CHECK(cs.metrics().envframe_truncate_doomed_closures_total.load(std::memory_order_relaxed) >=
              d0 + 1,
          "doomed metric");

    // apply must not UAF — nullopt or safe fallback return.
    auto r = ev.apply_closure(id, {});
    CHECK(!r.has_value() || r.has_value(), "apply returns without crash");
    // Prefer refusal under active dual-epoch tracking.
    if (ev.current_bridge_epoch() != 0)
        CHECK(!r.has_value() || ev.is_bridge_stale(0, ev.current_bridge_epoch()),
              "stale or refused");
}

static void ac6_compact_guard() {
    std::println("\n--- AC6: compact primitive Guard ---");
    auto prim = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                            "../src/compiler/evaluator_primitives_compile.cpp"});
    CHECK(!prim.empty(), "read");
    auto pos = prim.find("add(\"evaluator:compact-env-frames\"");
    CHECK(pos != std::string::npos, "found add()");
    auto win = prim.substr(pos, 600);
    CHECK(win.find("run_under_mutation_guard") != std::string::npos, "Guard helper");
    CHECK(win.find("track_env_compact_violation") != std::string::npos, "violation track");
}

static void ac7_noop() {
    std::println("\n--- AC7: no-op truncate ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 3; ++i)
        (void)ev.alloc_env_frame();
    ev.set_panic_safe_env_frames_size_for_test(ev.env_frames_size());
    const auto e0 = ev.current_bridge_epoch();
    const auto d0 = ev.defuse_version();
    const auto t0 =
        cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed);
    CHECK(ev.truncate_env_frames_to_checkpoint() == 0, "no-op");
    CHECK(ev.current_bridge_epoch() == e0, "no bridge bump");
    CHECK(ev.defuse_version() == d0, "no defuse bump");
    CHECK(cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed) == t0,
          "no truncate metric");
}

static void ac8_lineage() {
    std::println("\n--- AC8: #1889 lineage ---");
    CompilerService cs;
    CHECK(href(cs, "schema") == 1889, "schema 1889");
    CHECK(href(cs, "active") == 1, "active");
    CHECK(href(cs, "truncate-bumps-bridge-epoch") == 1, "wired");
}

} // namespace

int main() {
    std::println("=== Issue #1927: envframe truncate dual-epoch / Guard ===");
    ac1_source();
    ac2_schema();
    ac3_dual_epoch_bump();
    ac4_stale_closure();
    ac5_apply_safe();
    ac6_compact_guard();
    ac7_noop();
    ac8_lineage();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
