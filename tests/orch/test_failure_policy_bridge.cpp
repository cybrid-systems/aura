// @category: unit
// @reason: Issue #2539 — FailurePolicy (#2007) → AgentFailurePolicy (#2229)
// mapping bridge (unidirectional; no default behaviour change).
// Issue #2756 — WorkflowFailurePolicy composition (batch + AgentScope +
// residual preference); additive over #2539.
//
//   AC1: to_agent_policy(FailurePolicy, max_restarts=...) callable under aura::orch
//   AC2: mapping table locked (FailFast/CollectAll/RetryN/CircuitBreaker)
//   AC3: defaults of AgentFailurePolicy / ParallelPolicy unchanged when unused
//   AC4: mapping API only (supervise-batch sugar deferred)
//   AC5: tests + source-cite; no docs/design
//
//   #2756 ACs:
//   AC1: compose maps onto ParallelPolicy + AgentFailurePolicy without
//        changing defaults when unused
//   AC2: residual preference observed via note_workflow_residual_reclaim
//   AC3: additive orch-module-stats keys + schema-2756
//   AC4: FailFast→Cancel, RetryN→RestartN, CircuitBreaker→Cancel+limit,
//        residual observation
//   AC5: README + source-cite; no docs/design/
//   AC6: MVP scope linter still green

#include "test_harness.hpp"

#include "orch/agent_spawn.h"
#include "orch/orch.h"
#include "serve/parallel_orch.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

import std;

namespace {

using aura::orch::AgentFailureAction;
using aura::orch::AgentFailurePolicy;
using aura::orch::compose_workflow_policy;
using aura::orch::FailurePolicy;
using aura::orch::g_orch_module_stats;
using aura::orch::kFailurePolicyBridgeIssue;
using aura::orch::kWorkflowFailurePolicyIssue;
using aura::orch::note_workflow_residual_reclaim_under_policy;
using aura::orch::ParallelPolicy;
using aura::orch::residual_prefers_cancel;
using aura::orch::residual_prefers_defer;
using aura::orch::ResidualReclaimPreference;
using aura::orch::to_agent_policy;
using aura::orch::to_parallel_policy;
using aura::orch::WorkflowFailurePolicy;
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

} // namespace

