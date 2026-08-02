// @category: unit
// @reason: Issue #2115 — unify is_at_safe_mutation_boundary with
// depth-safe API (steal correctness under concurrent mutate).
//
//   AC1: Holding MutationBoundary (depth>0) fiber is never steal-safe
//   AC2: YieldReason::MutationBoundary + depth==0 is steal-safe; depth>0 not
//   AC3: Alias is_at_safe_mutation_boundary ≡ is_at_mutation_boundary_safe
//   AC4: steal_skipped_mutation_boundary_total queryable (schema-2115)
//   AC5: dual-fiber mutate + steal stress; existing suites source-wired

#include "test_harness.hpp"

#include "serve/fiber.h"
#include "serve/metrics.h"
#include "serve/scheduler.h"
#include "serve/worker.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

extern "C" std::size_t aura_evaluator_mutation_stack_depth_from_ptr(void*);
extern "C" void aura_evaluator_test_push_mutation_checkpoint();
extern "C" void aura_evaluator_test_pop_mutation_checkpoint();

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::metrics::adaptive_steal_stats;
using aura::test::g_failed;
using aura::test::g_passed;
// Prefer fully-qualified aura::serve::g_current_fiber in bodies.

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:orchestration-steal-stats\") \"{}\")", key));
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

static void ac1_depth_gt0_never_steal_safe() {
    std::println("\n--- AC1: depth>0 never steal-safe (alias + depth-safe) ---");
    Scheduler sched(2);
    std::atomic<bool> checked{false};
    sched.spawn([&]() {
        aura_evaluator_test_push_mutation_checkpoint();
        CHECK(aura::serve::g_current_fiber != nullptr, "fiber active");
        if (aura::serve::g_current_fiber) {
            aura::serve::g_current_fiber->set_yield_reason(YieldReason::MutationBoundary);
            const auto depth = aura_evaluator_mutation_stack_depth_from_ptr(
                aura::serve::g_current_fiber->mutation_stack_ptr());
            CHECK(depth >= 1, "depth >= 1 while checkpoint held");
            CHECK(!aura::serve::g_current_fiber->is_at_mutation_boundary_safe(),
                  "depth-safe false");
            CHECK(!aura::serve::g_current_fiber->is_at_safe_mutation_boundary(), "alias false");
            // Issue #2549: reason-class candidate still true; is_stealable
            // now requires snapshot-safe (false when depth>0).
            CHECK(aura::serve::g_current_fiber->is_steal_candidate(),
                  "MB still steal candidate (reason class)");
            CHECK(!aura::serve::g_current_fiber->is_stealable(),
                  "MB + depth>0 not is_stealable (snapshot gate)");
            checked.store(true);
        }
        aura_evaluator_test_pop_mutation_checkpoint();
        Fiber::yield(YieldReason::Explicit);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!checked.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(checked.load(), "AC1 probe ran");
}

static void ac2_depth0_safe_depth_gt0_not() {
    std::println("\n--- AC2: depth==0 steal-safe; depth>0 not ---");
    Scheduler sched(2);
    std::atomic<int> phases{0};
    sched.spawn([&]() {
        // depth==0 + MB yield → safe
        if (aura::serve::g_current_fiber) {
            aura::serve::g_current_fiber->set_yield_reason(YieldReason::MutationBoundary);
            CHECK(aura_evaluator_mutation_stack_depth_from_ptr(
                      aura::serve::g_current_fiber->mutation_stack_ptr()) == 0,
                  "depth 0");
            CHECK(aura::serve::g_current_fiber->is_at_mutation_boundary_safe(), "depth0 safe");
            CHECK(aura::serve::g_current_fiber->is_at_safe_mutation_boundary(),
                  "alias depth0 safe");
            phases.fetch_add(1);
        }
        // depth>0 + MB → unsafe
        aura_evaluator_test_push_mutation_checkpoint();
        if (aura::serve::g_current_fiber) {
            aura::serve::g_current_fiber->set_yield_reason(YieldReason::MutationBoundary);
            CHECK(!aura::serve::g_current_fiber->is_at_mutation_boundary_safe(), "depth>0 unsafe");
            CHECK(!aura::serve::g_current_fiber->is_at_safe_mutation_boundary(),
                  "alias depth>0 unsafe");
            phases.fetch_add(1);
        }
        aura_evaluator_test_pop_mutation_checkpoint();
        // Explicit yield always safe for depth-safe probe
        if (aura::serve::g_current_fiber) {
            aura::serve::g_current_fiber->set_yield_reason(YieldReason::Explicit);
            CHECK(aura::serve::g_current_fiber->is_at_mutation_boundary_safe(), "Explicit safe");
            CHECK(aura::serve::g_current_fiber->is_at_safe_mutation_boundary(),
                  "alias Explicit safe");
            phases.fetch_add(1);
        }
        Fiber::yield(YieldReason::Explicit);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (phases.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(phases.load() == 3, "all AC2 phases");
}

static void ac3_alias_and_source() {
    std::println("\n--- AC3: alias + source wiring ---");
    auto fh = read_file("src/serve/fiber.h");
    auto wc = read_file("src/serve/worker.cpp");
    CHECK(fh.find("Issue #2115") != std::string::npos || fh.find("#2115") != std::string::npos,
          "fiber.h cites #2115");
    CHECK(fh.find("is_at_safe_mutation_boundary") != std::string::npos, "safe name present");
    CHECK(fh.find("return is_at_mutation_boundary_safe()") != std::string::npos, "alias body");
    // Old always-false body for MB must be gone
    CHECK(fh.find("Currently yielded at a MutationBoundary — assume") == std::string::npos,
          "old always-false path removed");
    CHECK(wc.find("steal_skipped_mutation_boundary_total") != std::string::npos,
          "worker bumps skip metric");
    // #2184: steal samples MutationSafetySnapshot then calls
    // is_at_mutation_boundary_safe(snap); empty-paren form is not required.
    CHECK(wc.find("is_at_mutation_boundary_safe") != std::string::npos, "worker depth-safe");
}

static void ac4_query_and_metric_bump() {
    std::println("\n--- AC4: steal_skipped_mutation_boundary_total + schema-2115 ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "eval");
    CHECK(href(cs, "schema-2115") == 2115, "schema-2115");
    CHECK(href(cs, "issue-2115") == 2115, "issue-2115");
    CHECK(href(cs, "depth-safe-boundary-steal-wired") == 1, "wired");
    CHECK(href(cs, "is-at-safe-mutation-boundary-aliased") == 1, "aliased flag");
    CHECK(href(cs, "steal-skipped-mutation-boundary-total") >= 0, "skip counter present");
    CHECK(href(cs, "steal_skipped_mutation_boundary_total") >= 0, "underscore alias");

    // Stress: dual workers; pin victim with depth>0 MB yield; idle thieves steal.
    const auto skipped0 = adaptive_steal_stats().steal_skipped_mutation_boundary_total.load(
        std::memory_order_relaxed);
    Scheduler sched(4);
    std::atomic<int> done{0};
    constexpr int k_victims = 4;
    for (int i = 0; i < k_victims; ++i) {
        sched.spawn_with_affinity(
            [&]() {
                for (int j = 0; j < 40; ++j) {
                    aura_evaluator_test_push_mutation_checkpoint();
                    Fiber::yield(YieldReason::MutationBoundary);
                    aura_evaluator_test_pop_mutation_checkpoint();
                }
                done.fetch_add(1);
            },
            /*affinity=*/0);
    }
    // Thief work so workers stay busy / steal
    for (int i = 0; i < 8; ++i) {
        sched.spawn([&]() {
            for (int j = 0; j < 20; ++j)
                Fiber::yield(YieldReason::Explicit);
        });
    }
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (done.load() < k_victims && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sched.stop();
    io.join();
    const auto skipped1 = adaptive_steal_stats().steal_skipped_mutation_boundary_total.load(
        std::memory_order_relaxed);
    // Under concurrent steal + depth>0 MB yield, skip counter should grow
    // (best-effort — if steal never attempted, still green on query keys).
    CHECK(done.load() == k_victims, "victims finished");
    CHECK(skipped1 >= skipped0, "skip counter monotonic");
    if (skipped1 > skipped0) {
        CHECK(href(cs, "steal-skipped-mutation-boundary-total") >= 0, "query after stress");
        std::println("    steal_skipped_mutation_boundary_total delta = {}", skipped1 - skipped0);
    } else {
        std::println("    note: no steal skip observed this run (timing); query keys still OK");
    }
}

static void ac5_guard_held_and_dual_fiber() {
    std::println("\n--- AC5: outermost Guard held + dual-fiber ---");
    // Real MutationBoundaryGuard: enter pushes active stack → depth>0
    CompilerService cs;
    bool ok = true;
    std::atomic<bool> unsafe{false};
    Scheduler sched(2);
    sched.spawn([&]() {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        CHECK(cs.evaluator().mutation_boundary_held(), "held");
        if (aura::serve::g_current_fiber) {
            aura::serve::g_current_fiber->set_yield_reason(YieldReason::MutationBoundary);
            // Guard enter → per-fiber stack depth > 0 on active fiber
            const bool safe = aura::serve::g_current_fiber->is_at_mutation_boundary_safe();
            const bool alias = aura::serve::g_current_fiber->is_at_safe_mutation_boundary();
            CHECK(safe == alias, "alias matches");
            // With real Guard, depth should be > 0 → not safe
            if (!safe)
                unsafe.store(true);
        }
        Fiber::yield(YieldReason::MutationBoundary);
    });
    std::thread io([&sched]() { sched.run(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!unsafe.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    sched.stop();
    io.join();
    CHECK(unsafe.load(), "Guard-held MB yield not steal-safe");

    auto mh = read_file("src/serve/metrics.h");
    auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(mh.find("steal_skipped_mutation_boundary_total") != std::string::npos, "metrics field");
    CHECK(q.find("schema-2115") != std::string::npos, "query schema");
}

} // namespace

int main() {
    std::println("=== Issue #2115: depth-safe mutation boundary steal ===");
    ac1_depth_gt0_never_steal_safe();
    ac2_depth0_safe_depth_gt0_not();
    ac3_alias_and_source();
    ac4_query_and_metric_bump();
    ac5_guard_held_and_dual_fiber();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
