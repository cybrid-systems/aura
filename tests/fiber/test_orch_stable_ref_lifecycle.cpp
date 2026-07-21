// @category: unit
// @reason: Issue #1879 — agent_spawn / join / fiber resume-steal force
// Issue #1879 (#1978 renamed): issue# moved from filename to header.
// StableNodeRef provenance validation + auto pin/refresh + linear probe.
//
//   AC1: source cites #1879; orch spawn body exit + join provenance
//   AC2: query:orch-module-stats schema-1879 provenance fields
//   AC3: spawn+join advances orch stable-ref / linear counters
//   AC4: complete_post_resume_steal_refresh bumps CompilerMetrics orch_*
//   AC5: 200-iter concurrent spawn/join stress — counters monotonic
//   AC6: existing orch spawn still joins Ok (no regression)

#include "compiler/observability_metrics.h"
#include "orch/orch.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
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
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

struct SchedRunner {
    Scheduler& sched;
    std::thread thr;
    explicit SchedRunner(Scheduler& s)
        : sched(s)
        , thr([&s] { s.run(); }) {}
    ~SchedRunner() {
        sched.stop();
        if (thr.joinable())
            thr.join();
    }
};

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1879 source surface ---");
        auto spawn = read_first({"src/orch/agent_spawn.h", "../src/orch/agent_spawn.h"});
        auto mut = read_first({"src/compiler/evaluator_fiber_mutation.cpp",
                               "../src/compiler/evaluator_fiber_mutation.cpp"});
        auto agent = read_first({"src/compiler/evaluator_primitives_agent.cpp",
                                 "../src/compiler/evaluator_primitives_agent.cpp"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!spawn.empty() && spawn.find("#1879") != std::string::npos,
              "agent_spawn cites #1879");
        CHECK(spawn.find("orch_agent_body_exit_provenance") != std::string::npos,
              "body exit provenance");
        CHECK(spawn.find("orch_post_join_provenance") != std::string::npos, "join provenance");
        CHECK(spawn.find("stable_ref_auto_refresh_total") != std::string::npos,
              "stable_ref metric");
        CHECK(spawn.find("fiber_steal_provenance_enforced_total") != std::string::npos,
              "steal provenance metric");
        CHECK(spawn.find("linear_violation_prevented_total") != std::string::npos, "linear metric");
        CHECK(!mut.empty() &&
                  mut.find("orch_fiber_steal_provenance_enforced_total") != std::string::npos,
              "resume bumps orch steal metric");
        CHECK(mut.find("orch_stable_ref_auto_refresh_total") != std::string::npos,
              "join/resume bumps orch stable-ref");
        CHECK(!agent.empty() && agent.find("schema-1879") != std::string::npos,
              "orch-module-stats schema-1879");
        CHECK(!hdr.empty() && hdr.find("orch_stable_ref_auto_refresh_total") != std::string::npos,
              "CompilerMetrics orch fields");
    }

    // ── AC2: query:orch-module-stats fields ──
    {
        std::println("\n--- AC2: orch-module-stats #1879 fields ---");
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:orch-module-stats"))");
        CHECK(h && is_hash(*h), "stats hash");
        CHECK(href(cs, "schema") == 1588, "schema 1588 preserved");
        CHECK(href(cs, "schema-1879") == 1879, "schema-1879");
        CHECK(href(cs, "provenance-mandate-wired") == 1, "provenance-mandate-wired");
        CHECK(href(cs, "stable-ref-auto-refresh-total") >= 0, "stable-ref field");
        CHECK(href(cs, "fiber-steal-provenance-enforced-total") >= 0, "steal field");
        CHECK(href(cs, "linear-violation-prevented-total") >= 0, "linear field");
        CHECK(href(cs, "phase") >= 1, "phase");
    }

    // ── AC3: spawn+join advances OrchModuleStats ──
    {
        std::println("\n--- AC3: spawn+join advances orch counters ---");
        std::uint64_t r0 = 0, s0 = 0, l0 = 0;
        aura::orch::snapshot_orch_provenance_stats(r0, s0, l0);
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<int> ran{0};
        auto h = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "ac3", .body = [&] {
                        ran.fetch_add(1, std::memory_order_relaxed);
                        Fiber::yield(YieldReason::Explicit);
                    }});
        CHECK(h.ok, "spawn ok");
        auto jr = aura::orch::join_agent(h, std::optional<std::uint64_t>{5000});
        CHECK(jr.status == JoinStatus::Ok, "join Ok");
        CHECK(ran.load() == 1, "body ran");
        std::uint64_t r1 = 0, s1 = 0, l1 = 0;
        aura::orch::snapshot_orch_provenance_stats(r1, s1, l1);
        std::println("  stable-ref {}→{} steal {}→{} linear {}→{}", r0, r1, s0, s1, l0, l1);
        CHECK(r1 > r0, "stable_ref_auto_refresh advanced");
        CHECK(s1 > s0, "fiber_steal_provenance advanced (body exit)");
        CHECK(l1 > l0, "linear_violation_prevented advanced");
    }

    // ── AC4: evaluator complete_post_resume / join metrics ──
    {
        std::println("\n--- AC4: CompilerMetrics orch_* on refresh ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        CHECK(m != nullptr, "metrics");
        const auto steal0 = m->orch_fiber_steal_provenance_enforced_total.load();
        const auto stable0 = m->orch_stable_ref_auto_refresh_total.load();
        const auto lin0 = m->orch_linear_violation_prevented_total.load();
        // Direct closed-loop calls (same path Fiber::resume / join use).
        ev.complete_post_resume_steal_refresh(nullptr);
        CHECK(m->orch_fiber_steal_provenance_enforced_total.load() > steal0,
              "resume bumps orch_fiber_steal");
        CHECK(m->orch_stable_ref_auto_refresh_total.load() > stable0,
              "resume bumps orch_stable_ref");
        const auto stable1 = m->orch_stable_ref_auto_refresh_total.load();
        ev.complete_post_join_linear_enforcement(nullptr);
        CHECK(m->orch_stable_ref_auto_refresh_total.load() > stable1, "join bumps orch_stable_ref");
        CHECK(m->orch_linear_violation_prevented_total.load() > lin0, "join bumps orch_linear");
        // query surface reflects max(orch process, metrics)
        CHECK(href(cs, "stable-ref-auto-refresh-total") >= 1, "query stable-ref after refresh");
        CHECK(href(cs, "fiber-steal-provenance-enforced-total") >= 1, "query steal after refresh");
    }

    // ── AC5: stress concurrent spawn/join ──
    {
        std::println("\n--- AC5: concurrent spawn/join stress ---");
        std::uint64_t r0 = 0, s0 = 0, l0 = 0;
        aura::orch::snapshot_orch_provenance_stats(r0, s0, l0);
        Scheduler sched(4);
        SchedRunner runner(sched);
        // Batches of 50 keep scheduler load predictable under sandbox CI.
        constexpr int kBatches = 4;
        constexpr int kPerBatch = 50;
        constexpr int kN = kBatches * kPerBatch;
        std::atomic<int> done{0};
        for (int b = 0; b < kBatches; ++b) {
            std::vector<aura::orch::AgentHandle> handles;
            handles.reserve(kPerBatch);
            for (int i = 0; i < kPerBatch; ++i) {
                handles.push_back(aura::orch::spawn_agent_with_mailbox(
                    sched, {.name = std::format("s{}-{}", b, i), .body = [&] {
                                done.fetch_add(1, std::memory_order_relaxed);
                                Fiber::yield(YieldReason::Explicit);
                            }}));
            }
            auto jr = aura::orch::join_agents(handles, std::optional<std::uint64_t>{60000});
            CHECK(jr.status == JoinStatus::Ok ||
                      static_cast<int>(std::count_if(handles.begin(), handles.end(),
                                                     [](const auto& h) {
                                                         return h.fiber && h.fiber->is_done();
                                                     })) == kPerBatch,
                  std::format("batch {} join", b));
        }
        std::uint64_t r1 = 0, s1 = 0, l1 = 0;
        aura::orch::snapshot_orch_provenance_stats(r1, s1, l1);
        std::println("  after stress: stable-ref={} steal={} linear={} done={}", r1, s1, l1,
                     done.load());
        // Body exit + join each bump counters; allow small races but require
        // clear monotonic growth under 200 agents.
        CHECK(r1 > r0 && (r1 - r0) >= static_cast<std::uint64_t>(kN), "stable-ref grew by >= N");
        CHECK(s1 > s0 && (s1 - s0) >= static_cast<std::uint64_t>(kN) / 2,
              "steal provenance grew substantially");
        CHECK(l1 > l0 && (l1 - l0) >= static_cast<std::uint64_t>(kN), "linear grew by >= N");
        CHECK(done.load() == kN, "all agents ran");
    }

    // ── AC6: regression join Ok ──
    {
        std::println("\n--- AC6: regression spawn/join ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        auto h = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "reg", .body = [] { Fiber::yield(YieldReason::Explicit); }});
        CHECK(h.ok, "spawn");
        auto jr = aura::orch::join_agent(h, std::optional<std::uint64_t>{5000});
        CHECK(jr.status == JoinStatus::Ok, "join Ok");
    }

    std::println("\n=== test_orch_stable_ref_lifecycle_1879: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
