// test_prompt6_linear_jit_l2_post_invalidate_arena_gc.cpp — Issue #740:
// Linear ownership + GuardShape metadata driving post-invalidate
// Arena/DropOp / GC root re-sync in JIT L2 hot paths.
//
// Non-duplicative with #719 (runtime guard), #720 (metadata), #658, #657.
//
//   - AC1: query:linear-jit-safety-stats reachable (schema 740)
//   - AC2: invalidate triggers gc-root-resync + post-invalidate path
//   - AC3: linear workspace eval remains safe post-invalidate
//   - AC4: metrics monotonic over mutate + invalidate cycle
//   - AC5: query regression (linear-ownership-enforcement-stats)

#include "test_harness.hpp"

#include <cstdint>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace aura_issue_740_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;

static constexpr const char* k_linear_prog = R"(
(define f (lambda () (let ((x (Linear 42))) (move x))))
)";

static std::int64_t jit_hash(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:linear-jit-safety-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t jit_sum(CompilerService& cs) {
    const auto arena = jit_hash(cs, "arena-forced-post-mutate");
    const auto drop = jit_hash(cs, "drop-op-emitted");
    const auto gc = jit_hash(cs, "gc-root-resync");
    if (arena < 0 || drop < 0 || gc < 0)
        return -1;
    return arena + drop + gc;
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:linear-jit-safety-stats (schema 740) ---");
    auto h = cs.eval("(query:linear-jit-safety-stats)");
    CHECK(h && is_hash(*h), "linear-jit-safety-stats returns hash");
    CHECK(jit_hash(cs, "schema") == 740, "schema == 740");
    CHECK(jit_hash(cs, "arena-forced-post-mutate") >= 0, "arena-forced-post-mutate present");
    CHECK(jit_hash(cs, "drop-op-emitted") >= 0, "drop-op-emitted present");
    CHECK(jit_hash(cs, "gc-root-resync") >= 0, "gc-root-resync present");

    const auto gc0 = jit_hash(cs, "gc-root-resync");
    const auto post0 =
        cs.metrics().linear_jit_post_invalidate_total.load(std::memory_order_relaxed);

    std::println("\n--- AC2: setup linear define + invalidate ---");
    CHECK(cs.eval(std::string("(set-code \"") + k_linear_prog + "\")").has_value(),
          "set-code linear define");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current linear define");
    cs.public_invalidate_function("f");
    const auto gc1 = jit_hash(cs, "gc-root-resync");
    const auto post1 =
        cs.metrics().linear_jit_post_invalidate_total.load(std::memory_order_relaxed);
    std::println("  gc-root-resync: {} -> {} post-invalidate: {} -> {}", gc0, gc1, post0, post1);
    CHECK(gc1 > gc0, "gc-root-resync grew after invalidate");
    CHECK(post1 > post0, "linear_jit_post_invalidate_total grew after invalidate");

    std::println("\n--- AC3: post-invalidate eval safe ---");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after invalidate");
    auto r = cs.eval("(f)");
    CHECK(r.has_value(), "(f) applies after invalidate");

    std::println("\n--- AC4: mutate + invalidate metrics monotonic ---");
    const auto sum4a = jit_sum(cs);
    (void)cs.eval("(mutate:rebind \"f\" \"(lambda () (let ((x (Linear 99))) (move x)))\" "
                  "\"issue740\")");
    cs.public_invalidate_function("f");
    const auto sum4b = jit_sum(cs);
    std::println("  jit-safety sum: {} -> {}", sum4a, sum4b);
    CHECK(sum4b >= sum4a, "jit-safety stats monotonic over mutate+invalidate");

    std::println("\n--- AC5: query regression ---");
    auto enforce = cs.eval("(query:linear-ownership-enforcement-stats)");
    auto safety = cs.eval("(query:linear-ownership-safety-stats)");
    CHECK(enforce && is_hash(*enforce), "linear-ownership-enforcement-stats regression");
    CHECK(safety && is_int(*safety), "linear-ownership-safety-stats regression");
}

} // namespace aura_issue_740_detail

int aura_issue_prompt6_linear_jit_l2_post_invalidate_arena_gc_run() {
    aura::compiler::CompilerService cs;
    aura_issue_740_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_prompt6_linear_jit_l2_post_invalidate_arena_gc_run();
}
#endif
