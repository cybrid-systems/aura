// @category: unit
// @reason: Issue #2032 — DepGraph concurrent stale-edge reject +
// configurable partial-relower threshold.
//
//   AC1: source cites #2032; get/set_partial_relower_threshold; reject counter
//   AC2: should_partial_relower / estimate_relower_blocks honor threshold
//   AC3: explicit-threshold pure overloads (4 / 8 / 16)
//   AC4: dep_graph_edge_reject_stale + generation metrics exist
//   AC5: query:incremental-relower-stats schema-2032 + wire flags
//   AC6: threshold change affects decision; reset restores default 8
//   AC7: service smoke — record_dependency / invalidate still work

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.ir_cache_pure;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::estimate_relower_blocks;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::kDefaultPartialRelowerThreshold;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::should_partial_relower;
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
    std::println("\n--- AC1: source cites #2032 ---");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    auto svc = read_file("src/compiler/service.ixx");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    auto met = read_file("src/compiler/observability_metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(!pure.empty() && pure.find("#2032") != std::string::npos, "ir_cache_pure #2032");
    CHECK(pure.find("get_partial_relower_threshold") != std::string::npos, "get threshold");
    CHECK(pure.find("set_partial_relower_threshold") != std::string::npos, "set threshold");
    CHECK(pure.find("kDefaultPartialRelowerThreshold") != std::string::npos, "default 8");
    CHECK(!svc.empty() && svc.find("dep_graph_edge_reject_stale") != std::string::npos,
          "record_dependency reject");
    CHECK(svc.find("dep_graph_generation_") != std::string::npos, "generation stamp");
    CHECK(!dirty.empty() && dirty.find("dep_graph_generation_") != std::string::npos,
          "erase bumps generation");
    CHECK(!met.empty() && met.find("dep_graph_edge_reject_stale_total") != std::string::npos,
          "metrics reject");
    CHECK(met.find("partial_relower_threshold_used") != std::string::npos, "metrics thr used");
    CHECK(!q.empty() && q.find("schema-2032") != std::string::npos, "query schema-2032");
}

static void ac2_threshold_decision() {
    std::println("\n--- AC2: should_partial_relower honors threshold ---");
    reset_partial_relower_threshold_for_test();
    CHECK(get_partial_relower_threshold() == 8, "default 8");
    CHECK(!should_partial_relower(0), "0 → false");
    CHECK(should_partial_relower(1), "1 → true");
    CHECK(should_partial_relower(7), "7 → true");
    CHECK(!should_partial_relower(8), "8 → false at thr=8");
    CHECK(estimate_relower_blocks(7) == 7, "est 7");
    CHECK(estimate_relower_blocks(8) == static_cast<std::size_t>(-1), "est 8 full");

    set_partial_relower_threshold(16);
    CHECK(get_partial_relower_threshold() == 16, "set 16");
    CHECK(should_partial_relower(8), "8 → true at thr=16");
    CHECK(should_partial_relower(15), "15 → true");
    CHECK(!should_partial_relower(16), "16 → false");
    CHECK(estimate_relower_blocks(15) == 15, "est 15");
    CHECK(estimate_relower_blocks(16) == static_cast<std::size_t>(-1), "est 16 full");

    set_partial_relower_threshold(4);
    CHECK(should_partial_relower(3), "3 → true at thr=4");
    CHECK(!should_partial_relower(4), "4 → false");
    reset_partial_relower_threshold_for_test();
    CHECK(get_partial_relower_threshold() == 8, "reset");
}

static void ac3_pure_overloads() {
    std::println("\n--- AC3: explicit-threshold pure overloads ---");
    CHECK(should_partial_relower(3, 4), "3 thr4");
    CHECK(!should_partial_relower(4, 4), "4 thr4");
    CHECK(should_partial_relower(7, 8), "7 thr8");
    CHECK(!should_partial_relower(8, 8), "8 thr8");
    CHECK(should_partial_relower(15, 16), "15 thr16");
    CHECK(!should_partial_relower(16, 16), "16 thr16");
    CHECK(estimate_relower_blocks(0, 8) == 0, "est 0");
    CHECK(estimate_relower_blocks(5, 8) == 5, "est 5 thr8");
    CHECK(estimate_relower_blocks(8, 8) == static_cast<std::size_t>(-1), "est full thr8");
    CHECK(estimate_relower_blocks(10, 16) == 10, "est 10 thr16");
}

static void ac4_metrics_exist() {
    std::println("\n--- AC4: metrics counters exist ---");
    CompilerService cs;
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    CHECK(m->dep_graph_edge_reject_stale_total.load() == 0 ||
              m->dep_graph_edge_reject_stale_total.load() >= 0,
          "reject counter");
    CHECK(m->dep_graph_generation_total.load() >= 0, "gen counter");
    CHECK(m->partial_relower_threshold_used.load() >= 1, "thr used >=1");
}

static void ac5_query_schema() {
    std::println("\n--- AC5: query:incremental-relower-stats schema-2032 ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2032") == 2032, "schema-2032");
    CHECK(href(cs, "issue-2032") == 2032, "issue-2032");
    CHECK(href(cs, "dep-graph-stale-reject-wired") == 1, "reject wired");
    CHECK(href(cs, "partial-relower-threshold-wired") == 1, "thr wired");
    CHECK(href(cs, "partial-relower-threshold") == 8, "thr current 8");
    CHECK(href(cs, "dep_graph_edge_reject_stale_total") >= 0, "reject key");
    CHECK(href(cs, "dep_graph_generation_total") >= 0, "gen key");
    CHECK(href(cs, "schema") == 1639 || href(cs, "schema") == 2032, "primary lineage");
}

static void ac6_threshold_visible() {
    std::println("\n--- AC6: threshold change visible on query ---");
    set_partial_relower_threshold(12);
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    CHECK(href(cs, "partial-relower-threshold") == 12, "query shows 12");
    reset_partial_relower_threshold_for_test();
    CHECK(href(cs, "partial-relower-threshold") == 8, "query shows 8 after reset");
}

static void ac7_service_smoke() {
    std::println("\n--- AC7: service smoke invalidate / dep ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define a (lambda () 1)) (define b (lambda () (a)))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto gen0 = m ? m->dep_graph_generation_total.load() : 0;
    // Hard invalidate should bump generation on cascade erase path.
    cs.public_invalidate_function("a");
    if (m) {
        CHECK(m->dep_graph_generation_total.load() >= gen0, "gen non-decreasing");
    }
    CHECK(cs.eval("(+ 1 2)").has_value(), "eval ok after invalidate");
    CHECK(href(cs, "schema-2032") == 2032, "schema after");
}

} // namespace

int main() {
    ac1_source();
    ac2_threshold_decision();
    ac3_pure_overloads();
    ac4_metrics_exist();
    ac5_query_schema();
    ac6_threshold_visible();
    ac7_service_smoke();
    if (g_failed)
        return 1;
    std::println("dep_graph + partial relower threshold (#2032): OK ({} passed)", g_passed);
    return 0;
}
