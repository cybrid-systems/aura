// @category: unit
// @reason: Issue #2551 — outermost Guard exit drain residual under
//          production becomes hard signal + Agent throttle flag.
//
//   AC1: Production/Strict + residual after budget → hard counter + flag
//   AC2: Soft / no residual → zero extra cost; flag stays clear
//   AC3: Subsequent successful drain to zero clears throttle flag
//   AC4: Chaos soak surfaces hard signal; Agents can read the flag
//   AC5: Source-cite next to #2511 / #2378; additive schema-2551

#include "test_harness.hpp"

#include "serve/multi_fiber_mailbox.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::mf_mailbox::clear_agent_throttle_for_mailbox_starvation;
using aura::serve::mf_mailbox::drain_deferred_under_budget;
using aura::serve::mf_mailbox::g_mf_mailbox_stats;
using aura::serve::mf_mailbox::note_mailbox_mutation_hold_defer;
using aura::serve::mf_mailbox::note_mailbox_push_ok_drain_progress;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:mf-mailbox-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void drain_all_depth() {
    std::uint64_t guard = 256;
    while (g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) > 0 &&
           guard-- > 0)
        note_mailbox_push_ok_drain_progress();
    // Free path clears throttle when depth already 0.
    (void)drain_deferred_under_budget(0);
}

static void strict_on() {
    ::setenv("AURA_MUTATE_MAILBOX_STRICT", "1", 1);
}
static void strict_off() {
    ::unsetenv("AURA_MUTATE_MAILBOX_STRICT");
}

// ── AC1: Strict + residual after budget → hard + flag ──
static void ac1_production_hard_signal() {
    std::println("\n--- #2551 AC1: Strict residual after budget → hard + flag ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_on();
    const auto hard0 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    const auto soft0 =
        g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed);
    note_mailbox_mutation_hold_defer();
    note_mailbox_mutation_hold_defer();
    CHECK(g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) >= 2,
          "AC1: depth open");
    // budget 0 → immediate starve
    auto r = drain_deferred_under_budget(/*budget_us=*/0);
    CHECK(r.starved, "AC1: starved under budget 0");
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) ==
              hard0 + 1,
          "AC1: hard total +1");
    CHECK(g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed) >
              soft0,
          "AC1: soft starvation still advances");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 1,
          "AC1: Agent throttle flag set");
    // Force-resolve may have zeroed depth under Strict — flag must stay set
    // until subsequent free drain (AC3).
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 1,
          "AC1: flag remains after force-resolve in same call");
    strict_off();
    drain_all_depth();
}

// ── AC2: Soft residual / free path → no hard, flag clear ──
static void ac2_soft_and_free() {
    std::println("\n--- #2551 AC2: Soft residual + free path zero extra ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_off();
    const auto hard0 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    note_mailbox_mutation_hold_defer();
    auto r = drain_deferred_under_budget(/*budget_us=*/0);
    CHECK(r.starved || r.remaining_depth > 0 || r.had_open_defer, "AC2: soft drain with open");
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) ==
              hard0,
          "AC2: hard total flat under Soft");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC2: throttle flag stays clear under Soft");
    // Free path (no residual): flag clear, hard flat
    drain_all_depth();
    const auto hard1 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    auto r2 = drain_deferred_under_budget(1000);
    CHECK(!r2.had_open_defer, "AC2: free path no open");
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) ==
              hard1,
          "AC2: free path hard flat");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC2: free path flag clear");
}

// ── AC3: subsequent drain to zero clears flag ──
static void ac3_clear_on_drain_zero() {
    std::println("\n--- #2551 AC3: subsequent free drain clears throttle ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_on();
    note_mailbox_mutation_hold_defer();
    (void)drain_deferred_under_budget(0);
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 1,
          "AC3: flag set after hard starve");
    // Ensure depth is zero then free drain clears.
    drain_all_depth(); // may already be 0 from force-resolve; free path clears
    CHECK(g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) == 0,
          "AC3: depth zero");
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC3: flag cleared after free drain");
    // Natural window close also clears.
    clear_agent_throttle_for_mailbox_starvation();
    g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.store(1, std::memory_order_relaxed);
    note_mailbox_mutation_hold_defer();
    note_mailbox_push_ok_drain_progress(); // 1→0
    CHECK(g_mf_mailbox_stats.agent_throttle_for_mailbox_starvation.load(
              std::memory_order_relaxed) == 0,
          "AC3: push_ok window close clears flag");
    strict_off();
}

