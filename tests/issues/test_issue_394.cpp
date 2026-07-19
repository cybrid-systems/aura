// @category: integration
// @reason: uses CompilerService + Scheduler + concurrent atomic-batch safety
//
// test_issue_394.cpp — Issue #394 follow-up to #250:
//   1. hash-ref regression for (stats:get "atomic-batch:stats")
//   2. Concurrent fiber/thread safety during atomic batch
//   3. Documentation: docs/design/core/mutate_api.md removed per
//      Anqi 2026-07-19 directive (aura philosophy, no per-issue
//      plan docs); source-driven ACs above remain authoritative

#include "test_harness.hpp"
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.evaluator;
import aura.compiler.value;
import aura.compiler.service;

namespace aura_issue_394_detail {

using aura::compiler::CompilerService;
using aura::serve::Fiber;
using aura::serve::Scheduler;
using aura::serve::YieldReason;

static bool eval_ok(CompilerService& cs, std::string_view src) {
    return cs.eval(src).has_value();
}

static std::int64_t eval_int(CompilerService& cs, std::string_view src) {
    auto r = cs.eval(src);
    if (!r || !aura::compiler::types::is_int(*r))
        return -1;
    return aura::compiler::types::as_int(*r);
}

// ── AC1: hash-ref returns ints for all atomic-batch:stats keys ──
bool test_atomic_batch_stats_hash_ref() {
    std::println("\n--- AC1: hash-ref on (stats:get \"atomic-batch:stats\") keys ---");
    CompilerService cs;
    if (!eval_ok(cs, "(set-code \"(define x 1)\")")) {
        CHECK(false, "set-code");
        return false;
    }
    (void)cs.eval("(eval-current)");

    auto h = cs.eval("(stats:get \"atomic-batch:stats\")");
    CHECK(h.has_value(), "(stats:get \"atomic-batch:stats\") returns");
    if (!h || h->val == 11) {
        CHECK(false, "(stats:get \"atomic-batch:stats\") is a hash (not void)");
        return false;
    }

    for (const char* key :
         {"batch-count", "ops-total", "rollback-count", "ops-per-batch", "bumps-saved-total"}) {
        std::string q =
            std::string("(hash-ref (stats:get \"atomic-batch:stats\") \"") + key + "\")";
        auto v = cs.eval(q);
        if (!v || !aura::compiler::types::is_int(*v)) {
            CHECK(false, std::string("hash-ref \"") + key + "\" returns int");
        } else {
            CHECK(true, std::string("hash-ref \"") + key + "\" returns int");
        }
    }
    return true;
}

// ── AC2: bumps-saved-total > 0 after a multi-op batch ──
bool test_bumps_saved_after_batch() {
    std::println("\n--- AC2: bumps-saved-total grows after multi-op batch ---");
    CompilerService cs;
    if (!eval_ok(cs, "(set-code \"(define x 5)\")")) {
        CHECK(false, "set-code");
        return false;
    }
    (void)cs.eval("(eval-current)");

    const auto before =
        eval_int(cs, "(hash-ref (stats:get \"atomic-batch:stats\") \"bumps-saved-total\")");
    CHECK(before >= 0, "bumps-saved-total readable before batch");

    std::string batch = "(mutate:atomic-batch (list "
                        "  (list \"mutate:rebind\" \"x\" \"10\" \"a\") "
                        "  (list \"mutate:rebind\" \"x\" \"20\" \"b\") "
                        "  (list \"mutate:rebind\" \"x\" \"30\" \"c\") "
                        ") \"three rebinds\")";
    auto br = cs.eval(batch);
    CHECK(br.has_value() && aura::compiler::types::is_bool(*br) &&
              aura::compiler::types::as_bool(*br),
          "3-op atomic-batch returns #t");

    const auto after =
        eval_int(cs, "(hash-ref (stats:get \"atomic-batch:stats\") \"bumps-saved-total\")");
    CHECK(after > before, "bumps-saved-total increased after multi-op batch");
    std::println("  bumps-saved-total: {} -> {}", before, after);
    return true;
}

// Build a batch with N sequential rebinds of x.
static std::string make_rebind_batch(int n) {
    std::string ops = "(list ";
    for (int i = 1; i <= n; ++i) {
        ops += "(list \"mutate:rebind\" \"x\" \"" + std::to_string(i) + "\" \"s" +
               std::to_string(i) + "\") ";
    }
    ops += ")";
    return std::string("(mutate:atomic-batch ") + ops + " \"concurrent batch\")";
}

static std::uint64_t read_generation_shared(aura::compiler::Evaluator& ev) {
    std::uint64_t g = 0;
    ev.lock_workspace_shared();
    if (auto* flat = ev.workspace_flat())
        g = flat->generation();
    ev.unlock_workspace_shared();
    return g;
}

// ── AC3: concurrent reader sees pre- or post-batch generation only ──
bool test_concurrent_generation_no_torn_reads() {
    std::println("\n--- AC3: concurrent generation read during atomic batch ---");
    CompilerService cs;
    if (!eval_ok(cs, "(set-code \"(define x 0)\")")) {
        CHECK(false, "set-code");
        return false;
    }
    (void)cs.eval("(eval-current)");
    auto& ev = cs.evaluator();
    auto* ws = ev.workspace_flat();
    if (!ws) {
        CHECK(false, "workspace_flat available");
        return false;
    }

    const std::uint64_t g0 = ws->generation();
    std::atomic<bool> batch_started{false};
    std::atomic<bool> batch_done{false};
    std::vector<std::uint64_t> gens;
    std::mutex gens_mtx;

    // Reader thread: use workspace_mtx_ shared lock only. CompilerService
    // eval() is not thread-safe for parallel calls (#332); the shared
    // lock path matches concurrent query fibers blocking on the batch.
    std::thread query_thread([&]() {
        while (!batch_done.load(std::memory_order_acquire)) {
            if (!batch_started.load(std::memory_order_acquire))
                continue;
            auto g = read_generation_shared(ev);
            {
                std::lock_guard<std::mutex> lk(gens_mtx);
                gens.push_back(g);
            }
        }
        auto g = read_generation_shared(ev);
        {
            std::lock_guard<std::mutex> lk(gens_mtx);
            gens.push_back(g);
        }
    });

    std::thread batch_thread([&]() {
        batch_started.store(true, std::memory_order_release);
        (void)cs.eval(make_rebind_batch(20));
        batch_done.store(true, std::memory_order_release);
    });

    batch_thread.join();
    query_thread.join();

    const std::uint64_t g1 = ws->generation();
    CHECK(g1 == g0 + 1, "atomic batch bumps generation exactly once");

    bool only_pre_or_post = true;
    for (auto g : gens) {
        if (g != g0 && g != g1) {
            only_pre_or_post = false;
            std::println("  observed intermediate generation {} (expected {} or {})", g, g0, g1);
            break;
        }
    }
    std::println("  sampled {} generation reads", gens.size());
    CHECK(!gens.empty(), "reader thread sampled generation during batch");
    CHECK(only_pre_or_post, "concurrent readers saw pre-batch or post-batch generation only");
    return true;
}

// ── AC4: post-batch query + single-bump commit regression ──
bool test_query_after_batch_single_bump() {
    std::println("\n--- AC4: query:pattern after batch + single generation bump ---");
    CompilerService cs;
    if (!eval_ok(cs, "(set-code \"(define x 0) (define target 42)\")")) {
        CHECK(false, "set-code");
        return false;
    }
    (void)cs.eval("(eval-current)");
    auto* ws = cs.evaluator().workspace_flat();
    if (!ws) {
        CHECK(false, "workspace_flat available");
        return false;
    }

    const std::uint64_t g0 = ws->generation();
    auto br = cs.eval(make_rebind_batch(15));
    CHECK(br.has_value() && aura::compiler::types::is_bool(*br) &&
              aura::compiler::types::as_bool(*br),
          "15-op atomic-batch commits");

    const std::uint64_t g1 = ws->generation();
    CHECK(g1 == g0 + 1, "batch commits with a single generation bump");

    auto qr = cs.eval("(query:pattern \"42\")");
    CHECK(qr.has_value(), "query:pattern works after atomic batch");
    return true;
}

// ── AC5: Scheduler + 2 fibers while batch runs on a worker thread ──
bool test_scheduler_fibers_during_batch() {
    std::println("\n--- AC5: Scheduler fibers + atomic batch worker ---");
    CompilerService cs;
    if (!eval_ok(cs, "(set-code \"(define x 0)\")")) {
        CHECK(false, "set-code");
        return false;
    }
    (void)cs.eval("(eval-current)");

    std::atomic<bool> batch_done{false};
    std::atomic<int> fiber_done{0};
    constexpr int k_fibers = 2;
    constexpr int k_iters = 30;

    std::thread batch_thread([&]() {
        (void)cs.eval(make_rebind_batch(10));
        batch_done.store(true, std::memory_order_release);
    });

    Scheduler sched(2);
    for (int i = 0; i < k_fibers; ++i) {
        sched.spawn([&fiber_done, &batch_done]() {
            for (int j = 0; j < k_iters; ++j) {
                Fiber::yield(YieldReason::MutationBoundary);
                if (batch_done.load(std::memory_order_acquire))
                    break;
            }
            fiber_done.fetch_add(1);
        });
    }

    std::thread io_thread([&sched]() { sched.run(); });
    batch_thread.join();
    auto t0 = std::chrono::steady_clock::now();
    while (fiber_done.load() < k_fibers) {
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sched.stop();
    io_thread.join();

    std::println("  fibers completed: {}/{}", fiber_done.load(), k_fibers);
    CHECK(fiber_done.load() == k_fibers, "scheduler fibers completed while atomic batch ran");
    return true;
}

int run_tests() {
    std::println("═══ Issue #394 — hash-ref fix + concurrent atomic-batch safety ═══\n");
    test_atomic_batch_stats_hash_ref();
    test_bumps_saved_after_batch();
    test_concurrent_generation_no_torn_reads();
    test_query_after_batch_single_bump();
    test_scheduler_fibers_during_batch();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_394_detail

int aura_issue_394_run() {
    return aura_issue_394_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_394_run();
}
#endif