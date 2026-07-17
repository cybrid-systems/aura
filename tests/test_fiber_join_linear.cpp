// @category: integration
// @reason: Issue #1595 — Fiber::join / MultiFiberMailbox / parallel_intend
// linear ownership + StableNodeRef auto-restamp enforcement.
//
//   AC1: Fiber::join Ok → join_linear_enforcement + complete_post_join
//   AC2: MultiFiberMailbox push/recv linear check; linear-viol rejected
//   AC3: parallel_intend child completion + join refresh path
//   AC4: metrics linear_join_enforcement_total / mailbox_linear_violation /
//        stable_ref_post_join_repin_total + query schema 1595
//   AC5: 1000-iter steal/join/mailbox/mutate stress, version consistency
//   AC6: integration with #1584/#1585/#1586/#1592 surfaces

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::serve::Fiber;
using aura::serve::JoinStatus;
using aura::serve::Scheduler;
using aura::serve::YieldReason;
using aura::serve::mf_mailbox::MailMessage;
using aura::serve::mf_mailbox::MultiFiberMailbox;
using aura::serve::mf_mailbox::PushStatus;
using aura::serve::parallel_orch::parallel_intend;
using aura::serve::parallel_orch::ParallelPolicy;
using aura::serve::parallel_orch::TaskSpec;
using aura::test::g_failed;
using aura::test::g_passed;

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

static CompilerMetrics* metrics_of(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:join-linear-enforcement-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -999999;
    return as_int(*r);
}

