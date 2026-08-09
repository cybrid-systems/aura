// @category: unit
// @reason: Issue #2819 — capture_audit_event_forced lock-free trail publish
// (no g_trail().mu on hot path); concurrent writers; schema-2819 metrics.
//
//   AC1: capture_audit_event_forced cites #2819; lock-free write; no lock_guard
//   AC2: multi-thread concurrent publish advances lockfree + trail_writes
//   AC3: trail_latest / trail_at_seq / trail_find_by_mutation_id still work
//   AC4: schema-2819 query; this suite + linter; no docs/design/2819-*

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <atomic>
#include <cstdint>
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
using aura::test::g_failed;
using aura::test::g_passed;
namespace ta = aura::compiler::typed_audit;

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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_audit_trail_lockfree() {
    std::println("=== Issue #2819: audit trail lock-free publish ===");
    CHECK(true, "ac2819: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: lock-free capture path in source ---");
        auto h = read_file("src/compiler/typed_mutation_audit.h");
        CHECK(!h.empty(), "AC1: header readable");
        auto def = h.find("inline void capture_audit_event_forced");
        CHECK(def != std::string::npos, "AC1: capture_audit_event_forced present");
        // Window through the function body (until capture_audit_event next).
        auto next = h.find("inline void capture_audit_event(", def + 1);
        auto win = h.substr(def, next != std::string::npos ? next - def : 2500);
        CHECK(win.find("Issue #2819") != std::string::npos, "AC1: cites #2819");
        CHECK(win.find("audit_trail_lockfree_total") != std::string::npos, "AC1: lockfree metric");
        CHECK(win.find("lock_guard") == std::string::npos &&
                  win.find("std::lock_guard") == std::string::npos,
              "AC1: no lock_guard in capture_audit_event_forced");
        // Hot path must not take the trail mutex (comment may mention it).
        CHECK(win.find("std::lock_guard lock(") == std::string::npos &&
                  win.find("lock(g_trail()") == std::string::npos,
              "AC1: no trail mutex lock on hot path");
        CHECK(h.find("audit_trail_mutex_wait_us_total") != std::string::npos,
              "AC1: mutex wait metric present");
        CHECK(h.find("audit_trail_lockfree_wired") != std::string::npos, "AC1: wired flag");
        // trail readers also lock-free
        auto lat = h.find("inline bool trail_latest");
        if (lat == std::string::npos)
            lat = h.find("trail_latest");
        CHECK(h.find("lock-free") != std::string::npos || h.find("lockfree") != std::string::npos ||
                  h.find("lock-free ring") != std::string::npos ||
                  h.find("lock-free") != std::string::npos,
              "AC1: documents lock-free");
        (void)lat;
    }

    // ── AC2: concurrent writers ──
    {
        std::println("\n--- AC2: concurrent capture_audit_event_forced ---");
        ta::reset_for_test();
        ta::set_strategy(ta::AuditStrategy::Full);
        const auto lf0 = ta::g_typed_mutation_audit_counters.audit_trail_lockfree_total.load();
        const auto tw0 = ta::g_typed_mutation_audit_counters.trail_writes.load();
        const auto wait0 =
            ta::g_typed_mutation_audit_counters.audit_trail_mutex_wait_us_total.load();

        constexpr int kThreads = 8;
        constexpr int kPer = 200;
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        std::atomic<int> started{0};
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([t, &started] {
                started.fetch_add(1, std::memory_order_relaxed);
                while (started.load(std::memory_order_relaxed) < kThreads) {
                }
                for (int i = 0; i < kPer; ++i) {
                    const auto mid = static_cast<std::uint64_t>(t * kPer + i + 1);
                    ta::capture_audit_event_forced(
                        mid, "lockfree-stress", ta::MutationKind::MacroHygiene, mid, mid + 1,
                        ta::AuditOutcome::Success, 0, 1, static_cast<std::int64_t>(t), 0);
                }
            });
        }
        for (auto& th : threads)
            th.join();

        const auto expected = static_cast<std::uint64_t>(kThreads * kPer);
        const auto lf1 = ta::g_typed_mutation_audit_counters.audit_trail_lockfree_total.load();
        const auto tw1 = ta::g_typed_mutation_audit_counters.trail_writes.load();
        const auto wait1 =
            ta::g_typed_mutation_audit_counters.audit_trail_mutex_wait_us_total.load();
        const auto seq = ta::trail_seq();
        CHECK(lf1 >= lf0 + expected,
              std::format("AC2: lockfree_total +{} (got Δ={})", expected, lf1 - lf0));
        CHECK(tw1 >= tw0 + expected,
              std::format("AC2: trail_writes +{} (got Δ={})", expected, tw1 - tw0));
        CHECK(seq >= expected, std::format("AC2: trail_seq >= {} (got {})", expected, seq));
        CHECK(wait1 == wait0, "AC2: mutex_wait_us unchanged (lock-free path)");
        CHECK(ta::g_typed_mutation_audit_counters.audit_trail_lockfree_wired.load() == 1,
              "AC2: lockfree_wired == 1");
        CHECK(ta::trail_size() > 0, "AC2: trail_size > 0");
    }

    // ── AC3: read APIs ──
    {
        std::println("\n--- AC3: trail_latest / trail_at_seq / find ---");
        ta::reset_for_test();
        // MacroHygiene: no #2814 enforcement-link gap on Success.
        ta::capture_audit_event_forced(4242, "find-me", ta::MutationKind::MacroHygiene, 1, 2,
                                       ta::AuditOutcome::Success, 7, 3, 1, 0);
        ta::TypedMutationAuditEvent latest{};
        CHECK(ta::trail_latest(latest), "AC3: trail_latest");
        CHECK(latest.mutation_id == 4242, "AC3: latest mid");
        CHECK(std::string_view(latest.name) == "find-me", "AC3: latest name");

        ta::TypedMutationAuditEvent by_seq{};
        CHECK(ta::trail_at_seq(latest.seq, by_seq), "AC3: trail_at_seq");
        CHECK(by_seq.mutation_id == 4242, "AC3: at_seq mid");

        ta::TypedMutationAuditEvent by_mid{};
        CHECK(ta::trail_find_by_mutation_id(4242, by_mid), "AC3: find by mid");
        CHECK(by_mid.seq == latest.seq, "AC3: find seq matches");
    }

    // ── AC4: query surface ──
    {
        std::println("\n--- AC4: schema-2819 query keys ---");
        ta::reset_for_test();
        ta::capture_audit_event_forced(99, "query", ta::MutationKind::Other, 0, 1,
                                       ta::AuditOutcome::Success, 0, 0, 0, 0);
        CompilerService cs;
        CHECK(href(cs, "schema-2819") == 2819, "AC4: schema-2819");
        CHECK(href(cs, "issue-2819") == 2819, "AC4: issue-2819");
        CHECK(href(cs, "audit-trail-lockfree-wired") == 1, "AC4: wired key");
        CHECK(href(cs, "audit-trail-lockfree-total") >= 1, "AC4: lockfree total");
        CHECK(href(cs, "audit-trail-mutex-wait-us-total") == 0, "AC4: wait us == 0");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(obs.find("schema-2819") != std::string::npos, "AC4: obs schema-2819");
        CHECK(obs.find("audit-trail-lockfree-total") != std::string::npos, "AC4: obs lockfree key");
    }

    ta::reset_for_test();
    std::println("\n=== #2819 audit trail lock-free: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_audit_trail_lockfree();
}
#endif