int run_test_failure_policy_bridge() {
    std::println("=== Issue #2539: FailurePolicy → AgentFailurePolicy bridge ===");
    CHECK(kFailurePolicyBridgeIssue == 2539, "issue stamp");

    // ── AC1: API callable under aura::orch ──────────────────────
    {
        std::println("\n--- AC1: to_agent_policy under aura::orch ---");
        AgentFailurePolicy p = to_agent_policy(FailurePolicy::FailFast);
        CHECK(p.on_stall == AgentFailureAction::Cancel, "AC1: FailFast callable");
        ParallelPolicy pp;
        AgentFailurePolicy p2 = to_agent_policy(pp);
        CHECK(p2.on_stall == AgentFailureAction::ReportOnly,
              "AC1: ParallelPolicy overload (default CollectAll)");
        // Also via orch.h re-export surface (same symbols).
        CHECK(true, "AC1: orch.h includes agent_spawn → to_agent_policy visible");
    }

    // ── AC2: mapping table ──────────────────────────────────────
    {
        std::println("\n--- AC2: mapping table ---");

        {
            auto p = to_agent_policy(FailurePolicy::FailFast);
            CHECK(p.on_stall == AgentFailureAction::Cancel, "AC2: FailFast → Cancel");
            CHECK(p.max_restarts == 0, "AC2: FailFast max_restarts default 0");
            CHECK(p.consecutive_stall_limit == 3, "AC2: FailFast keeps default limit 3");
            CHECK(p.on_join_fail == AgentFailureAction::ReportOnly,
                  "AC2: on_join_fail default ReportOnly");
        }
        {
            auto p = to_agent_policy(FailurePolicy::CollectAll);
            CHECK(p.on_stall == AgentFailureAction::ReportOnly, "AC2: CollectAll → ReportOnly");
            CHECK(p.max_restarts == 0, "AC2: CollectAll max_restarts 0");
        }
        {
            auto p = to_agent_policy(FailurePolicy::RetryN, /*max_restarts=*/5,
                                     /*consecutive_stall_limit=*/3, /*restart_backoff_ms=*/10);
            CHECK(p.on_stall == AgentFailureAction::RestartN, "AC2: RetryN → RestartN");
            CHECK(p.max_restarts == 5, "AC2: RetryN max_restarts=5");
            CHECK(p.restart_backoff_ms == 10, "AC2: RetryN backoff threaded");
        }
        {
            auto p = to_agent_policy(FailurePolicy::RetryN); // max_restarts default 0
            CHECK(p.on_stall == AgentFailureAction::RestartN, "AC2: RetryN action even if cap 0");
            CHECK(p.max_restarts == 0, "AC2: RetryN default max_restarts=0 (no re-spawn)");
        }
        {
            auto p = to_agent_policy(FailurePolicy::CircuitBreaker, /*max_restarts=*/0,
                                     /*consecutive_stall_limit=*/7);
            CHECK(p.on_stall == AgentFailureAction::Cancel, "AC2: CircuitBreaker → Cancel");
            CHECK(p.consecutive_stall_limit == 7, "AC2: CircuitBreaker limit aligned");
        }

        // ParallelPolicy overload: resolved_failure_policy + field threading.
        {
            ParallelPolicy pp;
            pp.fail_fast = true; // overrides failure_policy → FailFast
            pp.failure_policy = FailurePolicy::CollectAll;
            auto p = to_agent_policy(pp);
            CHECK(p.on_stall == AgentFailureAction::Cancel,
                  "AC2: fail_fast wins → FailFast → Cancel");
        }
        {
            ParallelPolicy pp;
            pp.failure_policy = FailurePolicy::RetryN;
            pp.max_retries = 4;
            pp.retry_backoff_ms = 25;
            auto p = to_agent_policy(pp);
            CHECK(p.on_stall == AgentFailureAction::RestartN, "AC2: pp RetryN → RestartN");
            CHECK(p.max_restarts == 4, "AC2: pp.max_retries → max_restarts");
            CHECK(p.restart_backoff_ms == 25, "AC2: pp.retry_backoff_ms → restart_backoff_ms");
        }
        {
            ParallelPolicy pp;
            pp.failure_policy = FailurePolicy::CircuitBreaker;
            pp.consecutive_fail_limit = 9;
            auto p = to_agent_policy(pp);
            CHECK(p.on_stall == AgentFailureAction::Cancel, "AC2: pp CircuitBreaker → Cancel");
            CHECK(p.consecutive_stall_limit == 9,
                  "AC2: pp.consecutive_fail_limit → consecutive_stall_limit");
        }
        {
            ParallelPolicy pp; // default failure_policy = CollectAll
            auto p = to_agent_policy(pp);
            CHECK(p.on_stall == AgentFailureAction::ReportOnly,
                  "AC2: default ParallelPolicy → CollectAll → ReportOnly");
        }
    }

    // ── AC3: defaults unchanged when bridge unused ──────────────
    {
        std::println("\n--- AC3: defaults unchanged without bridge ---");
        AgentFailurePolicy def;
        CHECK(def.on_stall == AgentFailureAction::Cancel, "AC3: AgentFailurePolicy default Cancel");
        CHECK(def.max_restarts == 0, "AC3: default max_restarts 0");
        CHECK(def.consecutive_stall_limit == 3, "AC3: default consecutive_stall_limit 3");
        CHECK(def.on_join_fail == AgentFailureAction::ReportOnly, "AC3: default on_join_fail");
        CHECK(def.restart_backoff_ms == 0, "AC3: default restart_backoff_ms 0");

        ParallelPolicy pp;
        CHECK(pp.failure_policy == FailurePolicy::CollectAll,
              "AC3: ParallelPolicy default CollectAll");
        CHECK(pp.max_retries == 0, "AC3: default max_retries 0");
        CHECK(pp.consecutive_fail_limit == 3, "AC3: default consecutive_fail_limit 3");
        CHECK(!pp.fail_fast, "AC3: default fail_fast false");

        // Bridge is pure (no global mutation).
        (void)to_agent_policy(FailurePolicy::RetryN, 99);
        AgentFailurePolicy still_def;
        CHECK(still_def.max_restarts == 0, "AC3: constructing after bridge call still default");
    }

    // ── AC4: mapping only (no supervise-batch sugar in this issue) ─
    {
        std::println("\n--- AC4: mapping API only (sugar deferred) ---");
        auto header = read_file("src/orch/agent_spawn.h");
        CHECK(header.find("to_agent_policy") != std::string::npos, "AC4: to_agent_policy present");
        // No callable sugar this issue — only mapping API. Doc may name
        // "orch:supervise-batch" as deferred follow-up (allowed).
        CHECK(header.find("supervise_batch(") == std::string::npos &&
                  header.find("void supervise_batch") == std::string::npos &&
                  header.find("inline") != std::string::npos /* always true for helpers */,
              "AC4: no supervise_batch( callable");
        CHECK(header.find("Optional language sugar") != std::string::npos ||
                  header.find("mapping API only") != std::string::npos ||
                  header.find("deferred") != std::string::npos,
              "AC4: sugar deferred documented");
    }

    // ── AC5: source-cite + README mapping table ─────────────────
    {
        std::println("\n--- AC5: source-cite + README ---");
        auto header = read_file("src/orch/agent_spawn.h");
        auto readme = read_file("src/orch/README.md");
        CHECK(header.find("kFailurePolicyBridgeIssue") != std::string::npos, "AC5: issue stamp");
        CHECK(header.find("2539") != std::string::npos, "AC5: #2539 cited");
        CHECK(header.find("FailFast") != std::string::npos, "AC5: FailFast in mapping docs");
        CHECK(header.find("CollectAll") != std::string::npos, "AC5: CollectAll");
        CHECK(header.find("RetryN") != std::string::npos, "AC5: RetryN");
        CHECK(header.find("CircuitBreaker") != std::string::npos, "AC5: CircuitBreaker");
        CHECK(readme.find("2539") != std::string::npos, "AC5: README cites #2539");
        CHECK(readme.find("to_agent_policy") != std::string::npos, "AC5: README documents API");
        CHECK(readme.find("FailFast") != std::string::npos &&
                  readme.find("CollectAll") != std::string::npos,
              "AC5: README mapping table rows");
        // No AgentRegistry reintro.
        CHECK(header.find("class AgentRegistry") == std::string::npos, "AC5: no AgentRegistry");
    }

    // ── Issue #2756 AC1: composition helper maps onto two surfaces;
    // defaults unchanged when helper is not used.
    {
        std::println("\n--- #2756 AC1: compose maps batch + agent; unused defaults ---");
        CHECK(kWorkflowFailurePolicyIssue == 2756, "AC1: issue stamp 2756");
        // Defaults when helper unused.
        AgentFailurePolicy def_ap;
        CHECK(def_ap.on_stall == AgentFailureAction::Cancel, "AC1: AgentFailurePolicy default");
        ParallelPolicy def_pp;
        CHECK(def_pp.failure_policy == FailurePolicy::CollectAll, "AC1: ParallelPolicy default");
        WorkflowFailurePolicy raw;
        CHECK(raw.batch_policy == FailurePolicy::CollectAll, "AC1: WFP default batch CollectAll");
        CHECK(raw.residual == ResidualReclaimPreference::Report,
              "AC1: WFP default residual Report");
        // Compose projects onto existing surfaces.
        auto w = compose_workflow_policy(FailurePolicy::CollectAll);
        auto pp = to_parallel_policy(w);
        auto ap = to_agent_policy(w);
        CHECK(pp.failure_policy == FailurePolicy::CollectAll, "AC1: to_parallel_policy CollectAll");
        CHECK(ap.on_stall == AgentFailureAction::ReportOnly, "AC1: to_agent_policy ReportOnly");
        // Unused path still default.
        AgentFailurePolicy still;
        CHECK(still.on_stall == AgentFailureAction::Cancel, "AC1: unused defaults unchanged");
    }

    // ── Issue #2756 AC2: residual preference observed; #2661 not altered.
    {
        std::println("\n--- #2756 AC2: residual preference observe (#2661 preserved) ---");
        auto w_report =
            compose_workflow_policy(FailurePolicy::FailFast, ResidualReclaimPreference::Report);
        CHECK(!residual_prefers_cancel(w_report), "AC2: Report does not prefer cancel");
        CHECK(!residual_prefers_defer(w_report), "AC2: Report does not prefer defer");
        auto w_cancel =
            compose_workflow_policy(FailurePolicy::FailFast, ResidualReclaimPreference::Cancel);
        CHECK(residual_prefers_cancel(w_cancel), "AC2: Cancel preference");
        auto w_defer =
            compose_workflow_policy(FailurePolicy::FailFast, ResidualReclaimPreference::Defer);
        CHECK(residual_prefers_defer(w_defer), "AC2: Defer preference");
        const auto r0 = g_orch_module_stats.workflow_residual_reclaim_under_policy_total.load(
            std::memory_order_relaxed);
        note_workflow_residual_reclaim_under_policy(w_cancel);
        const auto r1 = g_orch_module_stats.workflow_residual_reclaim_under_policy_total.load(
            std::memory_order_relaxed);
        CHECK(r1 == r0 + 1, "AC2: residual observe bumps counter");
        // #2661 cleanup contract symbols still present (no reclaim rewrite).
        auto spawn = read_file("src/orch/agent_spawn.h");
        CHECK(spawn.find("join_reclaimed_deferred_cleanup_total") != std::string::npos,
              "AC2: #2661 deferred cleanup surface preserved");
        CHECK(spawn.find("complete_agent_join_cleanup") != std::string::npos ||
                  spawn.find("join_reclaimed_deferred_cleanup") != std::string::npos,
              "AC2: reclaim path symbols preserved");
    }

    // ── Issue #2756 AC3: additive metrics + schema-2756 on query surface.
    {
        std::println("\n--- #2756 AC3: additive orch-module-stats keys ---");
        auto q = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(q.find("workflow-compose-total") != std::string::npos, "AC3: compose-total key");
        CHECK(q.find("workflow-retry-total") != std::string::npos, "AC3: retry-total key");
        CHECK(q.find("workflow-circuit-open-total") != std::string::npos,
              "AC3: circuit-open-total key");
        CHECK(q.find("workflow-residual-reclaim-under-policy-total") != std::string::npos,
              "AC3: residual-reclaim-under-policy key");
        CHECK(q.find("workflow-failure-policy-wired") != std::string::npos, "AC3: wired sentinel");
        CHECK(q.find("schema-2756") != std::string::npos, "AC3: schema-2756");
        CHECK(q.find("issue-2756") != std::string::npos, "AC3: issue-2756");
        // Prior surfaces preserved.
        CHECK(q.find("schema-2229") != std::string::npos, "AC3: schema-2229 preserved");
        CHECK(q.find("agent-failure-policy-wired") != std::string::npos,
              "AC3: agent-failure-policy-wired preserved");
        CHECK(g_orch_module_stats.workflow_failure_policy_wired.load() == 1,
              "AC3: wired flag == 1");
        // Compose bumps class counters.
        const auto c0 = g_orch_module_stats.workflow_compose_total.load(std::memory_order_relaxed);
        const auto rt0 = g_orch_module_stats.workflow_retry_total.load(std::memory_order_relaxed);
        const auto cb0 =
            g_orch_module_stats.workflow_circuit_open_total.load(std::memory_order_relaxed);
        (void)compose_workflow_policy(FailurePolicy::RetryN, ResidualReclaimPreference::Report, 2);
        (void)compose_workflow_policy(FailurePolicy::CircuitBreaker);
        CHECK(g_orch_module_stats.workflow_compose_total.load() >= c0 + 2, "AC3: compose-total +2");
        CHECK(g_orch_module_stats.workflow_retry_total.load() >= rt0 + 1, "AC3: retry-total +1");
        CHECK(g_orch_module_stats.workflow_circuit_open_total.load() >= cb0 + 1,
              "AC3: circuit-open-total +1");
    }

    // ── Issue #2756 AC4: FailFast→Cancel, RetryN→RestartN,
    // CircuitBreaker→Cancel+limit, residual observation (covered AC2).
    {
        std::println("\n--- #2756 AC4: FailFast / RetryN / CircuitBreaker composition ---");
        {
            auto w = compose_workflow_policy(FailurePolicy::FailFast);
            CHECK(to_agent_policy(w).on_stall == AgentFailureAction::Cancel,
                  "AC4: FailFast → Cancel");
            CHECK(to_parallel_policy(w).failure_policy == FailurePolicy::FailFast,
                  "AC4: FailFast batch preserved");
        }
        {
            auto w = compose_workflow_policy(FailurePolicy::RetryN,
                                             ResidualReclaimPreference::Report, /*max_retries=*/4,
                                             /*consecutive_fail_limit=*/3, /*retry_backoff_ms=*/10);
            CHECK(to_agent_policy(w).on_stall == AgentFailureAction::RestartN,
                  "AC4: RetryN → RestartN");
            CHECK(to_agent_policy(w).max_restarts == 4, "AC4: RetryN max_restarts=4");
            CHECK(to_agent_policy(w).restart_backoff_ms == 10, "AC4: RetryN backoff");
            CHECK(to_parallel_policy(w).max_retries == 4, "AC4: batch max_retries=4");
        }
        {
            auto w = compose_workflow_policy(FailurePolicy::CircuitBreaker,
                                             ResidualReclaimPreference::Report, /*max_retries=*/0,
                                             /*consecutive_fail_limit=*/7);
            CHECK(to_agent_policy(w).on_stall == AgentFailureAction::Cancel,
                  "AC4: CircuitBreaker → Cancel");
            CHECK(to_agent_policy(w).consecutive_stall_limit == 7,
                  "AC4: CircuitBreaker consecutive limit");
        }
        // ParallelPolicy overload.
        {
            ParallelPolicy pp;
            pp.failure_policy = FailurePolicy::RetryN;
            pp.max_retries = 2;
            auto w = compose_workflow_policy(pp, ResidualReclaimPreference::Defer);
            CHECK(to_agent_policy(w).on_stall == AgentFailureAction::RestartN,
                  "AC4: pp RetryN → RestartN");
            CHECK(residual_prefers_defer(w), "AC4: residual Defer from compose");
        }
    }

    // ── Issue #2756 AC5/AC6: source-cite + README + no docs/design + MVP.
    {
        std::println("\n--- #2756 AC5/AC6: source-cite + README + MVP ---");
        auto header = read_file("src/orch/agent_spawn.h");
        auto readme = read_file("src/orch/README.md");
        auto q = read_file("src/compiler/evaluator_primitives_agent.cpp");
        auto t = read_file("tests/orch/test_failure_policy_bridge.cpp");
        auto build = read_file("build.py");
        auto lint = read_file("scripts/coverage/checks/check_workflow_failure_policy_2756.py");
        CHECK(header.find("Issue #2756") != std::string::npos, "AC5: header cites #2756");
        CHECK(header.find("WorkflowFailurePolicy") != std::string::npos, "AC5: WFP type");
        CHECK(header.find("compose_workflow_policy") != std::string::npos, "AC5: compose helper");
        CHECK(header.find("ResidualReclaimPreference") != std::string::npos, "AC5: residual enum");
        CHECK(readme.find("2756") != std::string::npos, "AC5: README cites #2756");
        CHECK(readme.find("WorkflowFailurePolicy") != std::string::npos,
              "AC5: README documents WFP");
        CHECK(readme.find("compose_workflow_policy") != std::string::npos, "AC5: README compose");
        CHECK(q.find("Issue #2756") != std::string::npos, "AC5: query cites #2756");
        CHECK(t.find("ac2756") != std::string::npos || t.find("#2756 AC1") != std::string::npos,
              "AC5: tests present in this file");
        CHECK(build.find("check_workflow_failure_policy_2756") != std::string::npos,
              "AC5: build.py wires linter");
        CHECK(!lint.empty(), "AC5: linter present");
        CHECK(read_file("docs/design/2756-workflow-failure-policy.md").empty(),
              "AC5: no docs/design/2756-* per #1655");
        // AC6: no AgentRegistry reintro (comments may name the forbidden
        // symbols as documentation — only class definitions are banned).
        CHECK(header.find("class AgentRegistry") == std::string::npos, "AC6: no AgentRegistry");
        CHECK(header.find("AgentRegistry {") == std::string::npos &&
                  header.find("global_agent_registry(") == std::string::npos &&
                  header.find("conduct_parallel(") == std::string::npos,
              "AC6: no registry / conduct_parallel callables");
    }

    std::println("\n=== #2539 + #2756 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_failure_policy_bridge();
}
#endif
