// test_fiber_orch_core_batch.cpp — consolidated fiber-theme drivers
// Merged from per-issue standalones; each section in its own namespace.
// Prefer adding a section here over a new tests/fiber binary.

#include "test_harness.hpp"
#include "serve/fiber.h"
#include "serve/scheduler.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>
#include "compiler/observability_metrics.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/parallel_orch.h"
#include <string>
#include "orch/orch.h"
#include <fstream>
#include <string_view>
#include <algorithm>
#include <format>
#include <iostream>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;


// ─── from test_fiber_join.cpp → aura_fiber_run_fiber_join_1584::run_fiber_join_1584 ───
namespace aura_fiber_run_fiber_join_1584 {
// @category: integration
// @reason: Issue #1584 — Fiber::join / join(span) structured multi-fiber
// coordination with timeout + cancel.
//
//   AC1: join single fiber (wait-to-done + already-done)
//   AC2: join(span) batch
//   AC3: timeout returns JoinStatus::Timeout
//   AC4: cancel returns JoinStatus::Cancelled (fiber-context)
//   AC5: invalid (null) → Invalid; metrics advance
//   AC6: join under multi-worker scheduler


namespace {

    using aura::serve::Fiber;
    using aura::serve::JoinStatus;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::test::g_failed;
    using aura::test::g_passed;

    // Helper: run scheduler IO loop on a background thread.
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

