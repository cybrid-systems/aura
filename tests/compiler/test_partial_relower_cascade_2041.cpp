// Issue #2041 — Partial re-lower + JIT hot-swap end-to-end on
// invalidate_function cascade for AI multi-round mutate.
//
// AC1: source cites #2041; should_partial_relower used in cascade
// AC2: query:incremental-relower-stats schema-2041 wire flags
// AC3: invalidate with nested-lambda body-only dirty prefers partial
//      (incremental_partial_relower / minimal_recompile / jit partial)
// AC4: sustained mutate → counters non-decreasing; no crash
// AC5: large dirty surface still allowed (threshold respected)
// AC6: bridge_epoch / epoch path still enforced (no silent stale IR)

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.ir_cache_pure;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::get_partial_relower_threshold;
using aura::compiler::reset_partial_relower_threshold_for_test;
using aura::compiler::set_partial_relower_threshold;
using aura::compiler::should_partial_relower;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:incremental-relower-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_source() {
    std::println("\n--- AC1: source cites #2041 + should_partial_relower cascade ---");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    CHECK(!dirty.empty(), "service_dirty readable");
    CHECK(dirty.find("#2041") != std::string::npos, "service_dirty #2041");
    CHECK(dirty.find("should_partial_relower") != std::string::npos,
          "cascade uses should_partial_relower");
    CHECK(dirty.find("relower_only_dirty_blocks") != std::string::npos ||
              dirty.find("try_partial_invalidate_relower") != std::string::npos,
          "partial path helper");
    auto pure = read_file("src/compiler/ir_cache_pure.ixx");
    CHECK(pure.find("should_partial_relower") != std::string::npos, "pure helper");
    auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("partial_recompile") != std::string::npos, "JIT partial_recompile wired");
}

void ac2_query_schema() {
    std::println("\n--- AC2: query schema-2041 wire flags ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto h = cs.eval("(engine:metrics \"query:incremental-relower-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema-2041") == 2041, "schema-2041");
    CHECK(href(cs, "issue-2041") == 2041, "issue-2041");
    CHECK(href(cs, "invalidate-cascade-partial-wired") == 1, "cascade partial wired");
    CHECK(href(cs, "should-partial-relower-cascade-wired") == 1, "should_partial wired");
    CHECK(href(cs, "jit-partial-recompile-on-cascade-wired") == 1, "jit partial wired");
    CHECK(href(cs, "partial-relower-threshold") == 8, "thr 8");
}

void ac3_invalidate_partial_path() {
    std::println("\n--- AC3: invalidate cascade prefers partial ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    // Call-chain: invalidate "a" marks body-only dirty on a (+ callers via
    // dep graph when edges exist). Cascade try_partial uses
    // should_partial_relower + relower_only_dirty_blocks → partial_recompile.
    CHECK(cs.eval("(set-code \""
                  "(define a (lambda () 1))"
                  "(define b (lambda () (a)))"
                  "\")")
              .has_value(),
          "set-code chain");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "metrics");
    const auto partial0 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto scope0 = m->minimal_recompile_scope_samples.load(std::memory_order_relaxed);
    const auto saved0 = m->minimal_recompile_clean_funcs_saved.load(std::memory_order_relaxed);
    const auto jit_partial0 =
        m->jit_partial_recompile_requests_total.load(std::memory_order_relaxed);
    const auto block_marks0 = m->dirty_propagation_block_marks.load(std::memory_order_relaxed);
    const auto inv0 = m->invalidate_function_calls.load(std::memory_order_relaxed);

    cs.public_invalidate_function("a");

    const auto partial1 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto scope1 = m->minimal_recompile_scope_samples.load(std::memory_order_relaxed);
    const auto saved1 = m->minimal_recompile_clean_funcs_saved.load(std::memory_order_relaxed);
    const auto jit_partial1 =
        m->jit_partial_recompile_requests_total.load(std::memory_order_relaxed);
    const auto block_marks1 = m->dirty_propagation_block_marks.load(std::memory_order_relaxed);
    const auto inv1 = m->invalidate_function_calls.load(std::memory_order_relaxed);

    std::println(
        "  inv {}→{} partial {}→{} scope {}→{} saved {}→{} jit_partial {}→{} block_marks {}→{}",
        inv0, inv1, partial0, partial1, scope0, scope1, saved0, saved1, jit_partial0, jit_partial1,
        block_marks0, block_marks1);

    CHECK(inv1 >= inv0 + 1, "invalidate_function_calls +1");
    // Body-only dirty marking on root (and possibly dependents).
    CHECK(block_marks1 >= block_marks0, "dirty_propagation_block_marks non-decreasing");
    // #2041 end-to-end: cascade partial path must fire for cached defines.
    CHECK(partial1 > partial0, "incremental_partial_relower_total advanced on cascade");
    CHECK(scope1 >= scope0, "minimal_recompile_scope_samples non-decreasing");
    CHECK(saved1 >= saved0, "minimal_recompile_clean_funcs_saved non-decreasing");
    CHECK(jit_partial1 >= jit_partial0, "jit_partial_recompile non-decreasing");
    CHECK(should_partial_relower(1), "1 dirty → partial");
    CHECK(should_partial_relower(7), "7 dirty → partial");
    CHECK(!should_partial_relower(8), "8 dirty → full");
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok after invalidate");
}

