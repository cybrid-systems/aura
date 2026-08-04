// Issue #2043 — Linear ownership post-mutate enforcement + GC/fiber
// atomic coordination under mutate_mtx_.
//
// AC1: source cites #2043; finalize_linear_gc_invalidation_window_ + lock order
// AC2: linear_ownership_epoch bumps on soft mark_define_dirty
// AC3: hard invalidate_function finalizes window under mutate
// AC4: query:linear-postmutate-fidelity-stats schema-2043 + window fields
// AC5: linear_post_mutate_enforcements / deopt_on_invalidate advance
// AC6: sustained soft+hard invalidate no crash; epoch monotonic
// AC7: C dual-write aura_get_linear_ownership_epoch tracks bumps
// AC8: lock_order_audit documents Mutate → linear+GC window

#include "test_harness.hpp"

#include "compiler/observability_metrics.h"
#include "compiler/runtime_shared.h"

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

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-postmutate-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

void ac1_source() {
    std::println("\n--- AC1: source cites #2043 ---");
    auto svc = read_file("src/compiler/service.ixx");
    auto dirty = read_file("src/compiler/service_dirty.cpp");
    auto lock = read_file("src/compiler/lock_order_audit.h");
    CHECK(!svc.empty() && svc.find("#2043") != std::string::npos, "service.ixx #2043");
    CHECK(svc.find("finalize_linear_gc_invalidation_window_") != std::string::npos, "finalize");
    CHECK(svc.find("bump_linear_ownership_epoch") != std::string::npos, "epoch bump");
    CHECK(dirty.find("finalize_linear_gc_invalidation_window_") != std::string::npos,
          "soft path finalize");
    CHECK(!lock.empty() && lock.find("#2043") != std::string::npos, "lock_order #2043");
    CHECK(lock.find("finalize_linear_gc_invalidation_window_") != std::string::npos,
          "lock_order documents window");
}

