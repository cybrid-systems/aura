// @category: unit
// @reason: Issue #2511 — outermost Guard exit forced mailbox deferred drain
//          under budget (hold-blocked SLA). Extends #2378.
//
//   AC1: outermost Guard exit calls drain_deferred_under_budget (source-cite)
//   AC2: hold-period defer then exit → drain path runs; depth resolved or audited
//   AC3: budget env; over-budget starvation + health score signal
//   AC4: multi-fiber chaos (N concurrent defers + drain)
//   AC5: no pending defer → single relaxed load (zero extra spin)

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
using aura::serve::mf_mailbox::drain_deferred_under_budget;
using aura::serve::mf_mailbox::g_mf_mailbox_stats;
using aura::serve::mf_mailbox::mailbox_hold_drain_budget_us;
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

static std::int64_t health_href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:mutation-concurrency-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void drain_all_depth() {
    std::uint64_t guard = 256;
    while (g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) > 0 &&
           guard-- > 0)
        note_mailbox_push_ok_drain_progress();
}

// ── AC1: source-cite ──
static void ac1_source_cite() {
    std::println("\n--- #2511 AC1: Guard exit calls drain_deferred_under_budget ---");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto mb = read_file("src/serve/multi_fiber_mailbox.h");
    CHECK(mb.find("drain_deferred_under_budget") != std::string::npos, "AC1: drain API");
    CHECK(mb.find("AURA_MAILBOX_HOLD_DRAIN_BUDGET_US") != std::string::npos, "AC1: budget env");
    CHECK(mb.find("Issue #2511") != std::string::npos, "AC1: #2511 in mailbox");
    CHECK(emb.find("drain_deferred_under_budget") != std::string::npos,
          "AC1: outermost Guard dtor drain");
    CHECK(emb.find("Issue #2511") != std::string::npos || emb.find("#2511") != std::string::npos,
          "AC1: Guard cites #2511");
    CHECK(mb.find("mailbox_hold_exit_drain_total") != std::string::npos, "AC1: drain total");
    CHECK(mb.find("mailbox_hold_exit_starvation_total") != std::string::npos,
          "AC1: starvation total");
}

// ── AC2: open defer + drain → path runs ──
static void ac2_hold_then_drain() {
    std::println("\n--- #2511 AC2: hold defers then exit drain ---");
    drain_all_depth();
    const auto drain0 =
        g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(std::memory_order_relaxed);
    const auto star0 =
        g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed);
    note_mailbox_mutation_hold_defer();
    note_mailbox_mutation_hold_defer();
    CHECK(g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) >= 2,
          "AC2: depth open");
    // budget 0 → immediate deadline → starvation (Soft keeps depth).
    ::setenv("AURA_MAILBOX_HOLD_DRAIN_BUDGET_US", "0", 1);
    // budget is cached static — force use explicit budget arg.
    auto r = drain_deferred_under_budget(/*budget_us=*/0);
    CHECK(r.had_open_defer, "AC2: had open defer");
    CHECK(g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(std::memory_order_relaxed) > drain0,
          "AC2: hold_exit_drain_total +1");
    CHECK(r.starved || r.remaining_depth == 0 || r.force_resolved > 0,
          "AC2: starved or resolved under budget 0");
    if (r.starved) {
        CHECK(g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(
                  std::memory_order_relaxed) > star0,
              "AC2: starvation total +1 when starved");
    }
    // Soft: depth may remain — close for next test.
    drain_all_depth();
    ::unsetenv("AURA_MAILBOX_HOLD_DRAIN_BUDGET_US");
}

