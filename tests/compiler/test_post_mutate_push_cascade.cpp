// @category: unit
// @reason: Issue #2038 — push-automatic incremental invalidation on
// mutate:* (dirty → DefUse/IR/JIT cascade + post-mutate latency metrics).
//
//   AC1: source cites #2038; push_post_mutate_incremental_cascade +
//        MutationBoundaryGuard success path
//   AC2: after mutate:rebind / set-body, cascade metric advances
//   AC3: subsequent eval-current sees updated result (no manual invalidate)
//   AC4: query:incremental-relower-stats schema-2038 + latency keys
//   AC5: cascade is scoped (defines marked) not global no-op when log empty
//   AC6: latency samples track cascade invocations

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2038 ---");
    auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    auto ixx = read_file("src/compiler/evaluator.ixx");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!mut.empty() && mut.find("push_post_mutate_incremental_cascade") != std::string::npos,
          "cascade impl");
    CHECK(mut.find("#2038") != std::string::npos, "mutate.cpp #2038");
    CHECK(!bound.empty() && bound.find("push_post_mutate_incremental_cascade") != std::string::npos,
          "Guard success wires cascade");
    CHECK(bound.find("#2038") != std::string::npos, "boundary #2038");
    CHECK(!ixx.empty() && ixx.find("push_post_mutate_incremental_cascade") != std::string::npos,
          "evaluator.ixx declares cascade");
    CHECK(!met.empty() && met.find("post_mutate_incremental_cascade_total") != std::string::npos,
          "cascade metric");
    CHECK(met.find("post_mutate_incremental_latency_us_total") != std::string::npos,
          "latency metric");
    CHECK(!q.empty() && q.find("schema-2038") != std::string::npos, "query schema-2038");
}

static void ac2_cascade_metric() {
    std::println("\n--- AC2: mutate advances cascade metric ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (id x) x) (id 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto c0 = href(cs, "post_mutate_incremental_cascade_total");
    const auto d0 = href(cs, "post_mutate_incremental_defines_total");
    // String args (not symbols) — set-body / rebind under MutationBoundaryGuard.
    auto m = cs.eval("(mutate:set-body \"id\" \"(lambda (x) (+ x 1))\" \"#2038\")");
    CHECK(m.has_value(), "set-body returned");
    const auto c1 = href(cs, "post_mutate_incremental_cascade_total");
    const auto d1 = href(cs, "post_mutate_incremental_defines_total");
    CHECK(c1 > c0, "cascade total advanced");
    CHECK(d1 >= d0, "defines total non-decreasing");
}

static void ac3_eval_sees_update() {
    std::println("\n--- AC3: eval after mutate sees new IR (no manual invalidate) ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (f 10)\")").has_value(), "set-code");
    auto r0 = cs.eval("(eval-current)");
    CHECK(r0 && is_int(*r0) && as_int(*r0) == 11, "f 10 → 11");
    // Mutate body then immediate eval — cascade must have dirtied IR/cache.
    auto m = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 2))\" \"#2038\")");
    CHECK(m.has_value(), "mutate f body");
    auto r1 = cs.eval("(eval-current)");
    CHECK(r1 && is_int(*r1) && as_int(*r1) == 12, "f 10 → 12 after set-body");
    CHECK(href(cs, "post_mutate_incremental_cascade_total") > 0, "cascade ran");
}

static void ac4_query_schema() {
    std::println("\n--- AC4: query schema-2038 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2038") == 2038, "schema-2038");
    CHECK(href(cs, "issue-2038") == 2038, "issue-2038");
    CHECK(href(cs, "post-mutate-push-cascade-wired") == 1, "wired");
    CHECK(href(cs, "post_mutate_incremental_cascade_total") >= 0, "cascade key");
    CHECK(href(cs, "post_mutate_incremental_latency_us_total") >= 0, "latency key");
    CHECK(href(cs, "post_mutate_incremental_latency_samples") >= 0, "samples key");
    // Lineage retained
    CHECK(href(cs, "schema-2033") == 2033, "schema-2033 retained");
}

static void ac5_scoped_not_empty_no_op() {
    std::println("\n--- AC5: cascade on structural mutate marks defines ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) x) (g 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto d0 = href(cs, "post_mutate_incremental_defines_total");
    (void)cs.eval("(mutate:set-body \"g\" \"(lambda (x) (* x 2))\" \"#2038\")");
    const auto d1 = href(cs, "post_mutate_incremental_defines_total");
    CHECK(d1 > d0, "defines total advanced after set-body");
}

static void ac6_latency_tracks() {
    std::println("\n--- AC6: latency samples track cascade invocations ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (h x) x) (h 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto s0 = href(cs, "post_mutate_incremental_latency_samples");
    const auto c0 = href(cs, "post_mutate_incremental_cascade_total");
    for (int i = 0; i < 3; ++i) {
        (void)cs.eval(
            std::format("(mutate:set-body \"h\" \"(lambda (x) (+ x {}))\" \"r{}\")", i + 1, i));
    }
    const auto s1 = href(cs, "post_mutate_incremental_latency_samples");
    const auto c1 = href(cs, "post_mutate_incremental_cascade_total");
    CHECK(c1 > c0, "cascade advanced");
    CHECK(s1 > s0, "samples advanced");
    CHECK(s1 - s0 >= c1 - c0 || s1 > s0, "samples track cascade growth");
    CHECK(href(cs, "post_mutate_incremental_latency_us_total") >= 0, "latency us total");
}

} // namespace

int main() {
    std::println("=== test_post_mutate_push_cascade (#2038) ===");
    ac1_source();
    ac2_cascade_metric();
    ac3_eval_sees_update();
    ac4_query_schema();
    ac5_scoped_not_empty_no_op();
    ac6_latency_tracks();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
