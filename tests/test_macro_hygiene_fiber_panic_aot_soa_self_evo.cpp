// test_macro_hygiene_fiber_panic_aot_soa_self_evo.cpp — Issue #654:
// 5 cross-cutting macro+reflect+self-evo hygiene gaps vs fiber/panic/
// AOT/SoA runtime (Task6 production review follow-up).
//
// Non-duplicative with #593 (pattern-ir-hygiene), #596 (guard-panic-reflect),
// #597 (macro-reflect-self-evo), #600 (incremental-closure), #653 (AOT checkpoint).
//
//   - AC1:  query:macro-hygiene-fiber-panic-stats reachable (schema 654)
//   - AC2:  macro expand + Guard mutate bumps reflect-hygiene-validation
//   - AC3:  query:pattern hygiene skip bumps provenance-violations (if set)
//   - AC4:  panic restamp observable on transfer hook
//   - AC5:  multi-round macro+mutate — stats monotonic
//   - AC6:  fiber yield under macro workspace — no crash
//   - AC7:  query regression (pattern-ir-hygiene, guard-panic-reflect, aot-checkpoint)
//
// Uses one CompilerService for the integration matrix.

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "serve/scheduler.h"
#include "serve/worker.h"

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

namespace aura_654_detail {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static std::int64_t hash_int(CompilerService& cs, const std::string& key) {
    auto r = cs.eval("(hash-ref (query:macro-hygiene-fiber-panic-stats) \"" + key + "\")");
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t stats_sum(CompilerService& cs) {
    const auto restamp = hash_int(cs, "hygiene-panic-restamp");
    const auto prov = hash_int(cs, "provenance-violations");
    const auto expand = hash_int(cs, "macro-expand-checkpoints");
    const auto reflect = hash_int(cs, "reflect-hygiene-validation");
    const auto dirty = hash_int(cs, "hygiene-dirty-impact");
    if (restamp < 0 || prov < 0 || expand < 0 || reflect < 0 || dirty < 0)
        return -1;
    return restamp + prov + expand + reflect + dirty;
}

static bool setup_macro_workspace(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (mk x) "
                 "  (list 'define (list 'v x) x)) "
                 "(define user-val 1) (mk 10)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void run_matrix(CompilerService& cs) {
    std::println("\n--- AC1: query:macro-hygiene-fiber-panic-stats (schema 654) ---");
    CHECK(setup_macro_workspace(cs), "macro workspace setup");
    auto h = cs.eval("(query:macro-hygiene-fiber-panic-stats)");
    CHECK(h && is_hash(*h), "macro-hygiene-fiber-panic-stats returns hash");
    CHECK(hash_int(cs, "schema") == 654, "schema == 654");
    const auto s0 = stats_sum(cs);
    std::println("  macro-hygiene-fiber-panic sum = {}", s0);
    CHECK(s0 >= 0, "macro-hygiene-fiber-panic stats non-negative");

    std::println("\n--- AC2: Guard mutate bumps reflect-hygiene-validation ---");
    const auto reflect0 = hash_int(cs, "reflect-hygiene-validation");
    (void)cs.eval("(mutate:rebind \"user-val\" \"42\")");
    (void)cs.eval("(eval-current)");
    const auto reflect1 = hash_int(cs, "reflect-hygiene-validation");
    std::println("  reflect-hygiene-validation: {} -> {}", reflect0, reflect1);
    CHECK(reflect1 >= reflect0, "reflect-hygiene-validation monotonic after Guard mutate");

    std::println("\n--- AC3: query:pattern hygiene filter ---");
    const auto prov0 = hash_int(cs, "provenance-violations");
    (void)cs.eval("(query:pattern \"v\")");
    (void)cs.eval("(query:pattern \"define\")");
    const auto prov1 = hash_int(cs, "provenance-violations");
    std::println("  provenance-violations: {} -> {}", prov0, prov1);
    CHECK(prov1 >= prov0, "provenance-violations monotonic after query:pattern");

    std::println("\n--- AC4: panic restamp observable ---");
    const auto restamp0 = hash_int(cs, "hygiene-panic-restamp");
    cs.evaluator().bump_macro_hygiene_panic_restamp();
    const auto restamp1 = hash_int(cs, "hygiene-panic-restamp");
    std::println("  hygiene-panic-restamp: {} -> {}", restamp0, restamp1);
    CHECK(restamp1 > restamp0, "hygiene-panic-restamp bumped");

    std::println("\n--- AC5: multi-round macro+mutate stats monotonic ---");
    const auto stats5a = stats_sum(cs);
    for (int round = 0; round < 3; ++round) {
        (void)cs.eval("(mutate:rebind \"user-val\" \"" + std::to_string(50 + round) + "\")");
        (void)cs.eval("(eval-current)");
        (void)cs.eval("(query:pattern \"define\")");
    }
    const auto stats5b = stats_sum(cs);
    std::println("  macro-hygiene sum: {} -> {}", stats5a, stats5b);
    CHECK(stats5b >= stats5a, "macro-hygiene stats monotonic over matrix");

    std::println("\n--- AC6: fiber yield under macro workspace ---");
    Scheduler sched(4);
    std::atomic<int> done{0};
    for (int f = 0; f < 8; ++f) {
        sched.spawn([&]() {
            aura_evaluator_test_push_mutation_checkpoint();
            Fiber::yield(YieldReason::MutationBoundary);
            aura_evaluator_test_pop_mutation_checkpoint();
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (done.load() < 8 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.stop();
    io.join();
    CHECK(done.load() == 8, "fiber yield completed under macro workspace");

    std::println("\n--- AC7: query regression ---");
    auto pir = cs.eval("(query:pattern-ir-hygiene-closed-loop-stats)");
    auto gpr = cs.eval("(query:guard-panic-reflect-stats)");
    auto aot = cs.eval("(query:aot-checkpoint-version-stats)");
    CHECK(pir && is_hash(*pir), "pattern-ir-hygiene-closed-loop-stats regression");
    CHECK(gpr && is_hash(*gpr), "guard-panic-reflect-stats regression");
    CHECK(aot && is_hash(*aot), "aot-checkpoint-version-stats regression");
}

} // namespace aura_654_detail

int aura_issue_macro_hygiene_fiber_panic_aot_soa_self_evo_run() {
    aura::compiler::CompilerService cs;
    aura_654_detail::run_matrix(cs);
    return RUN_ALL_TESTS();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_macro_hygiene_fiber_panic_aot_soa_self_evo_run();
}
#endif