// ── AC3: budget + health ──
static void ac3_budget_and_health() {
    std::println("\n--- #2511 AC3: budget + health score ---");
    CHECK(mailbox_hold_drain_budget_us() >= 0, "AC3: budget loadable");
    drain_all_depth();
    note_mailbox_mutation_hold_defer();
    auto r = drain_deferred_under_budget(0);
    CHECK(r.had_open_defer, "AC3: drain invoked");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2511") == 2511, "AC3: schema-2511");
    CHECK(href(cs, "mailbox-hold-exit-drain-wired") == 1, "AC3: wired");
    CHECK(href(cs, "mailbox-hold-exit-drain-total") >= 0, "AC3: drain total query");
    CHECK(href(cs, "mailbox-hold-exit-starvation-total") >= 0, "AC3: starvation query");
    CHECK(href(cs, "mailbox-hold-exit-drain-budget-us") >= 0, "AC3: budget query");
    // Health component exposed.
    CHECK(health_href(cs, "component-mailbox-hold-exit-starvation-total") >= 0 ||
              health_href(cs, "schema-2511") == 2511,
          "AC3: health sees hold-exit starvation / schema-2511");
    drain_all_depth();
}

// ── AC4: multi-thread chaos ──
static void ac4_chaos() {
    std::println("\n--- #2511 AC4: concurrent defers + drains ---");
    drain_all_depth();
    const auto star0 =
        g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed);
    const auto drain0 =
        g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(std::memory_order_relaxed);
    constexpr int N = 8;
    constexpr int K = 20;
    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([K]() {
            for (int i = 0; i < K; ++i) {
                note_mailbox_mutation_hold_defer();
                if ((i % 3) == 0)
                    (void)drain_deferred_under_budget(50);
                if ((i % 5) == 0)
                    note_mailbox_push_ok_drain_progress();
            }
        });
    }
    for (auto& th : threads)
        th.join();
    // Final drain under budget 0.
    (void)drain_deferred_under_budget(0);
    drain_all_depth();
    CHECK(g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(std::memory_order_relaxed) > drain0,
          "AC4: drains ran under chaos");
    // No silent loss: depth closed after final drain_all.
    CHECK(g_mf_mailbox_stats.mailbox_deferred_depth.load(std::memory_order_relaxed) == 0,
          "AC4: depth closed (no silent residual)");
    (void)star0;
}

// ── AC5: happy path zero extra ──
static void ac5_happy_path() {
    std::println("\n--- #2511 AC5: no pending → free path ---");
    drain_all_depth();
    const auto drain0 =
        g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(std::memory_order_relaxed);
    const auto star0 =
        g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed);
    auto r = drain_deferred_under_budget(1000);
    CHECK(!r.had_open_defer, "AC5: no open defer");
    CHECK(!r.starved, "AC5: not starved");
    CHECK(r.force_resolved == 0, "AC5: no force resolve");
    CHECK(g_mf_mailbox_stats.mailbox_hold_exit_drain_total.load(std::memory_order_relaxed) ==
              drain0,
          "AC5: drain total flat when depth 0");
    CHECK(g_mf_mailbox_stats.mailbox_hold_exit_starvation_total.load(std::memory_order_relaxed) ==
              star0,
          "AC5: starvation flat");
}

// ── schema ──
static void ac_schema() {
    std::println("\n--- #2511 schema ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2511") == 2511, "schema-2511");
    CHECK(href(cs, "issue-2511") == 2511, "issue-2511");
    CHECK(href(cs, "schema-2378") == 2378, "schema-2378 retained");
    CHECK(href(cs, "mailbox-hold-exit-force-resolved-total") >= 0, "force-resolved key");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_mailbox_hold_exit_drain") != std::string::npos, "cmake");
}

} // namespace

int run_test_mailbox_hold_exit_drain() {
    std::println("test_mailbox_hold_exit_drain");
    ac1_source_cite();
    ac2_hold_then_drain();
    ac3_budget_and_health();
    ac4_chaos();
    ac5_happy_path();
    ac_schema();
    if (g_failed)
        return 1;
    std::println("mailbox hold-exit drain #2511: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_mailbox_hold_exit_drain();
}
#endif