// ── AC4: chaos + query readable ──
static void ac4_chaos_and_query() {
    std::println("\n--- #2551 AC4: chaos + Agent-readable query ---");
    drain_all_depth();
    clear_agent_throttle_for_mailbox_starvation();
    strict_on();
    const auto hard0 =
        g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed);
    constexpr int N = 6;
    constexpr int K = 12;
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([K]() {
            for (int i = 0; i < K; ++i) {
                note_mailbox_mutation_hold_defer();
                if ((i % 2) == 0)
                    (void)drain_deferred_under_budget(0);
                if ((i % 4) == 0)
                    note_mailbox_push_ok_drain_progress();
            }
        });
    }
    for (auto& th : threads)
        th.join();
    (void)drain_deferred_under_budget(0);
    CHECK(g_mf_mailbox_stats.mailbox_hold_starvation_hard_total.load(std::memory_order_relaxed) >
              hard0,
          "AC4: hard signal advanced under chaos");
    // Agents read via query
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2551") == 2551, "AC4: schema-2551");
    CHECK(href(cs, "mailbox-hold-starvation-hard-wired") == 1, "AC4: wired");
    CHECK(href(cs, "mailbox-hold-starvation-hard-total") >= 0, "AC4: hard total query");
    CHECK(href(cs, "agent-throttle-for-mailbox-starvation") >= 0, "AC4: throttle flag query");
    // Agent skip-mutate: flag is 0/1 readable
    const auto thr = href(cs, "agent-throttle-for-mailbox-starvation");
    CHECK(thr == 0 || thr == 1, "AC4: throttle is boolean 0/1");
    drain_all_depth();
    strict_off();
}

// ── AC5: source-cite + gate ──
static void ac5_source_and_gate() {
    std::println("\n--- #2551 AC5: source-cite + linter ---");
    const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto epm = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    const auto lint =
        read_file("scripts/coverage/checks/check_mailbox_hold_starvation_hard_2551.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(mb.find("Issue #2551") != std::string::npos, "AC5: mailbox cites #2551");
    CHECK(mb.find("mailbox_hold_starvation_hard_total") != std::string::npos, "AC5: hard total");
    CHECK(mb.find("agent_throttle_for_mailbox_starvation") != std::string::npos, "AC5: throttle");
    CHECK(emb.find("#2551") != std::string::npos, "AC5: Guard dtor cites #2551");
    CHECK(emb.find("drain_deferred_under_budget") != std::string::npos, "AC5: dtor drain");
    CHECK(epm.find("schema-2551") != std::string::npos, "AC5: schema-2551");
    CHECK(epm.find("agent-throttle-for-mailbox-starvation") != std::string::npos,
          "AC5: query throttle");
    // Lineage next to #2511 / #2378
    CHECK(mb.find("Issue #2511") != std::string::npos, "AC5: #2511 lineage");
    CHECK(mb.find("#2378") != std::string::npos || mb.find("Issue #2378") != std::string::npos,
          "AC5: #2378 lineage");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(cmake.find("test_mailbox_hold_starvation_hard_2551") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_mailbox_hold_starvation_hard_2551") != std::string::npos,
          "AC5: build script");
    CHECK(build.find("cmd_mailbox_hold_starvation_hard_coverage") != std::string::npos,
          "AC5: build cmd");
}

} // namespace

int run_test_mailbox_hold_starvation_hard_2551() {
    std::println("=== Issue #2551: mailbox hold starvation hard + Agent throttle ===");
    ac1_production_hard_signal();
    ac2_soft_and_free();
    ac3_clear_on_drain_zero();
    ac4_chaos_and_query();
    ac5_source_and_gate();
    std::println("\n=== #2551: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mailbox_hold_starvation_hard_2551();
}
#endif
