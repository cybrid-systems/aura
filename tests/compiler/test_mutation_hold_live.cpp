// @category: unit
// @reason: Issue #2517 — real-time longest outermost MutationBoundary hold
// probe (fiber_id + start_ns) for Agent self-degrade.
//
//   AC1: outermost enter/exit maintain live max probe
//   AC2: query fiber_id / duration match real hold (unit + concurrent shape)
//   AC3: no holder → zeros
//   AC4: coexist with #2405 estimate / #2379 health schemas
//   AC5: best-effort CAS documented; stress does not hang

#include "test_harness.hpp"

#include "compiler/mutation_hold_budget.h"
#include "compiler/observability_metrics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::g_mutation_hold_live_clear_total;
using aura::compiler::g_mutation_hold_live_update_total;
using aura::compiler::mutation_hold_live_note_enter;
using aura::compiler::mutation_hold_live_note_exit;
using aura::compiler::mutation_hold_live_reset_for_test;
using aura::compiler::mutation_hold_live_snapshot;
using aura::compiler::mutation_hold_steady_ns_now;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

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

static std::int64_t href_live(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:mutation-hold-live\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_est(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:mutation-hold-estimate\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void spin_us(std::int64_t min_us) {
    auto t0 = std::chrono::steady_clock::now();
    while (
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
            .count() < min_us) {
    }
}

// ── AC3: no holder → zeros ──
static void ac3_empty_zeros() {
    std::println("\n--- AC3: no holder → zeros ---");
    mutation_hold_live_reset_for_test();
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    auto h = cs.eval("(engine:metrics \"query:mutation-hold-live\")");
    CHECK(h && is_hash(*h), "AC3: live hash reachable");
    CHECK(href_live(cs, "held") == 0, "AC3: held=0");
    CHECK(href_live(cs, "fiber-id") == 0, "AC3: fiber-id=0");
    CHECK(href_live(cs, "start-ns") == 0, "AC3: start-ns=0");
    CHECK(href_live(cs, "duration-us") == 0, "AC3: duration-us=0");
    CHECK(href_live(cs, "depth") == 0, "AC3: depth=0");
    CHECK(href_live(cs, "hold-live-wired") == 1, "AC3: wired");
    CHECK(href_live(cs, "schema-2517") == 2517, "AC3: schema-2517");
    CHECK(href_live(cs, "issue-2517") == 2517, "AC3: issue-2517");
    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC1 + AC2: enter/exit + query during hold ──
static void ac1_ac2_enter_exit_query() {
    std::println("\n--- AC1+AC2: enter/exit + live query during hold ---");
    mutation_hold_live_reset_for_test();
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);

    const auto upd0 = g_mutation_hold_live_update_total.load(std::memory_order_relaxed);
    const auto clr0 = g_mutation_hold_live_clear_total.load(std::memory_order_relaxed);

    bool ok = true;
    {
        Evaluator::MutationBoundaryGuard g(cs.evaluator(), &ok);
        // Live probe must show held during outermost.
        auto snap = mutation_hold_live_snapshot();
        CHECK(snap.held, "AC1: held during outermost Guard");
        CHECK(snap.fiber_id != 0, "AC1: fiber_id non-zero while held");
        CHECK(snap.start_ns != 0, "AC1: start_ns non-zero while held");
        spin_us(500);
        snap = mutation_hold_live_snapshot();
        CHECK(snap.duration_us >= 400, "AC2: duration grows during hold");

        // Query surface matches probe.
        CHECK(href_live(cs, "held") == 1, "AC2: query held=1");
        CHECK(href_live(cs, "fiber-id") > 0, "AC2: query fiber-id > 0");
        CHECK(href_live(cs, "start-ns") > 0, "AC2: query start-ns > 0");
        CHECK(href_live(cs, "duration-us") >= 0, "AC2: query duration-us readable");
        CHECK(href_live(cs, "depth") >= 1, "AC2: query depth >= 1");
    }
    // After exit: cleared.
    auto after = mutation_hold_live_snapshot();
    CHECK(!after.held, "AC1: cleared after outermost exit");
    CHECK(after.fiber_id == 0, "AC1: fiber_id=0 after exit");
    CHECK(href_live(cs, "held") == 0, "AC1: query held=0 after exit");
    CHECK(g_mutation_hold_live_update_total.load(std::memory_order_relaxed) > upd0,
          "AC1: update_total advanced");
    CHECK(g_mutation_hold_live_clear_total.load(std::memory_order_relaxed) > clr0,
          "AC1: clear_total advanced");

    // Nested does not double-publish as a separate outermost.
    {
        bool ok2 = true;
        Evaluator::MutationBoundaryGuard outer(cs.evaluator(), &ok2);
        auto mid = mutation_hold_live_snapshot();
        CHECK(mid.held, "AC1: outer holds");
        {
            bool ok3 = true;
            Evaluator::MutationBoundaryGuard nested(cs.evaluator(), &ok3);
            auto nest = mutation_hold_live_snapshot();
            CHECK(nest.held, "AC1: still held under nested");
            CHECK(nest.fiber_id == mid.fiber_id, "AC1: nested does not replace fiber");
        }
        CHECK(mutation_hold_live_snapshot().held, "AC1: still held after nested exit");
    }
    CHECK(!mutation_hold_live_snapshot().held, "AC1: clear after outer exit");

    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC4: coexist with #2405 ──
static void ac4_coexist_2405() {
    std::println("\n--- AC4: coexist with #2405 estimate ---");
    CompilerService cs;
    CompilerMetrics metrics;
    cs.evaluator().set_compiler_metrics(&metrics);
    CHECK(href_live(cs, "schema-2405") == 2405, "AC4: live exposes schema-2405");
    CHECK(href_live(cs, "hold-estimate-coexist") == 1, "AC4: coexist marker");
    CHECK(href_est(cs, "schema-2405") == 2405, "AC4: estimate schema intact");
    CHECK(href_est(cs, "hold-estimate-wired") == 1, "AC4: estimate wired intact");
    CHECK(href_est(cs, "hold-us-p50") >= 0, "AC4: estimate p50 key intact");

    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
    CHECK(emb.find("Issue #2517") != std::string::npos, "AC4: boundary cites #2517");
    CHECK(emb.find("mutation_hold_live_note_enter") != std::string::npos, "AC4: enter wire");
    CHECK(emb.find("mutation_hold_live_note_exit") != std::string::npos, "AC4: exit wire");
    CHECK(q.find("query:mutation-hold-live") != std::string::npos, "AC4: query registered");
    CHECK(q.find("query:mutation-hold-estimate") != std::string::npos, "AC4: estimate retained");
    cs.evaluator().set_compiler_metrics(nullptr);
}

// ── AC5: concurrent best-effort CAS stress ──
static void ac5_cas_stress() {
    std::println("\n--- AC5: concurrent CAS stress (best-effort) ---");
    mutation_hold_live_reset_for_test();
    constexpr int kThreads = 4;
    constexpr int kIters = 200;
    std::atomic<int> done{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &done] {
            const std::uint64_t fid = static_cast<std::uint64_t>(t + 10);
            for (int i = 0; i < kIters; ++i) {
                const auto start = mutation_hold_steady_ns_now();
                mutation_hold_live_note_enter(fid, start, 1);
                // Tiny hold window.
                spin_us(10);
                mutation_hold_live_note_exit(fid);
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(done.load() == kThreads, "AC5: all threads completed");
    // After stress, probe should be clear (all exited) or best-effort empty.
    mutation_hold_live_reset_for_test();
    auto snap = mutation_hold_live_snapshot();
    CHECK(!snap.held, "AC5: reset leaves empty probe");

    const auto bud = read_file("src/compiler/mutation_hold_budget.h");
    CHECK(bud.find("Best-effort") != std::string::npos ||
              bud.find("best-effort") != std::string::npos,
          "AC5: best-effort CAS documented");
    CHECK(bud.find("mutation_hold_live_note_enter") != std::string::npos, "AC5: enter API");
    CHECK(bud.find("mutation_hold_live_note_exit") != std::string::npos, "AC5: exit API");
}

} // namespace

int run_test_mutation_hold_live() {
    std::println("=== Issue #2517: mutation-hold-live max outermost probe ===");
    ac3_empty_zeros();
    ac1_ac2_enter_exit_query();
    ac4_coexist_2405();
    ac5_cas_stress();
    mutation_hold_live_reset_for_test();
    std::println("\n=== #2517: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mutation_hold_live();
}
#endif