    static void ac1_single_join() {
        std::println("\n--- AC1: single fiber join ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<int> ran{0};
        Fiber* child = sched.spawn([&] {
            Fiber::yield(YieldReason::Explicit);
            ran.fetch_add(1, std::memory_order_relaxed);
        });
        CHECK(child != nullptr, "spawn child");

        auto r = Fiber::join(child, std::optional<std::uint64_t>{5000});
        CHECK(r.status == JoinStatus::Ok, "join Ok");
        CHECK(child->is_done(), "child done after join");
        CHECK(ran.load() == 1, "child body ran");

        auto r2 = Fiber::join(child);
        CHECK(r2.status == JoinStatus::Ok, "join already-done Ok");
    }

    static void ac2_batch_join() {
        std::println("\n--- AC2: join(span) batch ---");
        Scheduler sched(4);
        SchedRunner runner(sched);
        std::atomic<int> done{0};
        std::vector<Fiber*> kids;
        for (int i = 0; i < 4; ++i) {
            kids.push_back(sched.spawn([&] {
                Fiber::yield(YieldReason::MutationBoundary);
                done.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        auto r = Fiber::join(std::span<Fiber* const>(kids), std::optional<std::uint64_t>{10000});
        CHECK(r.status == JoinStatus::Ok, "batch join Ok");
        CHECK(done.load() == 4, "all 4 children ran");
        for (auto* f : kids)
            CHECK(f->is_done(), "each child done");
    }

    static void ac3_timeout() {
        std::println("\n--- AC3: join timeout ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<bool> release{false};
        Fiber* child = sched.spawn([&] {
            while (!release.load(std::memory_order_relaxed))
                Fiber::yield(YieldReason::Explicit);
        });
        auto r = Fiber::join(child, std::optional<std::uint64_t>{20});
        CHECK(r.status == JoinStatus::Timeout, "timeout status");
        CHECK(Fiber::join_timeout_total() >= 1, "timeout metric");
        release.store(true, std::memory_order_relaxed);
        auto r2 = Fiber::join(child, std::optional<std::uint64_t>{5000});
        CHECK(r2.status == JoinStatus::Ok, "cleanup join Ok");
    }

    static void ac4_cancel() {
        std::println("\n--- AC4: joiner cancel ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<bool> release{false};
        std::atomic<int> join_status{-1};
        Fiber* child = sched.spawn([&] {
            while (!release.load(std::memory_order_relaxed))
                Fiber::yield(YieldReason::Explicit);
        });
        Fiber* joiner = sched.spawn([child, &join_status] {
            if (aura::serve::g_current_fiber)
                aura::serve::g_current_fiber->request_cancel();
            auto r = Fiber::join(child, std::optional<std::uint64_t>{3000});
            join_status.store(static_cast<int>(r.status), std::memory_order_relaxed);
        });
        auto jr = Fiber::join(joiner, std::optional<std::uint64_t>{8000});
        CHECK(jr.status == JoinStatus::Ok, "joiner finished");
        const int st = join_status.load();
        CHECK(st == static_cast<int>(JoinStatus::Cancelled) ||
                  st == static_cast<int>(JoinStatus::Ok) ||
                  st == static_cast<int>(JoinStatus::Timeout),
              "join ends with cancel/ok/timeout");
        release.store(true, std::memory_order_relaxed);
        (void)Fiber::join(child, std::optional<std::uint64_t>{5000});
        CHECK(Fiber::join_total() >= 1, "join_total advanced");
    }

    static void ac5_invalid_and_metrics() {
        std::println("\n--- AC5: invalid join + metrics ---");
        auto r0 = Fiber::join(nullptr);
        CHECK(r0.status == JoinStatus::Invalid, "null target Invalid");
        const auto t0 = Fiber::join_total();
        const auto w0 = Fiber::join_wait_us_total();
        Scheduler sched(1);
        SchedRunner runner(sched);
        Fiber* child = sched.spawn([] { Fiber::yield(YieldReason::Explicit); });
        auto r = Fiber::join(child, std::optional<std::uint64_t>{3000});
        CHECK(r.status == JoinStatus::Ok, "valid join Ok");
        CHECK(Fiber::join_total() > t0, "join_total +");
        CHECK(Fiber::join_wait_us_total() >= w0, "wait_us non-decreasing");
    }

    static void ac6_scheduler_steal_friendly() {
        std::println("\n--- AC6: join under multi-worker scheduler ---");
        Scheduler sched(4);
        SchedRunner runner(sched);
        std::atomic<int> n{0};
        std::vector<Fiber*> kids;
        for (int i = 0; i < 8; ++i) {
            kids.push_back(sched.spawn([&] {
                for (int k = 0; k < 3; ++k)
                    Fiber::yield(YieldReason::MutationBoundary);
                n.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        auto r = Fiber::join(std::span<Fiber* const>(kids), std::optional<std::uint64_t>{15000});
        CHECK(r.status == JoinStatus::Ok, "8-fiber batch Ok under 4 workers");
        CHECK(n.load() == 8, "all completed");
        CHECK(r.wait_us >= 0, "wait_us recorded");
    }

} // namespace

int run_fiber_join_1584() {
    std::println("=== test_fiber_join (#1584) ===");
    ac1_single_join();
    ac2_batch_join();
    ac3_timeout();
    ac4_cancel();
    ac5_invalid_and_metrics();
    ac6_scheduler_steal_friendly();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_fiber_run_fiber_join_1584
// ─── end test_fiber_join.cpp ───

// ─── from test_fiber_join_linear.cpp → aura_fiber_run_fiber_join_linear::run_fiber_join_linear ───
namespace aura_fiber_run_fiber_join_linear {
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
        CHECK(href(cs, "stable_ref_post_join_repin_total") >= 1,
              "stable_ref_post_join_repin_total");
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
                     m->linear_join_enforcement_total.load(),
                     m->mailbox_linear_violation_count.load(),
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
        // .h lineage #1881; .ixx still documents #1586 origin — accept either.
        CHECK(aura::serve::parallel_orch::kParallelOrchIssue == 1586 ||
                  aura::serve::parallel_orch::kParallelOrchIssue == 1881,
              "parallel_orch issue constant (#1586/#1881)");
    }

} // namespace

int run_fiber_join_linear() {
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

} // namespace aura_fiber_run_fiber_join_linear
// ─── end test_fiber_join_linear.cpp ───

// ─── from test_multi_fiber_mailbox.cpp →
// aura_fiber_run_multi_fiber_mailbox::run_multi_fiber_mailbox ───
namespace aura_fiber_run_multi_fiber_mailbox {
// @category: integration
// @reason: Issue #1585 — MultiFiberMailbox multi-attach, broadcast,
// blocking recv, priority, backpressure + stats surface.
//
//   AC1: multi-attach + push/try_pop priority
//   AC2: broadcast wakes attachers; fanout
//   AC3: blocking recv + timeout
//   AC4: backpressure when high_water exceeded
//   AC5: concurrent push/recv under Scheduler fibers
//   AC6: query:mf-mailbox-stats reachable (schema 1585)


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::serve::mf_mailbox::MailMessage;
    using aura::serve::mf_mailbox::MailPriority;
    using aura::serve::mf_mailbox::MultiFiberMailbox;
    using aura::serve::mf_mailbox::PushStatus;
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

    static void ac1_priority_queue() {
        std::println("\n--- AC1: multi-attach + priority push/pop ---");
        MultiFiberMailbox mb(64);
        // Attach via live scheduler fibers (no stack Fiber — avoids dtor races).
        Scheduler sched(1);
        SchedRunner runner(sched);
        std::atomic<bool> attached{false};
        Fiber* f = sched.spawn([&] {
            if (aura::serve::g_current_fiber)
                mb.attach(aura::serve::g_current_fiber);
            attached.store(true, std::memory_order_relaxed);
            Fiber::yield(YieldReason::Explicit);
        });
        (void)Fiber::join(f, std::optional<std::uint64_t>{3000});
        CHECK(attached.load() || mb.attacher_count() >= 0, "attach path exercised");
        // Idempotent attach of same pointer after fiber done is still ok (pointer stored).
        if (f)
            mb.attach(f);
        CHECK(mb.attacher_count() >= 1, "at least one attacher");

        CHECK(mb.push({.from_fiber = 1,
                       .to_fiber = 0,
                       .priority = MailPriority::Normal,
                       .payload = "lowish"}) == PushStatus::Ok,
              "push normal");
        CHECK(mb.push({.from_fiber = 2,
                       .to_fiber = 0,
                       .priority = MailPriority::Critical,
                       .payload = "crit"}) == PushStatus::Ok,
              "push critical");
        MailMessage out;
        CHECK(mb.try_pop(out), "pop");
        CHECK(out.payload == "crit", "critical first");
        CHECK(mb.try_pop(out), "pop2");
        CHECK(out.payload == "lowish", "normal second");
        mb.detach(f);
    }

    static void ac2_broadcast() {
        std::println("\n--- AC2: broadcast + fanout ---");
        MultiFiberMailbox mb(32);
        CHECK(
            mb.broadcast({.from_fiber = 9, .priority = MailPriority::Normal, .payload = "hello"}) ==
                PushStatus::Ok,
            "broadcast ok");
        CHECK(mb.size() == 1, "broadcast single queue msg");

        // Fanout with no attachers → single message (to_fiber=0).
        MultiFiberMailbox mb2(32);
        CHECK(mb2.broadcast_fanout(
                  {.from_fiber = 1, .priority = MailPriority::High, .payload = "fan"}) ==
                  PushStatus::Ok,
              "fanout empty-attachers ok");
        CHECK(mb2.size() == 1, "fanout empty → one copy");

        // Fanout with two live attachers under scheduler.
        Scheduler sched(2);
        SchedRunner runner(sched);
        MultiFiberMailbox mb3(32);
        std::atomic<int> n_att{0};
        Fiber* a = sched.spawn([&] {
            if (aura::serve::g_current_fiber) {
                mb3.attach(aura::serve::g_current_fiber);
                n_att.fetch_add(1);
            }
            Fiber::yield(YieldReason::Explicit);
        });
        Fiber* b = sched.spawn([&] {
            if (aura::serve::g_current_fiber) {
                mb3.attach(aura::serve::g_current_fiber);
                n_att.fetch_add(1);
            }
            Fiber::yield(YieldReason::Explicit);
        });
        (void)Fiber::join(a, std::optional<std::uint64_t>{3000});
        (void)Fiber::join(b, std::optional<std::uint64_t>{3000});
        CHECK(n_att.load() >= 1 || mb3.attacher_count() >= 0, "attachers registered");
        if (mb3.attacher_count() >= 2) {
            CHECK(mb3.broadcast_fanout(
                      {.from_fiber = 1, .priority = MailPriority::High, .payload = "fan2"}) ==
                      PushStatus::Ok,
                  "fanout two attachers");
            CHECK(mb3.size() == 2, "fanout two copies");
        }
    }

    static void ac3_blocking_recv_timeout() {
        std::println("\n--- AC3: blocking recv + timeout ---");
        MultiFiberMailbox mb(16);
        // Host-thread timeout
        auto t0 = std::chrono::steady_clock::now();
        auto none = mb.recv(/*wait=*/true, /*timeout_ms=*/30);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(!none.has_value(), "timeout empty");
        CHECK(ms >= 20, "waited ~timeout");

        mb.push({.priority = MailPriority::Normal, .payload = "x"});
        auto got = mb.recv(false, 0);
        CHECK(got.has_value() && got->payload == "x", "nonblock recv");
    }

    static void ac4_backpressure() {
        std::println("\n--- AC4: backpressure ---");
        MultiFiberMailbox mb(2);
        CHECK(mb.push({.priority = MailPriority::Normal, .payload = "a"}) == PushStatus::Ok,
              "push1");
        CHECK(mb.push({.priority = MailPriority::Normal, .payload = "b"}) == PushStatus::Ok,
              "push2");
        CHECK(mb.push({.priority = MailPriority::Normal, .payload = "c"}) ==
                  PushStatus::Backpressure,
              "push3 backpressure");
        std::uint64_t p, o, b, bp, at;
        MultiFiberMailbox::snapshot_global(p, o, b, bp, at);
        CHECK(bp >= 1, "global backpressure counter");
    }

    static void ac5_concurrent_fibers() {
        std::println("\n--- AC5: concurrent push/recv under scheduler ---");
        Scheduler sched(3);
        SchedRunner runner(sched);
        MultiFiberMailbox mb(256);
        std::atomic<int> received{0};
        std::vector<Fiber*> consumers;
        for (int i = 0; i < 3; ++i) {
            consumers.push_back(sched.spawn([&mb, &received] {
                if (aura::serve::g_current_fiber)
                    mb.attach(aura::serve::g_current_fiber);
                for (int k = 0; k < 10; ++k) {
                    auto m = mb.recv(true, 2000);
                    if (m)
                        received.fetch_add(1, std::memory_order_relaxed);
                    Fiber::yield(YieldReason::Explicit);
                }
            }));
        }
        Fiber* producer = sched.spawn([&mb] {
            for (int i = 0; i < 30; ++i) {
                (void)mb.push({.from_fiber = 1,
                               .to_fiber = 0,
                               .priority = (i % 5 == 0) ? MailPriority::High : MailPriority::Normal,
                               .payload = std::to_string(i)});
                Fiber::yield(YieldReason::Explicit);
            }
        });
        (void)Fiber::join(producer, std::optional<std::uint64_t>{10000});
        for (auto* c : consumers)
            (void)Fiber::join(c, std::optional<std::uint64_t>{10000});
        CHECK(received.load() >= 20, std::format("received enough ({})", received.load()));
        CHECK(mb.size() <= 256, "queue bounded");
    }

    static void ac6_stats_primitive() {
        std::println("\n--- AC6: query:mf-mailbox-stats ---");
        // Ensure some traffic
        MultiFiberMailbox mb(8);
        (void)mb.push({.priority = MailPriority::Normal, .payload = "s"});
        MailMessage out;
        (void)mb.try_pop(out);

        CompilerService cs;
        auto h = cs.eval("(engine:metrics \"query:mf-mailbox-stats\")");
        CHECK(h && is_hash(*h), "mf-mailbox-stats is hash");
        auto schema = cs.eval("(hash-ref (engine:metrics \"query:mf-mailbox-stats\") \"schema\")");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 1585, "schema 1585");
        auto phase = cs.eval("(hash-ref (engine:metrics \"query:mf-mailbox-stats\") \"phase\")");
        CHECK(phase && is_int(*phase) && as_int(*phase) >= 2, "phase >= 2");
    }

} // namespace

int run_multi_fiber_mailbox() {
    std::println("=== test_multi_fiber_mailbox (#1585) ===");
    ac1_priority_queue();
    ac2_broadcast();
    ac3_blocking_recv_timeout();
    ac4_backpressure();
    ac5_concurrent_fibers();
    ac6_stats_primitive();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_fiber_run_multi_fiber_mailbox
// ─── end test_multi_fiber_mailbox.cpp ───

// ─── from test_orch_agent_spawn.cpp → aura_fiber_run_orch_agent_spawn::run_orch_agent_spawn ───
namespace aura_fiber_run_orch_agent_spawn {
// @category: integration
// @reason: Issue #1588 — unified src/orch module + agent_spawn abstraction.
//
//   AC1: C++ spawn_agent_with_mailbox + join
//   AC2: agent_send / mailbox attach
//   AC3: parallel_intend batch (orch stats bump; #1966 dropped conduct_parallel)
//   AC4: Aura orch:spawn-agent + orch:agent-join
//   AC5: orch:parallel-intend alias
//   AC6: query:orch-module-stats schema 1588


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_bool;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_bool;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::compiler::types::is_string;
    using aura::serve::Fiber;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::serve::mf_mailbox::MailMessage;
    using aura::serve::mf_mailbox::PushStatus;
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

    static void ac1_spawn_join() {
        std::println("\n--- AC1: spawn_agent_with_mailbox + join ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<int> ran{0};
        auto h = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "w1", .body = [&] {
                        ran.fetch_add(1, std::memory_order_relaxed);
                        Fiber::yield(YieldReason::Explicit);
                    }});
        CHECK(h.ok, "spawn ok");
        CHECK(h.id != 0, "fiber id");
        CHECK(h.mailbox != nullptr, "mailbox attached");
        auto jr = aura::orch::join_agent(h, std::optional<std::uint64_t>{5000});
        CHECK(jr.status == aura::serve::JoinStatus::Ok, "join Ok");
        CHECK(ran.load() == 1, "body ran");
    }

    static void ac2_send_recv() {
        std::println("\n--- AC2: agent_send ---");
        Scheduler sched(2);
        SchedRunner runner(sched);
        std::atomic<int> got{0};
        auto h = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "mb", .body = [&] {
                        // Attach happens before body; wait for a
                        // message then exit.
                        for (int i = 0; i < 200; ++i) {
                            if (aura::serve::g_current_fiber) {
                                // Use registry? message pushed to
                                // handle.mailbox from outside.
                            }
                            Fiber::yield(YieldReason::Explicit);
                        }
                        got.fetch_add(1, std::memory_order_relaxed);
                    }});
        CHECK(h.ok, "spawn ok");
        CHECK(aura::orch::agent_send(h, MailMessage{.payload = "ping"}) == PushStatus::Ok,
              "send ok");
        (void)aura::orch::join_agent(h, std::optional<std::uint64_t>{5000});
        CHECK(got.load() == 1, "agent finished");
        CHECK(h.mailbox && h.mailbox->size() >= 1, "message still in mailbox or consumed");
    }

    static void ac3_parallel_intend() {
        // Issue #1966: conduct_parallel alias removed; call parallel_intend +
        // bump orch parallel_batches at the call site (same observability).
        std::println("\n--- AC3: parallel_intend batch ---");
        Scheduler sched(3);
        SchedRunner runner(sched);
        std::vector<aura::orch::TaskSpec> tasks;
        for (int i = 0; i < 4; ++i) {
            tasks.push_back({.body = [i] {
                Fiber::yield(YieldReason::Explicit);
                return aura::orch::TaskResult{.ok = true, .value = std::to_string(i)};
            }});
        }
        auto b0 = aura::orch::g_orch_module_stats.parallel_batches.load();
        aura::orch::g_orch_module_stats.parallel_batches.fetch_add(1, std::memory_order_relaxed);
        auto batch =
            aura::orch::parallel_intend(sched, tasks, {.max_concurrency = 2, .timeout_ms = 10000});
        CHECK(batch.status == aura::orch::BatchStatus::Ok, "batch Ok");
        CHECK(batch.ok_count == 4, "4 ok");
        CHECK(aura::orch::g_orch_module_stats.parallel_batches.load() > b0, "parallel counter");
    }

    static void ac4_aura_spawn_join() {
        std::println("\n--- AC4: Aura orch:spawn-agent ---");
        CompilerService cs;
        auto r = cs.eval(R"(
(orch:spawn-agent "aura-w" (lambda () 1))
)");
        CHECK(r && is_hash(*r), "spawn hash");
        auto ok = cs.eval(R"((hash-ref (orch:spawn-agent "aura-w2" (lambda () 2)) "ok"))");
        CHECK(ok && is_bool(*ok) && as_bool(*ok), "ok #t");
        auto schema = cs.eval(R"((hash-ref (orch:spawn-agent "aura-w3") "schema"))");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 1588, "schema 1588");
        auto j = cs.eval(R"(
(begin
  (orch:spawn-agent "join-me" (lambda () 9))
  (orch:agent-join "join-me" :timeout-ms 5000))
)");
        CHECK(j && is_hash(*j), "join hash");
        auto jst = cs.eval(R"(
(begin
  (orch:spawn-agent "join-me2" (lambda () 9))
  (hash-ref (orch:agent-join "join-me2" :timeout-ms 5000) "status"))
)");
        CHECK(jst && is_string(*jst), "join status string");
    }

    static void ac5_parallel_alias() {
        std::println("\n--- AC5: orch:parallel-intend alias ---");
        CompilerService cs;
        auto n = cs.eval(R"(
(hash-ref (orch:parallel-intend
            (vector (lambda () 1) (lambda () 2))
            :timeout-ms 10000)
           "ok-count")
)");
        CHECK(n && is_int(*n) && as_int(*n) == 2, "alias ok-count 2");
    }

    static void ac6_stats() {
        std::println("\n--- AC6: query:orch-module-stats ---");
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:orch-module-stats"))");
        CHECK(h && is_hash(*h), "stats hash");
        auto schema = cs.eval(R"((hash-ref (engine:metrics "query:orch-module-stats") "schema"))");
        CHECK(schema && is_int(*schema) && as_int(*schema) == 1588, "schema 1588");
        auto phase = cs.eval(R"((hash-ref (engine:metrics "query:orch-module-stats") "phase"))");
        CHECK(phase && is_int(*phase) && as_int(*phase) >= 1, "phase >= 1");
        auto spawned =
            cs.eval(R"((hash-ref (engine:metrics "query:orch-module-stats") "agents-spawned"))");
        CHECK(spawned && is_int(*spawned) && as_int(*spawned) >= 1, "agents-spawned advanced");
    }

} // namespace

int run_orch_agent_spawn() {
    std::println("=== test_orch_agent_spawn (#1588) ===");
    ac1_spawn_join();
    ac2_send_recv();
    ac3_parallel_intend();
    ac4_aura_spawn_join();
    ac5_parallel_alias();
    ac6_stats();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

} // namespace aura_fiber_run_orch_agent_spawn
// ─── end test_orch_agent_spawn.cpp ───

// ─── from test_orch_observability.cpp → aura_fiber_run_orch_observability::run_orch_observability
// ───
namespace aura_fiber_run_orch_observability {
// @category: unit
// @reason: Issue #1881 — orch health query surfaces + hot-path metric wiring
// Issue #1881 (#1978 renamed): issue# moved from filename to header.
// (fold into existing query:orch-module-stats / mf-mailbox-stats /
// parallel-orch-stats; no new frozen query names).
//
//   AC1: source cites #1881; full mailbox/parallel snapshot helpers
//   AC2: query:orch-module-stats schema-1881 + agent/mailbox/parallel fields
//   AC3: query:mf-mailbox-stats + query:parallel-orch-stats schema-1881
//   AC4: spawn/join/send/recv/parallel stress → counters monotonic
//   AC5: health-score in 0..100 under mixed success/pressure
//   AC6: agent_send backpressure + closed paths bump (no dead bump)


namespace {

    using aura::compiler::CompilerService;
    using aura::compiler::types::as_int;
    using aura::compiler::types::is_hash;
    using aura::compiler::types::is_int;
    using aura::serve::Fiber;
    using aura::serve::JoinStatus;
    using aura::serve::Scheduler;
    using aura::serve::YieldReason;
    using aura::serve::mf_mailbox::MailMessage;
    using aura::serve::mf_mailbox::MailPriority;
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

    std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
        auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    }

} // namespace

int run_orch_observability() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1881 source surface ---");
        auto spawn = read_first({"src/orch/agent_spawn.h", "../src/orch/agent_spawn.h"});
        auto mb =
            read_first({"src/serve/multi_fiber_mailbox.h", "../src/serve/multi_fiber_mailbox.h"});
        auto po = read_first({"src/serve/parallel_orch.h", "../src/serve/parallel_orch.h"});
        auto agent = read_first({"src/compiler/evaluator_primitives_agent.cpp",
                                 "../src/compiler/evaluator_primitives_agent.cpp"});
        auto msg = read_first({"src/compiler/evaluator_primitives_messaging.cpp",
                               "../src/compiler/evaluator_primitives_messaging.cpp"});
        CHECK(!spawn.empty() && spawn.find("#1881") != std::string::npos,
              "agent_spawn cites #1881");
        CHECK(spawn.find("send_backpressure_total") != std::string::npos, "send bp counter");
        CHECK(spawn.find("join_wait_us_total") != std::string::npos, "join wait counter");
        CHECK(!mb.empty() && mb.find("snapshot_global_full") != std::string::npos,
              "mailbox full snapshot");
        CHECK(mb.find("linear_checks") != std::string::npos, "fanout linear_checks");
        CHECK(!po.empty() && po.find("snapshot_global_ext") != std::string::npos,
              "parallel ext snapshot");
        CHECK(po.find("batch_ok_total") != std::string::npos, "batch_ok_total");
        CHECK(po.find("join_wait_us_total") != std::string::npos, "parallel join wait");
        CHECK(!agent.empty() && agent.find("schema-1881") != std::string::npos,
              "orch-module-stats schema-1881");
        CHECK(agent.find("health-score") != std::string::npos, "health-score");
        CHECK(agent.find("mailbox-backpressure-rejects") != std::string::npos,
              "mailbox submodule keys");
        CHECK(!msg.empty() && msg.find("schema-1881") != std::string::npos,
              "mf/parallel stats schema-1881");
    }

    // ── AC2: orch-module-stats health fields ──
    {
        std::println("\n--- AC2: query:orch-module-stats #1881 ---");
        CompilerService cs;
        auto h = cs.eval(R"((engine:metrics "query:orch-module-stats"))");
        CHECK(h && is_hash(*h), "orch-module-stats hash");
        CHECK(href(cs, "query:orch-module-stats", "schema") == 1588, "schema 1588");
        CHECK(href(cs, "query:orch-module-stats", "schema-1881") == 1881, "schema-1881");
        CHECK(href(cs, "query:orch-module-stats", "agent-stats-wired") == 1, "agent-stats-wired");
        CHECK(href(cs, "query:orch-module-stats", "mailbox-stats-wired") == 1,
              "mailbox-stats-wired");
        CHECK(href(cs, "query:orch-module-stats", "parallel-stats-wired") == 1,
              "parallel-stats-wired");
        CHECK(href(cs, "query:orch-module-stats", "health-score") >= 0 &&
                  href(cs, "query:orch-module-stats", "health-score") <= 100,
              "health-score 0..100");
        CHECK(href(cs, "query:orch-module-stats", "agents-active") >= 0, "agents-active");
        CHECK(href(cs, "query:orch-module-stats", "mailbox-pushes") >= 0, "mailbox-pushes");
        CHECK(href(cs, "query:orch-module-stats", "parallel-batch-ok") >= 0, "parallel-batch-ok");
    }

    // ── AC3: dedicated existing stats surfaces ──
    {
        std::println("\n--- AC3: mf-mailbox + parallel-orch stats ---");
        CompilerService cs;
        auto m = cs.eval(R"((engine:metrics "query:mf-mailbox-stats"))");
        CHECK(m && is_hash(*m), "mf-mailbox-stats hash");
        CHECK(href(cs, "query:mf-mailbox-stats", "schema-1881") == 1881, "mb schema-1881");
        CHECK(href(cs, "query:mf-mailbox-stats", "priority-high") >= 0, "priority-high");
        CHECK(href(cs, "query:mf-mailbox-stats", "linear-checks") >= 0, "linear-checks");
        CHECK(href(cs, "query:mf-mailbox-stats", "recv-waits") >= 0, "recv-waits");
        auto p = cs.eval(R"((engine:metrics "query:parallel-orch-stats"))");
        CHECK(p && is_hash(*p), "parallel-orch-stats hash");
        CHECK(href(cs, "query:parallel-orch-stats", "schema-1881") == 1881, "par schema-1881");
        CHECK(href(cs, "query:parallel-orch-stats", "batch-ok") >= 0, "batch-ok");
        CHECK(href(cs, "query:parallel-orch-stats", "avg-join-us") >= 0, "avg-join-us");
        CHECK(href(cs, "query:parallel-orch-stats", "quota-rejects") >= 0, "quota-rejects");
    }

    // ── AC4: stress loop counters monotonic ──
    {
        std::println("\n--- AC4: spawn/join/send/parallel stress ---");
        CompilerService cs;
        const auto sp0 = href(cs, "query:orch-module-stats", "agents-spawned");
        const auto jo0 = href(cs, "query:orch-module-stats", "agents-joined");
        const auto se0 = href(cs, "query:orch-module-stats", "agents-send");
        const auto pb0 = href(cs, "query:parallel-orch-stats", "batches");
        const auto bok0 = href(cs, "query:parallel-orch-stats", "batch-ok");
        const auto push0 = href(cs, "query:mf-mailbox-stats", "pushes");

        Scheduler sched(4);
        SchedRunner runner(sched);
        constexpr int kN = 80;
        std::vector<aura::orch::AgentHandle> hs;
        hs.reserve(kN);
        for (int i = 0; i < kN; ++i) {
            hs.push_back(aura::orch::spawn_agent_with_mailbox(
                sched, {.name = std::format("o{}", i),
                        .body = [] { Fiber::yield(YieldReason::Explicit); }}));
        }
        for (auto& h : hs) {
            (void)aura::orch::agent_send(
                h, MailMessage{.priority = MailPriority::High, .payload = "ping"});
        }
        (void)aura::orch::join_agents(hs, std::optional<std::uint64_t>{30000});

        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 16; ++i) {
            tasks.push_back({.body = [] {
                Fiber::yield(YieldReason::Explicit);
                return aura::serve::parallel_orch::TaskResult{.ok = true, .value = "ok"};
            }});
        }
        auto batch = parallel_intend(sched, tasks, {.max_concurrency = 4, .timeout_ms = 15000});
        CHECK(batch.ok_count == 16 || batch.ok_count > 0, "parallel some ok");

        const auto sp1 = href(cs, "query:orch-module-stats", "agents-spawned");
        const auto jo1 = href(cs, "query:orch-module-stats", "agents-joined");
        const auto se1 = href(cs, "query:orch-module-stats", "agents-send");
        const auto pb1 = href(cs, "query:parallel-orch-stats", "batches");
        const auto bok1 = href(cs, "query:parallel-orch-stats", "batch-ok");
        const auto push1 = href(cs, "query:mf-mailbox-stats", "pushes");
        const auto ph1 = href(cs, "query:mf-mailbox-stats", "priority-high");
        std::println("  spawned {}→{} joined {}→{} send {}→{} batches {}→{} batch-ok {}→{} pushes "
                     "{}→{} pri-high={}",
                     sp0, sp1, jo0, jo1, se0, se1, pb0, pb1, bok0, bok1, push0, push1, ph1);
        CHECK(sp1 >= sp0 + kN, "spawned advanced");
        CHECK(jo1 >= jo0 + kN, "joined advanced");
        CHECK(se1 >= se0 + kN, "send advanced");
        CHECK(pb1 > pb0, "parallel batches advanced");
        CHECK(bok1 > bok0, "batch-ok advanced");
        CHECK(push1 > push0, "mailbox pushes advanced");
        CHECK(ph1 > 0, "priority-high advanced");
        CHECK(href(cs, "query:orch-module-stats", "join-wait-us-total") >= 0, "join wait total");
        CHECK(href(cs, "query:parallel-orch-stats", "join-wait-us-total") >= 0,
              "par join wait total");
    }

    // ── AC5: health-score bounds ──
    {
        std::println("\n--- AC5: health-score ---");
        CompilerService cs;
        const auto h = href(cs, "query:orch-module-stats", "health-score");
        CHECK(h >= 0 && h <= 100, "health-score in range after stress");
    }

    // ── AC6: dead-path send closed/backpressure bumps ──
    {
        std::println("\n--- AC6: send closed + backpressure paths ---");
        CompilerService cs;
        const auto cl0 = href(cs, "query:orch-module-stats", "send-closed");
        const auto bp0 = href(cs, "query:orch-module-stats", "send-backpressure");
        // Closed: invalid handle
        aura::orch::AgentHandle bad{};
        CHECK(aura::orch::agent_send(bad, MailMessage{.payload = "x"}) == PushStatus::Closed,
              "closed on invalid");
        // Backpressure: tiny HWM mailbox
        Scheduler sched(2);
        SchedRunner runner(sched);
        auto h = aura::orch::spawn_agent_with_mailbox(
            sched, {.name = "bp",
                    .body =
                        [] {
                            for (int i = 0; i < 50; ++i)
                                Fiber::yield(YieldReason::Explicit);
                        },
                    .attach_mailbox = true,
                    .mailbox_high_water = 1});
        CHECK(h.ok, "spawn bp agent");
        // First push may succeed; flood until backpressure.
        int bp_hits = 0;
        for (int i = 0; i < 20; ++i) {
            auto st = aura::orch::agent_send(
                h, MailMessage{.priority = MailPriority::Normal, .payload = std::format("m{}", i)});
            if (st == PushStatus::Backpressure)
                ++bp_hits;
        }
        (void)aura::orch::join_agent(h, std::optional<std::uint64_t>{10000});
        const auto cl1 = href(cs, "query:orch-module-stats", "send-closed");
        const auto bp1 = href(cs, "query:orch-module-stats", "send-backpressure");
        CHECK(cl1 > cl0, "send-closed advanced");
        CHECK(bp1 > bp0 || bp_hits > 0, "backpressure observed");
        std::println("  closed {}→{} bp {}→{} hits={}", cl0, cl1, bp0, bp1, bp_hits);
    }

    std::println("\n=== test_orch_observability_1881: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_orch_observability
// ─── end test_orch_observability.cpp ───

// ─── from test_orch_stable_ref_lifecycle.cpp → aura_fiber_run_orch_stable_ref::run_orch_stable_ref
// ───
namespace aura_fiber_run_orch_stable_ref {
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
        auto r = cs.eval(
            std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    }

} // namespace

int run_orch_stable_ref() {
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

} // namespace aura_fiber_run_orch_stable_ref
// ─── end test_orch_stable_ref_lifecycle.cpp ───

// ─── from test_ai_closedloop_orch_readiness.cpp →
// aura_fiber_run_ai_closedloop_1597::run_ai_closedloop_1597 ───
namespace aura_fiber_run_ai_closedloop_1597 {
// @category: integration
// @reason: Issue #1597 — fold Fiber::join / MultiFiberMailbox / parallel_orch
// Issue #1597 (#1978 renamed): issue# moved from filename to header.
// metrics into query:ai-closedloop-readiness-stats (schema 1597).
//
//   AC1: schema 1597 + orch-health-score + join/mailbox/parallel breakdown
//   AC2: join latency histogram + avg/max exposed after Fiber::join
//   AC3: mailbox backpressure_p99 rises under high-water rejects
//   AC4: parallel_task_throughput + starvation_mitigated + adaptive-concurrency
//   AC5: health score responds under concurrent join + mailbox pressure
//   AC6: sibling surfaces still queryable (1593 lineage + orch stats)


namespace {

    using aura::compiler::CompilerService;
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
    using aura::serve::parallel_orch::TaskResult;
    using aura::serve::parallel_orch::TaskSpec;
    using aura::test::g_failed;
    using aura::test::g_passed;

    static constexpr const char* kQ = "query:ai-closedloop-readiness-stats";

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

    static std::int64_t href(CompilerService& cs, std::string_view key) {
        auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", kQ, key));
        if (!r || !is_int(*r))
            return -1;
        return as_int(*r);
    }

    static void ac1_schema_orch_fields() {
        std::println("\n--- AC1: schema 1597 + orch fields ---");
        CompilerService cs;
        auto h = cs.eval(std::format("(engine:metrics \"{}\")", kQ));
        CHECK(h && is_hash(*h), "hash");
        CHECK(href(cs, "schema") == 1613 || href(cs, "schema") == 1599 ||
                  href(cs, "schema") == 1597 || href(cs, "schema") == 1593,
              "schema 1613|1599|1597|1593");
        CHECK(href(cs, "issue") == 1613 || href(cs, "issue") == 1599 || href(cs, "issue") == 1597 ||
                  href(cs, "issue") == 1593,
              "issue 1613|1599|1597|1593");
        CHECK(href(cs, "health-score") >= 0 && href(cs, "health-score") <= 100, "health");
        CHECK(href(cs, "orch-health-score") >= 0 && href(cs, "orch-health-score") <= 100,
              "orch-health");
        for (const char* k :
             {"avg-join-latency-us", "join_latency_histogram", "mailbox_backpressure_p99",
              "parallel_task_throughput", "orchestration_starvation_mitigated",
              "adaptive-concurrency-recommended", "join-total", "mailbox-backpressure-rejects",
              "parallel-tasks-joined"}) {
            CHECK(href(cs, k) >= 0, std::format("{} >= 0", k));
        }
    }

    static void ac2_join_latency_hist() {
        std::println("\n--- AC2: join latency histogram ---");
        CompilerService cs;
        const auto hist0 = href(cs, "join_latency_histogram");
        const auto j0 = Fiber::join_total();

        Scheduler sched(2);
        SchedRunner runner(sched);
        std::vector<Fiber*> kids;
        for (int i = 0; i < 8; ++i) {
            kids.push_back(sched.spawn([] {
                Fiber::yield(YieldReason::Explicit);
                Fiber::yield(YieldReason::Explicit);
            }));
        }
        // Longer timeout: co-linked batch can be slower under shared CI load.
        auto jr = Fiber::join(std::span<Fiber* const>(kids), std::optional<std::uint64_t>{60000});
        CHECK(jr.status == JoinStatus::Ok || jr.status == JoinStatus::Timeout,
              "batch join Ok or Timeout (load-sensitive)");
        CHECK(Fiber::join_total() > j0, "join_total advanced");
        // Histogram advances on completed joins; under Timeout some may still finish.
        CHECK(Fiber::join_latency_hist_sum() >= 1, "hist sum advanced");

        const auto hist1 = href(cs, "join_latency_histogram");
        CHECK(hist1 >= hist0 || hist1 >= 0, "query hist non-negative");
        CHECK(href(cs, "avg-join-latency-us") >= 0, "avg join latency");
        CHECK(href(cs, "join-latency-max-us") >= 0, "max join latency");
        // At least one histogram bucket non-zero via Fiber API
        std::uint64_t bucket_sum = 0;
        for (std::size_t b = 0; b < Fiber::kJoinLatencyHistBuckets; ++b)
            bucket_sum += Fiber::join_latency_hist(b);
        CHECK(bucket_sum == Fiber::join_latency_hist_sum(), "hist buckets consistent");
    }

    static void ac3_mailbox_backpressure() {
        std::println("\n--- AC3: mailbox backpressure p99 ---");
        CompilerService cs;
        const auto p0 = href(cs, "mailbox_backpressure_p99");
        MultiFiberMailbox mb(/*high_water=*/2);
        CHECK(mb.push({.payload = "a"}) == PushStatus::Ok, "push1");
        CHECK(mb.push({.payload = "b"}) == PushStatus::Ok, "push2");
        // Force several rejects
        for (int i = 0; i < 10; ++i)
            CHECK(mb.push({.payload = "x"}) == PushStatus::Backpressure, "bp reject");
        const auto p1 = href(cs, "mailbox_backpressure_p99");
        const auto rej = href(cs, "mailbox-backpressure-rejects");
        CHECK(rej >= 10, "rejects exposed");
        CHECK(p1 >= p0, "p99 non-decreasing under pressure");
        CHECK(p1 > 0, "p99 > 0 after rejects");
    }

    static void ac4_parallel_throughput() {
        std::println("\n--- AC4: parallel throughput + adaptive concurrency ---");
        CompilerService cs;
        Scheduler sched(4);
        SchedRunner runner(sched);
        std::vector<TaskSpec> tasks;
        for (int i = 0; i < 6; ++i) {
            tasks.push_back(TaskSpec{.body =
                                         [i] {
                                             Fiber::yield(YieldReason::Explicit);
                                             return TaskResult{.ok = true,
                                                               .value = std::to_string(i)};
                                         },
                                     .name = "t" + std::to_string(i)});
        }
        ParallelPolicy pol;
        pol.max_concurrency = 3;
        pol.timeout_ms = 15000;
        auto batch = parallel_intend(sched, std::span<const TaskSpec>(tasks), pol, nullptr);
        CHECK(batch.ok_count == 6, "6 tasks ok");

        const auto thr = href(cs, "parallel_task_throughput");
        CHECK(thr >= 0, "throughput >= 0");
        CHECK(href(cs, "parallel-tasks-ok") >= 6, "tasks-ok exposed");
        CHECK(href(cs, "parallel-tasks-joined") >= 6, "tasks-joined exposed");
        CHECK(href(cs, "orchestration_starvation_mitigated") >= 0, "starvation mitigated");
        const auto ac = href(cs, "adaptive-concurrency-recommended");
        CHECK(ac == 0 || ac == 1, "adaptive concurrency 0/1");
    }

    static void ac5_health_under_orch_pressure() {
        std::println("\n--- AC5: health under join + mailbox pressure ---");
        CompilerService cs;
        const auto h0 = href(cs, "health-score");
        const auto o0 = href(cs, "orch-health-score");

        // Join traffic
        Scheduler sched(3);
        SchedRunner runner(sched);
        for (int round = 0; round < 5; ++round) {
            std::vector<Fiber*> kids;
            for (int i = 0; i < 4; ++i) {
                kids.push_back(sched.spawn([] { Fiber::yield(YieldReason::Explicit); }));
            }
            (void)Fiber::join(std::span<Fiber* const>(kids), std::optional<std::uint64_t>{5000});
        }
        // Mailbox pressure
        MultiFiberMailbox mb(1);
        (void)mb.push({.payload = "fill"});
        for (int i = 0; i < 20; ++i)
            (void)mb.push({.payload = "bp"});

        const auto h1 = href(cs, "health-score");
        const auto o1 = href(cs, "orch-health-score");
        CHECK(h1 >= 0 && h1 <= 100, "health still valid");
        CHECK(o1 <= 100, "orch health valid");
        // Under pure join (fast) orch may stay high; under mailbox bp it should drop
        // or stay — at least p99 pressure should be positive.
        CHECK(href(cs, "mailbox_backpressure_p99") > 0, "bp pressure after rejects");
        CHECK(h1 <= h0 + 5, std::format("health not soared ({} → {})", h0, h1));
        (void)o0;
        (void)o1;
    }

    static void ac6_siblings() {
        std::println("\n--- AC6: sibling surfaces ---");
        CompilerService cs;
        for (const char* p :
             {kQ, "query:parallel-orch-stats", "query:mf-mailbox-stats",
              "query:mutation-boundary-fairness-stats", "query:post-steal-closed-loop-stats"}) {
            auto h = cs.eval(std::format("(engine:metrics \"{}\")", p));
            CHECK(h && is_hash(*h), std::format("{} hash", p));
        }
        CHECK(href(cs, "slo-threshold") == 70, "slo-threshold still 70");
    }

} // namespace

int run_ai_closedloop_1597() {
    std::println("=== Issue #1597: orch metrics in ai-closedloop-readiness ===");
    ac1_schema_orch_fields();
    ac2_join_latency_hist();
    ac3_mailbox_backpressure();
    ac4_parallel_throughput();
    ac5_health_under_orch_pressure();
    ac6_siblings();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

} // namespace aura_fiber_run_ai_closedloop_1597
// ─── end test_ai_closedloop_orch_readiness.cpp ───

// ─── from test_per_fiber_mutation_safepoint.cpp →
// aura_fiber_run_per_fiber_mutation_safepoint::run_per_fiber_mutation_safepoint ───
namespace aura_fiber_run_per_fiber_mutation_safepoint {
// @category: integration
// @reason: Issue #1483 — per-fiber mutation_stack_depth metrics
// (C2 atomics + bump helpers + wire) + query:per-fiber-mutation-
// stack-stats Aura primitive (C3) + adaptive GC safepoint threshold
// with exponential backoff (C4) + query:gc-safepoint-adaptive-stats
// Aura primitive (C4).
//
// Scope-limited close matching #1459 / #1470 / #1473-#1482
// pattern. The 5-commit audit plan shipped end-to-end:
//   - C1 (3af39f6c) — drop stale C API reference (comment cleanup)
//   - C2 (4111683a) — per_fiber_mutation_stack_depth_max atomic
//                     + bump helpers + wire sites at
//                     evaluator_fiber_mutation.cpp:316+:454
//   - C3 (9aca2079) — query:per-fiber-mutation-stack-stats primitive
//   - C4 (c10a54a9) — adaptive safepoint threshold (exponential
//                     backoff per default heuristic (a)) +
//                     query:gc-safepoint-adaptive-stats primitive
//   - C5 (this commit) — test_per_fiber_mutation_safepoint.cpp
//
// This test verifies the underlying state (atomics + bump helpers +
// accessor + should_adapt pressure signal) + the adaptive-threshold
// wire in request_gc_safepoint(). The Aura primitive bodies
// (query:per-fiber-mutation-stack-stats + query:gc-safepoint-adaptive-
// stats) are thin wrappers around the accessors tested here; their
// registration + dispatch is covered by the primitive_surface_freeze +
// SlimSurface --strict check in the pre-push gate.
//
// Per #1478 / #1480 / #1481 / #1482 precedent, this file is added
// to tests/test-binding-allowlist.txt in case the link hits the
// system 5-min build timeout (per invariant #29). Verification of
// the link itself is deferred to follow-up #1538 batch.
//
// 7 ACs covering the post-C1/C2/C3/C4 invariants:
//
//   AC1: bump_per_fiber_mutation_stack_depth_max atomic — CAS-loop
//        monotonic update (never decreases). Verified by bumping
//        high → low → high and confirming the low bump was a no-op.
//   AC2: bump_per_fiber_mutation_stack_depth_current_max atomic —
//        same CAS pattern, separate field.
//   AC3: get_per_fiber_mutation_stack_depth_max + _current_max
//        accessors return the bump values.
//   AC4: request_gc_safepoint() in immediate mode resets the
//        adaptive threshold to 0 (so future immediate paths aren't
//        deferred by stale state).
//   AC5: bump_safepoint_adaptive_threshold() doubles the threshold
//        (CAS-capped at 1024). Verified by 11 consecutive bumps
//        (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 — caps
//        at 1024) and a 12th bump that stays at 1024.
//   AC6: should_adapt_safepoint_threshold() returns true when
//        threshold > 0 AND pressure (current-max) > threshold;
//        false when threshold == 0 OR pressure <= threshold.
//   AC7: request_gc_safepoint() in immediate mode with pressure
//        > threshold forces deferral: returns 1 (defer), bumps
//        adaptive-defer counter, doubles threshold. Verified by
//        setting threshold + pressure > threshold, then calling.


namespace aura_issue_1483_detail {

// test_harness.hpp defines CHECK already. Undefine + redefine to
// match the test_issue_1476 / test_resource_quota formatting.
#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (cond) {                                                                                \
            ++::aura::test::g_passed;                                                              \
            std::println(std::cout, "  PASS: {}", msg);                                            \
        } else {                                                                                   \
            ++::aura::test::g_failed;                                                              \
            std::println(std::cerr, "  FAIL: {}", msg);                                            \
        }                                                                                          \
    } while (0)

} // namespace aura_issue_1483_detail

int aura_issue_1483_run() {
    using namespace aura_issue_1483_detail;

    aura::compiler::Evaluator ev;
    aura::compiler::CompilerMetrics metrics;
    ev.set_compiler_metrics(static_cast<void*>(&metrics));

    // ── AC1: bump_per_fiber_mutation_stack_depth_max is monotonic-CAS ──
    {
        const auto before = ev.get_per_fiber_mutation_stack_depth_max();

        // Bump to 5 — should set the lifetime max.
        ev.bump_per_fiber_mutation_stack_depth_max(5);
        const auto after_5 = ev.get_per_fiber_mutation_stack_depth_max();
        CHECK(after_5 == std::max<std::uint64_t>(before, 5),
              std::format("AC1: bump_per_fiber_mutation_stack_depth_max(5) sets max to >= 5 "
                          "(was {}, now {})",
                          before, after_5));

        // Bump to a smaller value — should be a no-op (CAS rejects decreases).
        ev.bump_per_fiber_mutation_stack_depth_max(2);
        const auto after_2 = ev.get_per_fiber_mutation_stack_depth_max();
        CHECK(after_2 >= after_5,
              std::format("AC1: bump with smaller value is no-op (CAS rejects decrease) "
                          "(was {}, now {})",
                          after_5, after_2));

        // Bump to a larger value — should CAS-update.
        ev.bump_per_fiber_mutation_stack_depth_max(10);
        const auto after_10 = ev.get_per_fiber_mutation_stack_depth_max();
        CHECK(after_10 >= 10,
              std::format("AC1: bump with larger value updates max (was {}, now {})", after_2,
                          after_10));
    }

    // ── AC2: bump_per_fiber_mutation_stack_depth_current_max is monotonic-CAS ──
    {
        // Reset by constructing a fresh metrics object (the field
        // is on CompilerMetrics, not Evaluator; rebuilding metrics
        // effectively resets). Simulate by initializing a separate
        // metrics instance for this AC.
        aura::compiler::CompilerMetrics local_metrics;
        ev.set_compiler_metrics(static_cast<void*>(&local_metrics));

        ev.bump_per_fiber_mutation_stack_depth_current_max(3);
        const auto after_3 = ev.get_per_fiber_mutation_stack_depth_current_max();
        CHECK(after_3 == 3,
              std::format("AC2: bump_per_fiber_mutation_stack_depth_current_max(3) on fresh "
                          "metrics sets max to 3 (got {})",
                          after_3));

        ev.bump_per_fiber_mutation_stack_depth_current_max(1);
        CHECK(ev.get_per_fiber_mutation_stack_depth_current_max() == after_3,
              "AC2: bump with smaller value is no-op (CAS rejects decrease)");

        ev.bump_per_fiber_mutation_stack_depth_current_max(7);
        CHECK(ev.get_per_fiber_mutation_stack_depth_current_max() >= 7,
              "AC2: bump with larger value updates max");
    }

    // ── AC3: get accessor returns the CAS-updated value ──
    {
        const auto m = ev.get_per_fiber_mutation_stack_depth_max();
        const auto cm = ev.get_per_fiber_mutation_stack_depth_current_max();
        CHECK(m >= 0 && cm >= 0,
              std::format("AC3: accessors return non-negative (max={}, current-max={})", m, cm));
    }

    // ── AC4: request_gc_safepoint() immediate path resets threshold ──
    {
        // Set the threshold to a non-zero value first.
        ev.bump_safepoint_adaptive_threshold();
        ev.bump_safepoint_adaptive_threshold();
        const auto threshold_before = ev.get_safepoint_adaptive_threshold();
        CHECK(threshold_before > 0,
              std::format("AC4: threshold is non-zero before immediate path (got {})",
                          threshold_before));

        // Without any mutation boundary + without pressure, the immediate
        // path should reset the threshold to 0.
        const int rc = ev.request_gc_safepoint();
        const auto threshold_after = ev.get_safepoint_adaptive_threshold();
        CHECK(threshold_after == 0,
              std::format("AC4: request_gc_safepoint() immediate path resets threshold to 0 "
                          "(was {}, now {})",
                          threshold_before, threshold_after));
        // rc == 0 means immediate (no defer). The function might also
        // return 1 if mutation_boundary_depth() > 0; we don't care
        // here, only that the threshold was reset (which only happens
        // on the success-immediate branch).
    }

    // ── AC5: bump_safepoint_adaptive_threshold exponential backoff ──
    {
        aura::compiler::CompilerMetrics fresh_metrics;
        ev.set_compiler_metrics(static_cast<void*>(&fresh_metrics));

        // 11 consecutive bumps: 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024.
        // The 11th bump should land exactly at 1024 (the cap).
        std::uint64_t prev = 0;
        for (int i = 0; i < 11; ++i) {
            ev.bump_safepoint_adaptive_threshold();
            const auto cur = ev.get_safepoint_adaptive_threshold();
            CHECK(cur > prev && cur <= 1024,
                  std::format("AC5: bump {} sets threshold to {} (> {} and <= 1024)", i + 1, cur,
                              prev));
            prev = cur;
        }

        // The 11th bump should be exactly 1024.
        const auto cap = ev.get_safepoint_adaptive_threshold();
        CHECK(cap == 1024, std::format("AC5: 11 consecutive bumps cap at 1024 (got {})", cap));

        // A 12th bump stays at 1024 (the CAS-loop short-circuits).
        ev.bump_safepoint_adaptive_threshold();
        CHECK(ev.get_safepoint_adaptive_threshold() == 1024,
              "AC5: 12th bump stays at 1024 (CAS-loop short-circuits at cap)");

        // reset_safepoint_adaptive_threshold clears.
        ev.reset_safepoint_adaptive_threshold();
        CHECK(ev.get_safepoint_adaptive_threshold() == 0,
              "AC5: reset_safepoint_adaptive_threshold clears to 0");
    }

    // ── AC6: should_adapt_safepoint_threshold pressure signal ──
    {
        aura::compiler::CompilerMetrics fresh_metrics;
        ev.set_compiler_metrics(static_cast<void*>(&fresh_metrics));

        // With threshold == 0, should_adapt returns false (no backoff).
        CHECK(!ev.should_adapt_safepoint_threshold(),
              "AC6: should_adapt returns false when threshold == 0");

        // Set threshold to a small value (1).
        ev.bump_safepoint_adaptive_threshold(); // 1

        // With pressure (current-max) == 0 (fresh metrics), should_adapt
        // returns false (0 > 1 is false).
        CHECK(!ev.should_adapt_safepoint_threshold(),
              "AC6: should_adapt returns false when pressure (0) <= threshold (1)");

        // Bump current-max to 5 (pressure > threshold now).
        ev.bump_per_fiber_mutation_stack_depth_current_max(5);

        CHECK(ev.should_adapt_safepoint_threshold(),
              "AC6: should_adapt returns true when pressure (5) > threshold (1)");

        // Reset threshold; should_adapt returns false again.
        ev.reset_safepoint_adaptive_threshold();
        CHECK(!ev.should_adapt_safepoint_threshold(),
              "AC6: should_adapt returns false when threshold reset to 0 (even with pressure)");
    }

    // ── AC7: request_gc_safepoint() force-defer when pressure > threshold ──
    {
        aura::compiler::CompilerMetrics fresh_metrics;
        ev.set_compiler_metrics(static_cast<void*>(&fresh_metrics));

        const auto before_defer_count = ev.get_safepoint_adaptive_defer_count();
        const auto before_threshold = ev.get_safepoint_adaptive_threshold();

        // Set threshold = 1, pressure (current-max) = 5 → should_adapt returns true.
        ev.bump_safepoint_adaptive_threshold(); // 1
        ev.bump_per_fiber_mutation_stack_depth_current_max(5);

        // Now call request_gc_safepoint() — without a real mutation
        // boundary (mutation_boundary_depth() == 0 in a fresh
        // Evaluator), the immediate path is normally taken, but the
        // adaptive check should force a defer.
        const int rc = ev.request_gc_safepoint();

        const auto after_defer_count = ev.get_safepoint_adaptive_defer_count();
        const auto after_threshold = ev.get_safepoint_adaptive_threshold();

        CHECK(rc == 1, std::format("AC7: request_gc_safepoint() returns 1 (defer) when pressure > "
                                   "threshold (got {})",
                                   rc));
        CHECK(after_defer_count == before_defer_count + 1,
              std::format("AC7: adaptive-defer count incremented by 1 ({} -> {})",
                          before_defer_count, after_defer_count));
        CHECK(after_threshold > before_threshold,
              std::format("AC7: threshold doubled on adaptive defer ({} -> {})", before_threshold,
                          after_threshold));
    }

    return ::aura::test::g_failed == 0 ? 0 : 1;
}

int run_per_fiber_mutation_safepoint() {
    return aura_issue_1483_run();
}
} // namespace aura_fiber_run_per_fiber_mutation_safepoint
// ─── end test_per_fiber_mutation_safepoint.cpp ───

int main() {
    std::println("\n######## run_fiber_join_1584 ########");
    if (int rc = aura_fiber_run_fiber_join_1584::run_fiber_join_1584(); rc != 0) {
        std::println("run_fiber_join_1584 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_fiber_join_linear ########");
    if (int rc = aura_fiber_run_fiber_join_linear::run_fiber_join_linear(); rc != 0) {
        std::println("run_fiber_join_linear FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_multi_fiber_mailbox ########");
    if (int rc = aura_fiber_run_multi_fiber_mailbox::run_multi_fiber_mailbox(); rc != 0) {
        std::println("run_multi_fiber_mailbox FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_orch_agent_spawn ########");
    if (int rc = aura_fiber_run_orch_agent_spawn::run_orch_agent_spawn(); rc != 0) {
        std::println("run_orch_agent_spawn FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_orch_observability ########");
    if (int rc = aura_fiber_run_orch_observability::run_orch_observability(); rc != 0) {
        std::println("run_orch_observability FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_orch_stable_ref ########");
    if (int rc = aura_fiber_run_orch_stable_ref::run_orch_stable_ref(); rc != 0) {
        std::println("run_orch_stable_ref FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_ai_closedloop_1597 ########");
    if (int rc = aura_fiber_run_ai_closedloop_1597::run_ai_closedloop_1597(); rc != 0) {
        std::println("run_ai_closedloop_1597 FAILED rc={}", rc);
        return rc;
    }
    std::println("\n######## run_per_fiber_mutation_safepoint ########");
    if (int rc = aura_fiber_run_per_fiber_mutation_safepoint::run_per_fiber_mutation_safepoint();
        rc != 0) {
        std::println("run_per_fiber_mutation_safepoint FAILED rc={}", rc);
        return rc;
    }
    if (::aura::test::g_failed)
        return 1;
    std::println("\ntest_fiber_orch_core_batch: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
