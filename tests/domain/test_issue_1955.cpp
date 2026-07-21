// tests/domain/test_issue_1955.cpp — Wave 4 relocate from tests/test_issue_1955.cpp
// Prefer domain/; do not re-add under tests/ root. (#root_test_classification)
// @category: unit
// @reason: Issue #1955 — refine #1927 compact/truncate env_frames dual-epoch
// + MutationBoundaryGuard consistency under panic rollback.
//
//   AC1: schema-1955 / issue-1955 on query:envframe-truncate-epoch-stats
//   AC2: metrics bridge_epoch_bump_on_truncate_total +
//        mutation_boundary_violation_on_env_compact live
//   AC3: truncate drops frames → bridge_epoch + defuse_version advance
//   AC4: post-checkpoint closure dual-check stale / env OOB
//   AC5: compact primitive Guard-wrapped; source cites #1955
//   AC6: no-op truncate does not bump epoch; multi-round stress clean

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerService;
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

static void ac1_schema() {
    std::println("\n--- AC1: schema-1955 surface ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:envframe-truncate-epoch-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1889, "lineage 1889");
    CHECK(href(cs, "schema-1927") == 1927, "schema-1927 retained");
    CHECK(href(cs, "schema-1955") == 1955, "schema-1955");
    CHECK(href(cs, "issue-1955") == 1955, "issue-1955");
    CHECK(href(cs, "truncate-bumps-bridge-epoch") == 1, "bridge wired");
    CHECK(href(cs, "truncate-bumps-defuse-version") == 1, "defuse wired");
    CHECK(href(cs, "nested-guard-skip-wired") == 1, "nested skip");
    CHECK(href(cs, "compact-primitive-guarded") == 1, "compact guarded");
    CHECK(href(cs, "doomed-closure-restamp-wired") == 1, "doomed restamp");
    CHECK(href(cs, "schema-1948") == 1948, "1948 lineage");
    CHECK(href(cs, "schema-1739") == 1739, "1739 lineage");
}

static void ac2_metrics() {
    std::println("\n--- AC2: AC metrics live ---");
    CompilerService cs;
    CHECK(href(cs, "bridge_epoch_bump_on_truncate_total") >= 0, "bump metric");
    CHECK(href(cs, "mutation_boundary_violation_on_env_compact") >= 0, "compact viol");
    CHECK(href(cs, "mutation-boundary-violation-on-env-truncate") >= 0, "trunc viol");
    CHECK(href(cs, "truncate-doomed-closures") >= 0, "doomed metric");
}

static void ac3_dual_epoch() {
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
    CHECK(ev.defuse_version() > d0, "defuse_version advanced");
    CHECK(cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed) ==
              t0 + 1,
          "bump-on-truncate +1");
    CHECK(ev.env_frames_size() == base, "size at checkpoint");
}

static void ac4_stale_closure() {
    std::println("\n--- AC4: post-checkpoint closure stale ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    for (int i = 0; i < 3; ++i)
        (void)ev.alloc_env_frame();
    const std::size_t base = ev.env_frames_size();
    ev.set_panic_safe_env_frames_size_for_test(base);

    const auto doomed_env = ev.alloc_env_frame();
    CHECK(doomed_env >= base, "env past checkpoint");

    Closure cl;
    cl.name = "post-ckpt-1955";
    cl.env_id = doomed_env;
    cl.bridge_epoch = ev.current_bridge_epoch();
    const auto pre = cl.bridge_epoch;

    (void)ev.truncate_env_frames_to_checkpoint();
    const auto cur = ev.current_bridge_epoch();
    if (cur != 0) {
        CHECK(ev.is_bridge_stale(pre, cur), "pre-truncate stamp stale");
        cl.bridge_epoch = pre;
        CHECK(ev.closure_is_epoch_or_env_stale(cl), "dual-check stale");
    }
    CHECK(ev.resolve_env_frame(doomed_env) == nullptr, "doomed env OOB");
}

static void ac5_compact_and_source() {
    std::println("\n--- AC5: compact Guard + source #1955 ---");
    auto prim = read_first({"src/compiler/evaluator_primitives_compile.cpp",
                            "../src/compiler/evaluator_primitives_compile.cpp"});
    auto env = read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
    auto mut = read_first({"src/compiler/evaluator_primitives_mutate.cpp",
                           "../src/compiler/evaluator_primitives_mutate.cpp"});
    CHECK(!prim.empty() && prim.find("#1955") != std::string::npos, "prim cites #1955");
    auto pos = prim.find("add(\"evaluator:compact-env-frames\"");
    CHECK(pos != std::string::npos, "found compact add()");
    auto win = prim.substr(pos, 600);
    CHECK(win.find("run_under_mutation_guard") != std::string::npos, "Guard helper");
    CHECK(!env.empty() && env.find("#1955") != std::string::npos, "env cites #1955");
    CHECK(env.find("bridge_epoch_bump_on_truncate_total") != std::string::npos, "truncate metric");
    CHECK(!mut.empty() && mut.find("schema-1955") != std::string::npos, "query schema-1955");
}

static void ac6_noop_and_stress() {
    std::println("\n--- AC6: no-op + multi-round stress ---");
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

    // Multi-round: allocate past checkpoint, truncate, re-check schema.
    for (int round = 0; round < 20; ++round) {
        const std::size_t base = ev.env_frames_size();
        ev.set_panic_safe_env_frames_size_for_test(base);
        for (int i = 0; i < 3; ++i)
            (void)ev.alloc_env_frame();
        CHECK(ev.truncate_env_frames_to_checkpoint() == 3, "stress drop 3");
        CHECK(ev.env_frames_size() == base, "size restored");
    }
    CHECK(href(cs, "schema-1955") == 1955, "schema after stress");
    CHECK(cs.metrics().bridge_epoch_bump_on_truncate_total.load(std::memory_order_relaxed) >=
              t0 + 20,
          "bumps mono");
}

} // namespace

int main() {
    std::println("=== Issue #1955: envframe truncate/compact refine of #1927 ===");
    ac1_schema();
    ac2_metrics();
    ac3_dual_epoch();
    ac4_stale_closure();
    ac5_compact_and_source();
    ac6_noop_and_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