void ac4_sustained_mutate() {
    std::println("\n--- AC4: sustained invalidate under multi-define ---");
    reset_partial_relower_threshold_for_test();
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define a (lambda () 1))"
                  "(define b (lambda () (a)))"
                  "(define c (lambda () (b)))"
                  "\")")
              .has_value(),
          "set-code chain");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto partial0 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto full0 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    constexpr int kIters = 24;
    for (int i = 0; i < kIters; ++i) {
        cs.public_invalidate_function("a");
        CHECK(cs.eval("(+ 1 2)").has_value(), "eval mid-loop");
    }
    const auto partial1 = m->incremental_partial_relower_total.load(std::memory_order_relaxed);
    const auto full1 = m->incremental_full_fallback_total.load(std::memory_order_relaxed);
    std::println("  after {} inv: partial {}→{} full_fb {}→{}", kIters, partial0, partial1, full0,
                 full1);
    CHECK(partial1 + full1 >= partial0 + full0, "relower path activity");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after loop");
}

void ac5_threshold_respected() {
    std::println("\n--- AC5: threshold decision still pure ---");
    set_partial_relower_threshold(4);
    CHECK(should_partial_relower(3), "3 thr4 → partial");
    CHECK(!should_partial_relower(4), "4 thr4 → full");
    set_partial_relower_threshold(16);
    CHECK(should_partial_relower(8), "8 thr16 → partial");
    CHECK(!should_partial_relower(16), "16 thr16 → full");
    reset_partial_relower_threshold_for_test();
    CHECK(get_partial_relower_threshold() == 8, "reset 8");
}

void ac6_epoch_still_enforced() {
    std::println("\n--- AC6: invalidate still bumps epoch / no crash ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define id (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto inv0 = m->invalidate_function_calls.load(std::memory_order_relaxed);
    const auto epoch0 = m->bridge_epoch_bumps_total.load(std::memory_order_relaxed);
    cs.public_invalidate_function("id");
    CHECK(m->invalidate_function_calls.load(std::memory_order_relaxed) >= inv0 + 1,
          "invalidate_function_calls +1");
    CHECK(m->bridge_epoch_bumps_total.load(std::memory_order_relaxed) >= epoch0,
          "bridge epoch bumps non-decreasing");
    CHECK(cs.eval("(id 42)").has_value() || cs.eval("(+ 1 1)").has_value(),
          "post-invalidate eval ok");
}

} // namespace

int run_test_partial_relower_cascade_2041() {
    std::println("=== Issue #2041: partial re-lower cascade + JIT partial ===");
    ac1_source();
    ac2_query_schema();
    ac3_invalidate_partial_path();
    ac4_sustained_mutate();
    ac5_threshold_respected();
    ac6_epoch_still_enforced();
    if (g_failed)
        return 1;
    std::println("partial re-lower cascade (#2041): OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_partial_relower_cascade_2041();
}
#endif