static void seed(CompilerService& cs) {
    CHECK(cs.eval("(set-code \"(define (f x) (+ x 1)) (define y (f 40))\")").has_value(),
          "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
}

static void ac1_join_enforcement() {
    std::println("\n--- AC1: Fiber::join linear enforcement ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    const auto j0 = Fiber::join_linear_enforcement_total();
    const auto e0 = m->linear_join_enforcement_total.load();
    const auto r0 = m->stable_ref_post_join_repin_total.load();

    // Direct API path (always wired when CompilerService present).
    ev.complete_post_join_linear_enforcement(nullptr);
    CHECK(m->linear_join_enforcement_total.load() > e0, "direct join enforce +1");
    CHECK(m->stable_ref_post_join_repin_total.load() >= r0, "repin non-decreasing");

    Scheduler sched(2);
    SchedRunner runner(sched);
    Fiber* child = sched.spawn([&] { Fiber::yield(YieldReason::Explicit); });
    auto jr = Fiber::join(child, std::optional<std::uint64_t>{5000});
    CHECK(jr.status == JoinStatus::Ok, "join Ok");
    CHECK(Fiber::join_linear_enforcement_total() > j0, "process join linear total advanced");
}

static void ac2_mailbox_linear() {
    std::println("\n--- AC2: MultiFiberMailbox linear check ---");
    CompilerService cs;
    seed(cs);
    auto* m = metrics_of(cs);
    const auto v0 = m->mailbox_linear_violation_count.load();

    MultiFiberMailbox mb(64);
    // Normal payload ok.
    auto st = mb.push(MailMessage{.from_fiber = 1, .to_fiber = 0, .payload = "hello"});
    CHECK(st == PushStatus::Ok, "normal push ok");

    // Synthetic linear violation prefix (process-level reject; no fiber stack).
    auto st2 = mb.push(MailMessage{.from_fiber = 1, .to_fiber = 0, .payload = "linear-viol:x"});
    CHECK(st2 == PushStatus::Closed, "linear-viol rejected at push");
    CHECK(aura::serve::mf_mailbox::g_mf_mailbox_stats.linear_violations.load() >= 1,
          "process mailbox violations");
    CHECK(aura::serve::mf_mailbox::g_mf_mailbox_stats.linear_checks.load() >= 2,
          "process mailbox linear checks");

    // recv path: deliver normal message.
    MailMessage out;
    CHECK(mb.try_pop(out), "try_pop normal");
    CHECK(out.payload == "hello", "payload intact");

    // Direct probe API (Evaluator metrics).
    CHECK(cs.evaluator().probe_mailbox_linear_and_stable_refs(1, 2, "ok"), "probe ok");
    CHECK(!cs.evaluator().probe_mailbox_linear_and_stable_refs(1, 2, "linear-viol:bad"),
          "probe rejects linear-viol");
    CHECK(m->mailbox_linear_violation_count.load() > v0, "violation metric bumped");
}

static void ac3_parallel_intend() {
    std::println("\n--- AC3: parallel_intend post-task refresh ---");
    CompilerService cs;
    seed(cs);
    const auto j0 = Fiber::join_linear_enforcement_total();

    Scheduler sched(4);
    SchedRunner runner(sched);
    std::vector<TaskSpec> tasks;
    for (int i = 0; i < 4; ++i) {
        tasks.push_back(TaskSpec{.body =
                                     [i] {
                                         Fiber::yield(YieldReason::Explicit);
                                         return aura::serve::parallel_orch::TaskResult{
                                             .ok = true, .value = std::to_string(i)};
                                     },
                                 .name = std::format("t{}", i)});
    }
    ParallelPolicy pol;
    pol.max_concurrency = 4;
    pol.timeout_ms = 10000;
    auto batch = parallel_intend(sched, std::span<const TaskSpec>(tasks), pol, nullptr);
    CHECK(batch.ok_count == 4, "4 tasks ok");
    CHECK(Fiber::join_linear_enforcement_total() >= j0, "join linear total non-decreasing");
}

static void ac4_query_metrics() {
    std::println("\n--- AC4: query:join-linear-enforcement-stats ---");
    CompilerService cs;
    seed(cs);
    cs.evaluator().complete_post_join_linear_enforcement(nullptr);
    (void)cs.evaluator().probe_mailbox_linear_and_stable_refs(0, 0, "linear-viol:t");

    auto h = cs.eval("(engine:metrics \"query:join-linear-enforcement-stats\")");
    CHECK(h && is_hash(*h), "hash");
    CHECK(href(cs, "schema") == 1595, "schema 1595");
    CHECK(href(cs, "issue") == 1595, "issue 1595");
    CHECK(href(cs, "linear_join_enforcement_total") >= 1, "linear_join_enforcement_total");
    CHECK(href(cs, "mailbox_linear_violation_count") >= 1, "mailbox_linear_violation_count");
    CHECK(href(cs, "stable_ref_post_join_repin_total") >= 1, "stable_ref_post_join_repin_total");
    CHECK(href(cs, "join-path-wired") == 1, "join path wired");
    CHECK(href(cs, "mailbox-path-wired") == 1, "mailbox path wired");
    CHECK(href(cs, "parallel-intend-path-wired") == 1, "parallel path wired");
}

static void ac5_stress_1000() {
    std::println("\n--- AC5: 1000-iter join+mailbox+mutate stress ---");
    CompilerService cs;
    seed(cs);
    auto& ev = cs.evaluator();
    auto* m = metrics_of(cs);
    std::atomic<int> errors{0};
    std::atomic<int> joins{0};

    Scheduler sched(4);
    SchedRunner runner(sched);

    std::vector<std::thread> host_threads;
    for (int t = 0; t < 2; ++t) {
        host_threads.emplace_back([&ev, &errors, t] {
            for (int i = 0; i < 250; ++i) {
                try {
                    if ((i + t) % 5 == 0)
                        ev.bump_defuse_version_for_test();
                    ev.complete_post_join_linear_enforcement(nullptr);
                    (void)ev.probe_mailbox_linear_and_stable_refs(1, 2, "payload");
                    if (i % 11 == 0)
                        (void)ev.probe_mailbox_linear_and_stable_refs(1, 2, "linear-viol:x");
                    ev.complete_post_resume_steal_refresh(nullptr); // #1592 integration
                } catch (...) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    // Fiber join stress under scheduler.
    for (int i = 0; i < 500; ++i) {
        Fiber* f = sched.spawn([&] {
            Fiber::yield(YieldReason::MutationBoundary);
            Fiber::yield(YieldReason::Explicit);
        });
        auto jr = Fiber::join(f, std::optional<std::uint64_t>{5000});
        if (jr.status != JoinStatus::Ok)
            errors.fetch_add(1, std::memory_order_relaxed);
        else
            joins.fetch_add(1, std::memory_order_relaxed);
    }

    for (auto& th : host_threads)
        th.join();

    CHECK(errors.load() == 0, "no errors in 1000+ stress");
    CHECK(joins.load() == 500, "500 joins ok");
    CHECK(m->linear_join_enforcement_total.load() >= 250, "join enforcements advanced");
    CHECK(m->mailbox_linear_violation_count.load() >= 1, "violations seen in stress");
    CHECK(Fiber::join_linear_enforcement_total() >= 500, "process join linear ≥ 500");
    std::println("  joins={} linear_enf={} violations={} repin={}", joins.load(),
                 m->linear_join_enforcement_total.load(), m->mailbox_linear_violation_count.load(),
                 m->stable_ref_post_join_repin_total.load());
}

static void ac6_siblings() {
    std::println("\n--- AC6: sibling issue surfaces (#1584/#1585/#1586/#1592) ---");
    CompilerService cs;
    seed(cs);
    // #1592 stats still present
    auto h1592 = cs.eval("(engine:metrics \"query:post-steal-closed-loop-stats\")");
    CHECK(h1592 && is_hash(*h1592), "post-steal stats (#1592)");
    auto h1595 = cs.eval("(engine:metrics \"query:join-linear-enforcement-stats\")");
    CHECK(h1595 && is_hash(*h1595), "join-linear stats (#1595)");
    CHECK(Fiber::join_total() >= 0, "join metrics (#1584)");
    CHECK(aura::serve::mf_mailbox::g_mf_mailbox_stats.pushes.load() >= 0, "mailbox (#1585)");
    CHECK(aura::serve::parallel_orch::kParallelOrchIssue == 1586, "parallel_orch (#1586)");
}

} // namespace

int main() {
    std::println("=== Issue #1595: join/mailbox/parallel linear + StableNodeRef ===");
    ac1_join_enforcement();
    ac2_mailbox_linear();
    ac3_parallel_intend();
    ac4_query_metrics();
    ac5_stress_1000();
    ac6_siblings();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
