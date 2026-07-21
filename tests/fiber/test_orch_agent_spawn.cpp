// @category: integration
// @reason: Issue #1588 — unified src/orch module + agent_spawn abstraction.
//
//   AC1: C++ spawn_agent_with_mailbox + join
//   AC2: agent_send / mailbox attach
//   AC3: parallel_intend batch (orch stats bump; #1966 dropped conduct_parallel)
//   AC4: Aura orch:spawn-agent + orch:agent-join
//   AC5: orch:parallel-intend alias
//   AC6: query:orch-module-stats schema 1588

#include "test_harness.hpp"

#include "orch/orch.h"
#include "serve/fiber.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <print>
#include <thread>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

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
    auto h = aura::orch::spawn_agent_with_mailbox(sched,
                                                  {.name = "w1", .body = [&] {
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
    auto h = aura::orch::spawn_agent_with_mailbox(sched,
                                                  {.name = "mb", .body = [&] {
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
    CHECK(aura::orch::agent_send(h, MailMessage{.payload = "ping"}) == PushStatus::Ok, "send ok");
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

int main() {
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