void ac2_soft_dirty_epoch() {
    std::println("\n--- AC2: soft mark_define_dirty bumps linear epoch ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto ep0 = cs.public_linear_ownership_epoch();
    const auto fin0 = cs.public_linear_gc_window_finalize_total();
    const auto bumps0 = cs.public_linear_ownership_epoch_bumps_total();
    cs.public_mark_define_dirty("f");
    const auto ep1 = cs.public_linear_ownership_epoch();
    const auto fin1 = cs.public_linear_gc_window_finalize_total();
    const auto bumps1 = cs.public_linear_ownership_epoch_bumps_total();
    std::println("  epoch {}→{} finalize {}→{} bumps {}→{}", ep0, ep1, fin0, fin1, bumps0, bumps1);
    CHECK(ep1 > ep0, "linear_ownership_epoch advanced");
    CHECK(fin1 > fin0, "finalize total advanced");
    CHECK(bumps1 > bumps0, "epoch bumps total advanced");
}

void ac3_hard_invalidate() {
    std::println("\n--- AC3: hard invalidate finalizes under mutate ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define a (lambda () 1)) (define b (lambda () (a)))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    const auto fin0 = m->linear_gc_window_finalize_total.load(std::memory_order_relaxed);
    const auto under0 = m->linear_gc_window_under_mutate_total.load(std::memory_order_relaxed);
    const auto deopt0 = m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
    const auto enf0 = m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
    cs.public_invalidate_function("a");
    const auto fin1 = m->linear_gc_window_finalize_total.load(std::memory_order_relaxed);
    const auto under1 = m->linear_gc_window_under_mutate_total.load(std::memory_order_relaxed);
    const auto deopt1 = m->linear_deopt_on_invalidate_total.load(std::memory_order_relaxed);
    const auto enf1 = m->linear_post_mutate_enforcements_total.load(std::memory_order_relaxed);
    std::println("  finalize {}→{} under_mutate {}→{} deopt {}→{} enforce {}→{}", fin0, fin1,
                 under0, under1, deopt0, deopt1, enf0, enf1);
    CHECK(fin1 > fin0, "finalize advanced");
    CHECK(under1 > under0, "window under mutate advanced");
    CHECK(deopt1 >= deopt0 + 1, "linear_deopt_on_invalidate advanced");
    CHECK(enf1 > enf0, "linear_post_mutate_enforcements advanced");
}

void ac4_query() {
    std::println("\n--- AC4: query fidelity-stats schema-2043 ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    cs.public_mark_define_dirty("g");
    auto h = cs.eval("(engine:metrics \"query:linear-postmutate-fidelity-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 800, "schema 800 lineage");
    CHECK(href(cs, "schema-2043") == 2043, "schema-2043");
    CHECK(href(cs, "issue-2043") == 2043, "issue-2043");
    CHECK(href(cs, "linear-gc-window-wired") == 1, "window wired");
    CHECK(href(cs, "linear-gc-window-finalize-total") >= 1, "finalize total in query");
    CHECK(href(cs, "linear-ownership-epoch-bumps") >= 1, "epoch bumps in query");
    CHECK(href(cs, "linear-ownership-epoch") >= 1, "epoch value in query");
}

void ac5_runtime_stats() {
    std::println("\n--- AC5: linear-ownership-runtime-stats ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define h (lambda (x) x))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto r0 = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
    CHECK(r0 && is_int(*r0), "runtime stats int");
    cs.public_invalidate_function("h");
    auto r1 = cs.eval("(engine:metrics \"query:linear-ownership-runtime-stats\")");
    CHECK(r1 && is_int(*r1), "runtime stats after");
    if (r0 && r1 && is_int(*r0) && is_int(*r1))
        CHECK(as_int(*r1) >= as_int(*r0), "runtime stats non-decreasing sum");
}

void ac6_sustained() {
    std::println("\n--- AC6: sustained soft+hard ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \""
                  "(define a (lambda (x) x))"
                  "(define b (lambda (x) (a x)))"
                  "\")")
              .has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    const auto ep0 = cs.public_linear_ownership_epoch();
    for (int i = 0; i < 16; ++i) {
        if (i % 2 == 0)
            cs.public_mark_define_dirty("a");
        else
            cs.public_invalidate_function("b");
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval mid");
    }
    const auto ep1 = cs.public_linear_ownership_epoch();
    std::println("  epoch after 16 ops: {}→{}", ep0, ep1);
    CHECK(ep1 >= ep0 + 16, "epoch advanced at least once per op");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after");
}

void ac7_c_dual_write() {
    std::println("\n--- AC7: C dual-write linear ownership epoch ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define k 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    cs.public_mark_define_dirty("k");
    const auto cpp_ep = cs.public_linear_ownership_epoch();
    const auto c_ep = aura_get_linear_ownership_epoch();
    std::println("  cpp_epoch={} c_epoch={}", cpp_ep, c_ep);
    CHECK(c_ep == cpp_ep, "C dual-write matches Evaluator epoch");
}

void ac8_public_finalize() {
    std::println("\n--- AC8: public finalize seam ---");
    CompilerService cs;
    const auto fin0 = cs.public_linear_gc_window_finalize_total();
    const auto ep0 = cs.public_linear_ownership_epoch();
    cs.public_finalize_linear_gc_window("manual");
    CHECK(cs.public_linear_gc_window_finalize_total() > fin0, "public finalize bumps");
    CHECK(cs.public_linear_ownership_epoch() > ep0, "public finalize bumps epoch");
}

} // namespace

int run_test_linear_gc_window() {
    std::println("=== Issue #2043: linear+GC atomic invalidation window ===");
    ac1_source();
    ac2_soft_dirty_epoch();
    ac3_hard_invalidate();
    ac4_query();
    ac5_runtime_stats();
    ac6_sustained();
    ac7_c_dual_write();
    ac8_public_finalize();
    if (g_failed)
        return 1;
    std::println("linear+GC window (#2043): OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_gc_window();
}
#endif
