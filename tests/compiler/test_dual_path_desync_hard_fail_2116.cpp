// @category: unit
// @reason: Issue #2116 — hard-fail dual-path desync in materialize_call_env
// + GCEnvWalkFn (no half-consistent EnvFrame in production default Hard).
//
// Policy:
//   Hard (default, mode=0): materialize returns empty Env; GC walk skips
//     desynced frame; dual_path_desync_hard_fail_total / gc_walk_skipped bump.
//   Soft (mode=1): legacy continue; dual_path_desync_soft_continue_total.
//
//   AC1: inject desync → hard path; metric++; materialize bindings empty
//   AC2: walk_env_frame_roots skips desynced frame (hard)
//   AC3: Soft mode continues; Hard default restored
//   AC4: query:envframe-dualpath-policy-stats schema-2116
//   AC5: src wiring + inject helper

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::Closure;
using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::EnvId;
using aura::compiler::Evaluator;
using aura::compiler::NULL_ENV_ID;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:envframe-dualpath-policy-stats\") \"{}\")", key));
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

static CompilerMetrics* metrics_of(Evaluator& ev) {
    return static_cast<CompilerMetrics*>(ev.compiler_metrics());
}

static void ac1_hard_materialize() {
    std::println("\n--- AC1: inject desync → hard materialize path ---");
    Evaluator::reset_envframe_dual_path_desync_mode_for_test();
    CHECK(Evaluator::envframe_dual_path_desync_is_hard(), "Hard default");

    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(ev);
    CHECK(m != nullptr, "metrics wired");

    const EnvId id = ev.alloc_env_frame();
    CHECK(id != NULL_ENV_ID, "alloc frame");
    // Empty frame is dual-path consistent (both paths empty).
    CHECK(ev.env_frame(id).ensure_dual_path_consistent(), "baseline consistent");

    // inject pushes only bindings_ → length desync vs bindings_symid_
    ev.inject_envframe_dual_path_desync_for_test(id);
    CHECK(!ev.env_frame(id).ensure_dual_path_consistent(), "inject desyncs");

    const auto hard0 = m->dual_path_desync_hard_fail_total.load();
    const auto panic0 = m->envframe_desync_panic_count_total.load();

    Closure cl;
    cl.env_id = id;
    cl.bridge_epoch = ev.current_bridge_epoch(); // avoid bridge-stale path
    auto ne = ev.materialize_call_env(cl);

    CHECK(ne.bindings().empty(), "hard path: empty string bindings");
    CHECK(ne.bindings_symid().empty(), "hard path: empty symid bindings");
    CHECK(m->dual_path_desync_hard_fail_total.load() == hard0 + 1, "hard_fail++");
    CHECK(m->envframe_desync_panic_count_total.load() == panic0 + 1, "desync panic++");
    CHECK(m->dual_path_desync_soft_continue_total.load() == 0, "no soft under Hard");
}

static void ac2_gc_walk_skip() {
    std::println("\n--- AC2: GCEnvWalkFn skips desynced frame ---");
    Evaluator::reset_envframe_dual_path_desync_mode_for_test();
    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(ev);
    CHECK(m != nullptr, "metrics");

    const EnvId id = ev.alloc_env_frame();
    // inject → bindings_ longer than bindings_symid_ → desync
    ev.inject_envframe_dual_path_desync_for_test(id);
    CHECK(!ev.env_frame(id).ensure_dual_path_consistent(), "desynced frame");

    const auto skip0 = m->dual_path_desync_gc_walk_skipped_total.load();
    const auto hard0 = m->dual_path_desync_hard_fail_total.load();
    std::vector<std::int64_t> pairs, clos;
    ev.walk_env_frame_roots(pairs, clos);
    CHECK(m->dual_path_desync_gc_walk_skipped_total.load() == skip0 + 1, "gc skip++");
    CHECK(m->dual_path_desync_hard_fail_total.load() == hard0 + 1, "hard on gc skip");
    // Desynced frame should not contribute roots (symid had only int, not pair)
    (void)pairs;
    (void)clos;
}

static void ac3_soft_mode() {
    std::println("\n--- AC3: Soft mode continues; Hard restored ---");
    Evaluator::set_envframe_dual_path_desync_mode(1);
    CHECK(!Evaluator::envframe_dual_path_desync_is_hard(), "Soft mode");

    CompilerService cs;
    auto& ev = cs.evaluator();
    auto* m = metrics_of(ev);
    const EnvId id = ev.alloc_env_frame();
    ev.inject_envframe_dual_path_desync_for_test(id);

    const auto soft0 = m->dual_path_desync_soft_continue_total.load();
    const auto hard0 = m->dual_path_desync_hard_fail_total.load();
    Closure cl;
    cl.env_id = id;
    cl.bridge_epoch = ev.current_bridge_epoch();
    auto ne = ev.materialize_call_env(cl);
    // Soft: continues with copy of (desynced) frame bindings
    CHECK(!ne.bindings().empty(), "soft path materializes string bindings");
    CHECK(m->dual_path_desync_soft_continue_total.load() == soft0 + 1, "soft++");
    CHECK(m->dual_path_desync_hard_fail_total.load() == hard0, "no hard under Soft");

    Evaluator::reset_envframe_dual_path_desync_mode_for_test();
    CHECK(Evaluator::envframe_dual_path_desync_is_hard(), "reset → Hard");
}

static void ac4_query() {
    std::println("\n--- AC4: query surface schema-2116 ---");
    Evaluator::reset_envframe_dual_path_desync_mode_for_test();
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "schema-2116") == 2116, "schema-2116");
    CHECK(href(cs, "issue-2116") == 2116, "issue-2116");
    CHECK(href(cs, "dual-path-desync-hard-wired") == 1, "wired");
    CHECK(href(cs, "dual-path-desync-hard-default") == 1, "hard default");
    CHECK(href(cs, "dual_path_desync_hard_fail_total") >= 0, "hard_fail key");
    CHECK(href(cs, "dual-path-desync-hard-fail-total") >= 0, "hard_fail alias");
    CHECK(href(cs, "dual-path-desync-policy-mode") == 0, "mode Hard");
}

static void ac5_source() {
    std::println("\n--- AC5: source wiring ---");
    auto env = read_file("src/compiler/evaluator_env.cpp");
    auto om = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
    CHECK(env.find("Issue #2116") != std::string::npos || env.find("#2116") != std::string::npos,
          "env cites #2116");
    CHECK(env.find("dual_path_desync_hard_fail_total") != std::string::npos, "materialize hard");
    CHECK(env.find("materialize-dual-desync") != std::string::npos, "dangling site");
    CHECK(env.find("dual_path_desync_gc_walk_skipped_total") != std::string::npos, "gc skip");
    CHECK(env.find("inject_envframe_dual_path_desync_for_test") != std::string::npos, "inject");
    CHECK(om.find("dual_path_desync_hard_fail_total") != std::string::npos, "metrics field");
    CHECK(q.find("schema-2116") != std::string::npos, "query schema");
    CHECK(q.find("query:envframe-dualpath-policy-stats") != std::string::npos, "policy query");
}

} // namespace

int run_test_dual_path_desync_hard_fail_2116() {
    std::println("=== Issue #2116: dual-path desync hard-fail ===");
    ac1_hard_materialize();
    ac2_gc_walk_skip();
    ac3_soft_mode();
    ac4_query();
    ac5_source();
    Evaluator::reset_envframe_dual_path_desync_mode_for_test();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_dual_path_desync_hard_fail_2116();
}
#endif
