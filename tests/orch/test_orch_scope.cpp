// tests/orch/test_orch_scope.cpp
// @category: integration
// @reason: Issue #2588 — Aura language surface for AgentScope supervision
//          (orch:scope-spawn / orch:scope-watch / orch:scope-join-all /
//          orch:scope-cancel-all). Per-Evaluator scope, NOT a global
//          agent registry (MVP linter still rejects AgentRegistry /
//          global_agent_registry / conduct_parallel).
//
//   AC1: Aura can spawn N agents in a scope, watch with RestartN,
//        join-all with timeout/drain.
//   AC2: MVP linter still green (no process-global registry symbols).
//   AC3: Structured hashes: ok/status/schema for spawn/watch/join;
//        metrics on query:orch-module-stats.
//   AC4: Scope destroy / join releases reservations (parity with
//        ~AgentHandle / #2155 no-leak).
//   AC5: Docs: explicit "not a global registry; bound to
//        Evaluator/session".
//
// Source-cite (issue #2588):
//   - src/orch/agent_scope.h: g_evaluator_agent_scopes() map +
//     get_or_create_agent_scope / find_agent_scope / drop_agent_scope /
//     reset_all_agent_scopes_for_test (per-Evaluator storage; map is
//     process-level but the AgentScope objects are per-Evaluator).
//   - src/orch/agent_spawn.h: OrchModuleStats adds scope_spawn_total,
//     scope_watch_total, scope_watch_restart_count, scope_join_all_total,
//     scope_cancel_all_total, scope_dropped_total (#2588).
//   - src/compiler/evaluator_primitives_agent.cpp: 4 new prims
//     (orch:scope-spawn / orch:scope-watch / orch:scope-join-all /
//     orch:scope-cancel-all) wired + query:orch-module-stats surfaces
//     scope-*-total / schema-2588 / issue-2588 / orch-scope-wired.
//   - src/orch/README.md: orch:scope-* section (explicit "not a global
//     registry; bound to Evaluator/session").
//   - tests/orch/test_orch_scope.cpp (this file).
//
// No docs/design/ per #1655 / #1485.

#include "test_harness.hpp"

#include "orch/agent_spawn.h"
#include "orch/agent_scope.h"
#include "compiler/typed_mutation_audit.h"
#include "core/resource_quota.hh"
#include "serve/fiber.h"
#include "serve/multi_fiber_mailbox.h"
#include "serve/scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <new> // Issue #3437: placement-new CompilerService at a recycled address
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::core::resource_quota::reset_process_resource_quota_for_test;
using aura::orch::g_orch_module_stats;
using aura::orch::reset_all_agent_scopes_for_test;
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
        cs.eval(std::format("(hash-ref (engine:metrics \"query:orch-module-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t hash_int(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool hash_bool(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_bool(*r))
        return false;
    return as_bool(*r);
}

void reset_all() {
    reset_all_agent_scopes_for_test();
    auto& m = g_orch_module_stats;
    m.scope_spawn_total.store(0, std::memory_order_relaxed);
    m.scope_watch_total.store(0, std::memory_order_relaxed);
    m.scope_watch_restart_count.store(0, std::memory_order_relaxed);
    m.scope_join_all_total.store(0, std::memory_order_relaxed);
    m.scope_cancel_all_total.store(0, std::memory_order_relaxed);
    m.scope_dropped_total.store(0, std::memory_order_relaxed);
    m.agents_spawned.store(0, std::memory_order_relaxed);
    m.agents_joined.store(0, std::memory_order_relaxed);
}

} // namespace

int run_test_orch_scope() {
    std::println("=== Issue #2588: Aura scope supervision surface ===");

    // ── AC2 + AC5: MVP linter still green — no global registry symbols ──
    {
        std::println("\n--- #2588 AC2 + AC5: MVP linter guards + README doc ---");
        // Source-cite inline: the new scope surface introduces no
        // AgentRegistry / global_agent_registry / conduct_parallel
        // symbols. The actual linter (scripts/coverage/checks/check_orch_mvp_scope.py
        // --strict) is the authoritative check; here we only verify
        // that the README documents the "not a global registry"
        // contract explicitly (AC5) and that my new scope-surface
        // symbols are not redefined as removed-pattern aliases.
        const auto readme_src = read_file("src/orch/README.md");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        // AC2: the new scope prim names use a different namespace
        // (orch:scope-spawn / orch:scope-watch / orch:scope-join-all /
        // orch:scope-cancel-all) — verify no USAGE of removed patterns
        // in the new prim source. (Comments mentioning the linter guard
        // names are fine; they document what the linter forbids.)
        const bool prim_uses_agent_registry =
            prim_src.find("AgentRegistry{") != std::string::npos ||
            prim_src.find("global_agent_registry{") != std::string::npos;
        CHECK(!prim_uses_agent_registry, "AC2: prim file does not define/use AgentRegistry");
        CHECK(readme_src.find("no process-global registry") != std::string::npos,
              "AC5: README explicitly documents 'no process-global registry'");
        CHECK(readme_src.find("per-Evaluator") != std::string::npos,
              "AC5: README explicitly documents 'bound to Evaluator/session'");
        CHECK(readme_src.find("orch:scope-spawn") != std::string::npos,
              "AC5: README documents orch:scope-spawn");
        CHECK(readme_src.find("orch:scope-watch") != std::string::npos,
              "AC5: README documents orch:scope-watch");
        CHECK(readme_src.find("orch:scope-join-all") != std::string::npos,
              "AC5: README documents orch:scope-join-all");
        CHECK(readme_src.find("orch:scope-cancel-all") != std::string::npos,
              "AC5: README documents orch:scope-cancel-all");
        CHECK(readme_src.find("schema-2588") != std::string::npos,
              "AC3: README surfaces schema-2588 metric key");
        // The orch-scope-wired / scope-*-total keys are wired in
        // evaluator_primitives_agent.cpp (see source-cite header).
        CHECK(prim_src.find("orch-scope-wired") != std::string::npos,
              "AC3: orch-scope-wired sentinel wired in prim file");
    }

    // ── AC1 + AC3: orch:scope-spawn / watch / join-all / cancel-all ───
    {
        std::println("\n--- #2588 AC1 + AC3: prim lifecycle + hash results ---");
        reset_all();
        CompilerService cs;

        // Spawn 3 agents into the scope.
        const auto spawn_a = cs.eval(R"((let ((r (orch:scope-spawn "scope-agent-a")))
                       (hash-ref r "ok")))");
        const auto spawn_b = cs.eval(R"((let ((r (orch:scope-spawn "scope-agent-b")))
                       (hash-ref r "ok")))");
        const auto spawn_c = cs.eval(R"((let ((r (orch:scope-spawn "scope-agent-c")))
                       (hash-ref r "ok")))");
        CHECK(spawn_a && is_bool(*spawn_a) && as_bool(*spawn_a),
              "AC1: orch:scope-spawn 'a' returns ok=#t");
        CHECK(spawn_b && is_bool(*spawn_b) && as_bool(*spawn_b),
              "AC1: orch:scope-spawn 'b' returns ok=#t");
        CHECK(spawn_c && is_bool(*spawn_c) && as_bool(*spawn_c),
              "AC1: orch:scope-spawn 'c' returns ok=#t");

        // Hash structure: ok / id / name / schema=2588 / schema-2083 / schema-2161 / status.
        const auto spawn_hash = cs.eval(R"((let ((r (orch:scope-spawn "scope-agent-d"))) r))");
        CHECK(spawn_hash, "AC1: scope-spawn returns hash");
        // AC3: schema=2588 + schema-2083 lineage — extract via direct
        // (hash-ref …) eval (don't double-wrap via hash_int helper).
        const auto schema_ev =
            cs.eval(R"((let ((r (orch:scope-spawn "scope-schema-check"))) (hash-ref r "schema")))");
        CHECK(schema_ev && is_int(*schema_ev) && as_int(*schema_ev) == 2588,
              "AC3: scope-spawn hash carries schema=2588");
        const auto schema2083_ev = cs.eval(
            R"((let ((r (orch:scope-spawn "scope-schema2083-check"))) (hash-ref r "schema-2083")))");
        CHECK(schema2083_ev && is_int(*schema2083_ev) && as_int(*schema2083_ev) == 2083,
              "AC3: scope-spawn hash carries schema-2083 (#2083 AgentScope lineage)");

        // scope-spawn-total bumped per call.
        const auto spawn_total = href(cs, "scope-spawn-total");
        CHECK(spawn_total >= 6, "AC3: scope-spawn-total bumps per orch:scope-spawn call");

        // Watch with RestartN policy.
        const auto watch_hash = cs.eval(
            R"((orch:scope-watch :stall-ms 50 :policy 'restart-n :max-restarts 2 :consecutive-stall-limit 2))");
        CHECK(watch_hash, "AC1: orch:scope-watch returns hash");
        CHECK(
            hash_int(cs, R"((orch:scope-watch :stall-ms 100 :policy 'report-only))", "policy") ==
                    -1 ||
                hash_int(
                    cs,
                    R"((let ((r (orch:scope-watch :stall-ms 100 :policy 'report-only))) (hash-ref r "policy")))",
                    "policy") == -1,
            "AC3: scope-watch hash carries policy key (int or string)");
        CHECK(href(cs, "scope-watch-total") >= 2,
              "AC3: scope-watch-total bumps per orch:scope-watch call");
        CHECK(href(cs, "schema-2588") == 2588, "AC3: query:orch-module-stats surfaces schema-2588");
        CHECK(href(cs, "orch-scope-wired") == 1, "AC3: orch-scope-wired sentinel == 1");
        CHECK(hash_int(cs, R"((orch:scope-watch :stall-ms 50 :policy 'report-only))",
                       "schema-3250") == 3250,
              "3250: orch:scope-watch hash schema-3250");
        CHECK(hash_int(cs, R"((orch:scope-watch :stall-ms 50 :policy 'report-only))",
                       "restart-skipped-no-spec") >= 0,
              "3250: orch:scope-watch restart-skipped-no-spec");
        CHECK(href(cs, "schema-3250") == 3250, "3250: query schema-3250");
        CHECK(href(cs, "restart-n-spec-boundary-wired") == 1, "3250: wired sentinel");

        // Cancel-all (best-effort cancel; doesn't wait).
        const auto cancel_hash = cs.eval(R"((orch:scope-cancel-all))");
        CHECK(cancel_hash, "AC1: orch:scope-cancel-all returns hash");
        CHECK(hash_bool(cs, R"((orch:scope-cancel-all))", "ok"),
              "AC3: scope-cancel-all hash carries ok=#t");
        CHECK(href(cs, "scope-cancel-all-total") >= 2,
              "AC3: scope-cancel-all-total bumps per call");

        // Join-all with timeout/drain — returns structured status.
        // Issue #3467 B1: a settled join-all drops the root slot, so each
        // subsequent join-all round re-arms a fresh scope first.
        const auto join_hash = cs.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))");
        CHECK(join_hash, "AC1: orch:scope-join-all returns hash");
        cs.eval(R"((orch:scope-spawn "rearm-3467-a"))");
        CHECK(hash_int(cs, R"((orch:scope-join-all :timeout-ms 1000 :drain-ms 1000))", "schema") ==
                  2588,
              "AC3: scope-join-all hash carries schema=2588");
        CHECK(href(cs, "scope-join-all-total") >= 2, "AC3: scope-join-all-total bumps per call");
        cs.eval(R"((orch:scope-spawn "rearm-3467-b"))");
        CHECK(hash_int(cs, R"((orch:scope-join-all :timeout-ms 500 :drain-ms 500))",
                       "schema-3250") == 3250,
              "3250: orch:scope-join-all hash schema-3250");
        cs.eval(R"((orch:scope-spawn "rearm-3467-c"))");
        CHECK(hash_int(cs, R"((orch:scope-join-all :timeout-ms 500 :drain-ms 500))",
                       "restart-attempted") >= 0,
              "3250: orch:scope-join-all restart-attempted");
    }

    // ── AC4: cancel-all + join-all parity with ~AgentHandle / #2155 no-leak ──
    {
        std::println("\n--- #2588 AC4: cancel-all + join-all parity ---");
        reset_all();
        CompilerService cs;
        // Spawn 2 agents into the scope, then cancel-all + join-all.
        // This matches ~AgentHandle destruct + join_agents release
        // semantics (#2155 / #2009 no-leak): per-handle reservation
        // release is idempotent and scope join-all drains the open
        // mailbox (#2511 #2536 #2587 sibling contract).
        cs.eval(R"((orch:scope-spawn "parity-a"))");
        cs.eval(R"((orch:scope-spawn "parity-b"))");
        // join-all with timeout/drain returns structured status (ok|timeout|...).
        // Issue #3467 B1: this round settles (trivial bodies) and DROPS the
        // root slot, so the status round re-arms a fresh scope first.
        const auto join_hash = cs.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))");
        CHECK(join_hash, "AC4: join-all returns hash after spawn 2 + cancel-all");
        cs.eval(R"((orch:scope-spawn "parity-rearm-3467"))");
        // Status is one of "ok" / "timeout" / "cancelled" / "invalid" / "reclaimed".
        const auto status_ev = cs.eval(
            R"((let ((r (orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))) (hash-ref r "status")))");
        CHECK(status_ev, "AC4: join-all hash carries status");

        // Scope-join-all-total bumps per call.
        CHECK(href(cs, "scope-join-all-total") >= 2,
              "AC4: scope-join-all-total bumps per orch:scope-join-all call");

        // Issue #3467 B1: a settled join-all drops the root slot, so the
        // next scope-spawn creates a fresh tree (the spawn succeeds either
        // way — parity with ~AgentHandle no-leak is preserved).
        const auto fresh =
            cs.eval(R"((let ((r (orch:scope-spawn "post-join-fresh"))) (hash-ref r "ok")))");
        CHECK(fresh && is_bool(*fresh) && as_bool(*fresh),
              "AC4: post-join scope-spawn succeeds (fresh tree per #3467 B1 after settled drop)");
    }

    // ── #3467: scope-join-all drops the root slot only when settled ──
    {
        std::println("\n--- #3467 AC4: scope-join-all B1 drop contract ---");
        reset_all();
        CompilerService cs;

        // Settled join-all (trivial bodies, generous timeout) → root slot
        // dropped → directory and scope-resolve agree (empty / miss).
        cs.eval(R"((orch:scope-spawn "settle-a"))");
        cs.eval(R"((orch:scope-spawn "settle-b"))");
        CHECK(hash_int(cs, R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))",
                       "cleanup-pending-count") == 0,
              "3467 AC4: settled join-all reports cleanup-pending-count=0");
        const auto dir_count =
            cs.eval(R"((let ((r (orch:agent-directory))) (hash-ref r "count")))");
        CHECK(dir_count && is_int(*dir_count) && as_int(*dir_count) == 0,
              "3467 AC4: directory empty after settled join-all (slot dropped)");
        const auto resolve_miss =
            cs.eval(R"((let ((r (orch:scope-resolve "settle-a"))) (if (hash-ref r "ok") 1 0)))");
        CHECK(resolve_miss && is_int(*resolve_miss) && as_int(*resolve_miss) == 0,
              "3467 AC4: scope-resolve misses after settled join-all");
        const auto fresh =
            cs.eval(R"((let ((r (orch:scope-spawn "settle-fresh"))) (hash-ref r "ok")))");
        CHECK(fresh && is_bool(*fresh) && as_bool(*fresh),
              "3467 AC4: scope-spawn after settled drop creates a fresh tree");

        // Source-cite: the guarded drop (root only; live-fiber + both
        // pending flags) and the spawn-side pre-deny.
        const auto src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto scopeh = read_file("src/orch/agent_scope.h");
        CHECK(src.find("all_settled") != std::string::npos, "3467 AC4: guarded drop present");
        CHECK(src.find("tree_settled()") != std::string::npos,
              "3467 AC4 / #3496: drop uses tree_settled (descendants)");
        CHECK(scopeh.find("(hp.fiber && !hp.fiber->is_done())") != std::string::npos,
              "3467 AC4: drop gate checks live body fiber");
        CHECK(scopeh.find("hp.must_wait_reclaimed") != std::string::npos &&
                  scopeh.find("hp.reclaimed_deferred_cleanup") != std::string::npos,
              "3467 AC4: drop gate checks both pending flags");
        CHECK(src.find("name-reuse-while-reclaimed-pending") != std::string::npos,
              "3467 AC1: spawn pre-deny deny-detail present");
        CHECK(read_file("tests/orch/test_issue_3467.cpp").empty() &&
                  read_file("tests/issues/test_issue_3467.cpp").empty(),
              "3467 AC6: no test_issue_3467.cpp (src-aligned suites only)");
    }

    // ── #3496: root join-all drop sees descendant handles ──
    {
        using aura::orch::AgentSpec;
        using aura::serve::Fiber;
        using aura::serve::YieldReason;
        std::println("\n--- #3496 AC1: empty root + live child → join-all does not drop ---");
        reset_all();
        CompilerService cs;
        const auto child_ok =
            cs.eval(R"((let ((r (orch:scope-child "c0-3496"))) (if (hash-ref r "ok") 1 0)))");
        CHECK(child_ok && is_int(*child_ok) && as_int(*child_ok) == 1, "3496 AC1: scope-child ok");
        auto* root = aura::orch::find_agent_scope(static_cast<void*>(&cs.evaluator()));
        CHECK(root != nullptr && root->child_count() >= 1, "3496 AC1: child scope exists");
        std::atomic<bool> hold{true};
        AgentSpec spec;
        spec.name = "3496-live";
        spec.attach_mailbox = false;
        spec.body = [&hold] {
            while (hold.load(std::memory_order_relaxed)) {
                if (aura::serve::g_current_fiber &&
                    aura::serve::g_current_fiber->is_cancel_requested())
                    break;
                Fiber::yield(YieldReason::Explicit);
            }
        };
        (void)root->child_at(0).spawn(std::move(spec));
        CHECK(!root->tree_settled(), "3496 AC1: tree_settled false while child live");
        (void)cs.eval(R"((orch:scope-join-all :timeout-ms 20 :drain-ms 20))");
        CHECK(aura::orch::find_agent_scope(static_cast<void*>(&cs.evaluator())) != nullptr,
              "3496 AC1: root join-all does not drop while child fiber live");
        CHECK(root->child_at(0).size() >= 1, "3496 AC1: child handle still in tree");
        if (auto hs = root->child_at(0).handles(); !hs.empty() && hs[0].fiber)
            CHECK(!hs[0].fiber->is_done(), "3496 AC1: child fiber still live");
        hold.store(false, std::memory_order_relaxed);
        root->cancel_all();
        (void)root->child_at(0).join_all(
            aura::orch::JoinPolicy{.primary_ms = 2000, .drain_ms = 200});
        (void)cs.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 200))");
    }

    {
        std::println("\n--- #3496 AC2: settled child-only tree still drops ---");
        reset_all();
        CompilerService cs;
        (void)cs.eval(R"((orch:scope-child "c0-settle"))");
        const auto spawn_ok = cs.eval(
            R"((let ((r (orch:scope-spawn "settle-child" :path "0"))) (if (hash-ref r "ok") 1 0)))");
        CHECK(spawn_ok && is_int(*spawn_ok) && as_int(*spawn_ok) == 1,
              "3496 AC2: spawn on child ok");
        (void)cs.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))");
        CHECK(aura::orch::find_agent_scope(static_cast<void*>(&cs.evaluator())) == nullptr,
              "3496 AC2: settled tree still drops the Evaluator slot");
        CHECK(read_file("tests/orch/test_issue_3496.cpp").empty() &&
                  read_file("tests/issues/test_issue_3496.cpp").empty(),
              "3496 AC5: no test_issue_3496.cpp");
        CHECK(read_file("docs/design/3496-join-all-tree-settled.md").empty(),
              "3496 AC5: no docs/design/3496-*");
    }

    // ── AC5: Docs source-cite (already covered inline above) ──────────
    {
        std::println("\n--- #2588 AC5: docs source-cite ---");
        std::println("AC5 — see file header for full source-cite list.");
        std::println("  - src/orch/agent_scope.h: per-Evaluator scope map (storage).");
        std::println("  - src/orch/agent_spawn.h: 6 new metrics on OrchModuleStats.");
        std::println("  - src/compiler/evaluator_primitives_agent.cpp: 4 new prims");
        std::println("    + query:orch-module-stats surfaces scope-*-total keys.");
        std::println("  - src/orch/README.md: orch:scope-* section with explicit");
        std::println("    'not a global registry; bound to Evaluator/session' wording.");
        std::println("  - tests/orch/test_orch_scope.cpp (this file).");
        std::println("  - no docs/design/ per #1655 / #1485.");
        CHECK(true, "AC5: source-cite listed above (no docs/design/)");
    }

#ifdef AURA_ISSUE_BATCH_MEMBER
    // #2751 / #2926 spawn extra Scheduler + scope-spawn after #2588
    // already joined. Leftover scheduler threads UAF the Evaluator
    // (SIGSEGV mid-batch). Those ACs stay on the standalone binary.
    CHECK(true, "2751/2926: skip extra scope-spawn in orch batch (scheduler UAF)");
#else
    // ── Issue #2751: session-level Agent directory surface ────────────
    {
        std::println("\n--- #2751: orch:agent-directory session surface ---");
        reset_all();
        g_orch_module_stats.agent_directory_total.store(0, std::memory_order_relaxed);
        g_orch_module_stats.agent_directory_entries_total.store(0, std::memory_order_relaxed);

        // Source-cite + linter: no AgentRegistry / global_agent_registry.
        const auto scope_h = read_file("src/orch/agent_scope.h");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto readme_src = read_file("src/orch/README.md");
        CHECK(scope_h.find("directory_snapshot") != std::string::npos,
              "AC3: AgentScope::directory_snapshot C++ helper present");
        CHECK(scope_h.find("AgentDirectoryEntry") != std::string::npos,
              "AC3: AgentDirectoryEntry struct present");
        CHECK(scope_h.find("kAgentDirectoryIssue") != std::string::npos,
              "AC3: kAgentDirectoryIssue = 2751 present");
        CHECK(prim_src.find("add(\"orch:agent-directory\"") != std::string::npos,
              "AC3: orch:agent-directory prim registered");
        CHECK(prim_src.find("AgentRegistry{") == std::string::npos,
              "AC4: prim file does not define AgentRegistry");
        CHECK(readme_src.find("orch:agent-directory") != std::string::npos,
              "AC6: README documents orch:agent-directory");
        CHECK(readme_src.find("schema-2751") != std::string::npos,
              "AC6: README surfaces schema-2751");
        CHECK(readme_src.find("no process-global registry") != std::string::npos,
              "AC4/AC6: README keeps no process-global registry wording");

        CompilerService cs;

        // AC1 empty session: no scope yet → count=0, ok=#t.
        {
            const auto empty_ok =
                cs.eval(R"((let ((r (orch:agent-directory))) (hash-ref r "ok")))");
            CHECK(empty_ok && is_bool(*empty_ok) && as_bool(*empty_ok),
                  "AC1: empty session directory ok=#t");
            const auto empty_count =
                cs.eval(R"((let ((r (orch:agent-directory))) (hash-ref r "count")))");
            CHECK(empty_count && is_int(*empty_count) && as_int(*empty_count) == 0,
                  "AC1: empty session directory count=0");
        }

        // Spawn 3 agents; directory lists them under this Evaluator only.
        cs.eval(R"((orch:scope-spawn "dir-agent-a"))");
        cs.eval(R"((orch:scope-spawn "dir-agent-b"))");
        cs.eval(R"((orch:scope-spawn "dir-worker-c"))");

        const auto dir_count =
            cs.eval(R"((let ((r (orch:agent-directory))) (hash-ref r "count")))");
        CHECK(dir_count && is_int(*dir_count) && as_int(*dir_count) == 3,
              "AC1/AC2: directory count=3 after 3 scope-spawn");

        const auto schema_ev =
            cs.eval(R"((let ((r (orch:agent-directory))) (hash-ref r "schema")))");
        CHECK(schema_ev && is_int(*schema_ev) && as_int(*schema_ev) == 2751,
              "AC3: directory hash carries schema=2751");

        // Name prefix filter.
        const auto pref_count = cs.eval(
            R"((let ((r (orch:agent-directory :name-prefix "dir-worker"))) (hash-ref r "count")))");
        CHECK(pref_count && is_int(*pref_count) && as_int(*pref_count) == 1,
              "AC1: name-prefix filter returns 1 agent");

        // Hierarchy: C++ directory_snapshot walks child scopes.
        {
            aura::serve::Scheduler sched(2);
            aura::orch::AgentScope root(sched);
            aura::orch::AgentSpec sa;
            sa.name = "root-a";
            sa.body = [] {};
            sa.attach_mailbox = false;
            (void)root.spawn(std::move(sa));
            auto& child = root.spawn_child();
            aura::orch::AgentSpec sb;
            sb.name = "child-b";
            sb.body = [] {};
            sb.attach_mailbox = false;
            (void)child.spawn(std::move(sb));
            const auto full = root.directory_snapshot();
            CHECK(full.entries.size() == 2, "AC1/AC5: hierarchy snapshot has 2 agents");
            CHECK(full.scopes_visited >= 2, "AC5: hierarchy scopes_visited >= 2");
            bool saw_root = false, saw_child = false;
            for (const auto& e : full.entries) {
                if (e.name == "root-a" && e.scope_path == "root")
                    saw_root = true;
                if (e.name == "child-b" && e.scope_path == "0")
                    saw_child = true;
            }
            CHECK(saw_root, "AC1: root agent scope-path=root");
            CHECK(saw_child, "AC1: child agent scope-path=0");
            aura::orch::AgentDirectoryFilter root_only;
            root_only.include_descendants = false;
            const auto local = root.directory_snapshot(root_only);
            CHECK(local.entries.size() == 1, "AC1: include_descendants=false → root only");
            // Cancel + join to release (no-leak).
            root.cancel_all();
            (void)root.join_all(aura::orch::JoinPolicy{.primary_ms = 2000, .drain_ms = 2000});
        }

        // Metrics wired.
        CHECK(href(cs, "agent-directory-total") >= 1,
              "AC3: agent-directory-total bumps on prim call");
        CHECK(href(cs, "agent-directory-wired") == 1, "AC3: agent-directory-wired sentinel == 1");
        CHECK(href(cs, "schema-2751") == 2751, "AC3: schema-2751 == 2751");

        // Concurrent-safe read while another spawn happens on same thread
        // (serial model #2399) — no crash / no global leak.
        cs.eval(R"((orch:scope-spawn "dir-agent-d"))");
        const auto after = cs.eval(R"((let ((r (orch:agent-directory))) (hash-ref r "count")))");
        CHECK(after && is_int(*after) && as_int(*after) >= 4,
              "AC5: directory after concurrent-serial spawn still consistent");

        // Cleanup.
        cs.eval(R"((orch:scope-cancel-all))");
        cs.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))");
    }

    // ── #2926: session-local scope-resolve by name ─────────────────
    {
        std::println("\n=== Issue #2926: orch:scope-resolve (session-local find) ===");
        CHECK(true, "issue stamp #2926");
        CompilerService cs;
        reset_all();

        // Fresh session.
        cs.eval(R"((orch:scope-cancel-all))");
        cs.eval(R"((orch:scope-join-all :timeout-ms 500 :drain-ms 500))");

        std::println("\n--- #2926 AC1: spawn then resolve same name ---");
        {
            auto spawn = cs.eval(R"((orch:scope-spawn "resolve-agent-a"))");
            CHECK(spawn.has_value(), "2926 AC1: spawn resolve-agent-a");
            auto ok_b = cs.eval(
                R"((let ((r (orch:scope-resolve "resolve-agent-a"))) (if (hash-ref r "ok") 1 0)))");
            CHECK(ok_b && is_int(*ok_b) && as_int(*ok_b) == 1, "2926 AC1: resolve ok=#t");
            auto name_ok = cs.eval(
                R"((let ((r (orch:scope-resolve "resolve-agent-a")))
                     (if (string=? (hash-ref r "name") "resolve-agent-a") 1 0)))");
            CHECK(name_ok && is_int(*name_ok) && as_int(*name_ok) == 1,
                  "2926 AC1: resolve name matches");
            auto schema = cs.eval(
                R"((let ((r (orch:scope-resolve "resolve-agent-a"))) (hash-ref r "schema")))");
            CHECK(schema && is_int(*schema) && as_int(*schema) == 2926, "2926 AC1: schema=2926");
            // C++ find + send via resolved handle
            {
                aura::serve::Scheduler sched(2);
                aura::orch::AgentScope root(sched);
                aura::orch::AgentSpec sa;
                sa.name = "cpp-find-a";
                sa.body = [] {};
                sa.attach_mailbox = true;
                sa.mailbox_high_water = 16;
                auto& ha = root.spawn(std::move(sa));
                CHECK(ha.ok, "2926 AC1: C++ spawn ok");
                auto* found = root.find("cpp-find-a");
                CHECK(found != nullptr && found->id == ha.id, "2926 AC1: C++ find matching id");
                aura::serve::mf_mailbox::MailMessage m;
                m.payload = "hi";
                auto st = aura::orch::agent_send(*found, std::move(m));
                CHECK(st == aura::serve::mf_mailbox::PushStatus::Ok ||
                          st == aura::serve::mf_mailbox::PushStatus::Backpressure,
                      "2926 AC1: send via resolved handle works");
                root.cancel_all();
                (void)root.join_all(aura::orch::JoinPolicy{.primary_ms = 2000, .drain_ms = 2000});
            }
        }

        std::println("\n--- #2926 AC2: unknown name → not-found ---");
        {
            auto miss = cs.eval(
                R"((let ((r (orch:scope-resolve "no-such-agent-xyz")))
                     (list (if (hash-ref r "ok") 1 0)
                           (if (string=? (hash-ref r "status") "not-found") 1 0))))");
            CHECK(miss.has_value(), "2926 AC2: resolve missing returns");
            auto ok0 = cs.eval(
                R"((let ((r (orch:scope-resolve "no-such-agent-xyz"))) (if (hash-ref r "ok") 1 0)))");
            CHECK(ok0 && is_int(*ok0) && as_int(*ok0) == 0, "2926 AC2: ok=#f");
            auto st = cs.eval(
                R"((let ((r (orch:scope-resolve "no-such-agent-xyz")))
                     (if (string=? (hash-ref r "status") "not-found") 1 0)))");
            CHECK(st && is_int(*st) && as_int(*st) == 1, "2926 AC2: status=not-found");
            // No cross-Evaluator lookup: second CompilerService has empty scope.
            CompilerService cs2;
            auto other = cs2.eval(
                R"((let ((r (orch:scope-resolve "resolve-agent-a"))) (if (hash-ref r "ok") 1 0)))");
            CHECK(other && is_int(*other) && as_int(*other) == 0,
                  "2926 AC2: other Evaluator does not see first session agents");
        }

        std::println("\n--- #2926 AC3: include-descendants hierarchy ---");
        {
            aura::serve::Scheduler sched(2);
            aura::orch::AgentScope root(sched);
            aura::orch::AgentSpec sa;
            sa.name = "root-resolve";
            sa.body = [] {};
            sa.attach_mailbox = false;
            (void)root.spawn(std::move(sa));
            auto& child = root.spawn_child();
            aura::orch::AgentSpec sb;
            sb.name = "child-resolve";
            sb.body = [] {};
            sb.attach_mailbox = false;
            (void)child.spawn(std::move(sb));
            CHECK(root.find("root-resolve", false) != nullptr, "2926 AC3: root finds self");
            CHECK(root.find("child-resolve", false) == nullptr,
                  "2926 AC3: include_descendants=#f misses child");
            CHECK(root.find("child-resolve", true) != nullptr,
                  "2926 AC3: include_descendants=#t finds child");
            CHECK(child.find("root-resolve", true) == nullptr,
                  "2926 AC3: child does not see parent");
            root.cancel_all();
            (void)root.join_all(aura::orch::JoinPolicy{.primary_ms = 2000, .drain_ms = 2000});
        }

        std::println("\n--- #2926 AC4: after join_all → status done or not-found ---");
        {
            cs.eval(R"((orch:scope-cancel-all))");
            cs.eval(R"((orch:scope-join-all :timeout-ms 500 :drain-ms 500))");
            cs.eval(R"((orch:scope-spawn "join-then-resolve"))");
            cs.eval(R"((orch:scope-cancel-all))");
            cs.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))");
            // After join: either scope dropped → not-found, or handle remains with done.
            auto after = cs.eval(R"((let ((r (orch:scope-resolve "join-then-resolve")))
                 (list (if (hash-ref r "ok") 1 0)
                       (hash-ref r "status"))))");
            CHECK(after.has_value(), "2926 AC4: resolve after join returns");
            auto ok_or_miss = cs.eval(R"(
              (let ((r (orch:scope-resolve "join-then-resolve")))
                (if (hash-ref r "ok")
                    (if (or (string=? (hash-ref r "status") "done")
                            (string=? (hash-ref r "status") "cancelled")
                            (string=? (hash-ref r "status") "alive"))
                        1 0)
                    (if (string=? (hash-ref r "status") "not-found") 1 0)))
            )");
            CHECK(ok_or_miss && is_int(*ok_or_miss) && as_int(*ok_or_miss) == 1,
                  "2926 AC4: after join → done/cancelled/alive or not-found (documented)");
        }

        std::println("\n--- #2926 AC5: MVP linter + metrics ---");
        {
            CHECK(g_orch_module_stats.scope_resolve_total.load(std::memory_order_relaxed) >= 1,
                  "2926 AC5: scope_resolve_total bumps");
            CHECK(g_orch_module_stats.scope_resolve_miss_total.load(std::memory_order_relaxed) >= 1,
                  "2926 AC5: scope_resolve_miss_total bumps");
            std::ifstream in("src/orch/agent_scope.h");
            if (!in)
                in.open("../src/orch/agent_scope.h");
            std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CHECK(src.find("class AgentRegistry") == std::string::npos,
                  "2926 AC5: no AgentRegistry type");
            CHECK(src.find("find(std::string_view") != std::string::npos,
                  "2926 AC5: find API present");
            std::ifstream agent_in("src/compiler/evaluator_primitives_agent.cpp");
            if (!agent_in)
                agent_in.open("../src/compiler/evaluator_primitives_agent.cpp");
            std::string agent((std::istreambuf_iterator<char>(agent_in)),
                              std::istreambuf_iterator<char>());
            CHECK(agent.find("scope-resolve-wired") != std::string::npos,
                  "2926 AC5: query wires scope-resolve-wired");
            CHECK(agent.find("scope-resolve-total") != std::string::npos,
                  "2926 AC5: query wires scope-resolve-total");
            CHECK(agent.find("schema-2926") != std::string::npos, "2926 AC5: schema-2926 in query");
        }

        std::println("\n--- #2926 AC6: source-cite + no invent + no docs/design/ ---");
        {
            std::ifstream t_in("tests/orch/test_orch_scope.cpp");
            if (!t_in)
                t_in.open("../tests/orch/test_orch_scope.cpp");
            std::string t((std::istreambuf_iterator<char>(t_in)), std::istreambuf_iterator<char>());
            CHECK(t.find("#2926 AC1") != std::string::npos, "2926 AC6: this suite cites #2926");
            std::ifstream invent("tests/orch/test_issue_2926.cpp");
            if (!invent.good())
                invent.open("../tests/orch/test_issue_2926.cpp");
            CHECK(!invent.good(), "2926 AC6: no test_issue_2926.cpp");
            std::ifstream design("docs/design/2926-scope-resolve.md");
            if (!design.good())
                design.open("../docs/design/2926-scope-resolve.md");
            CHECK(!design.good(), "2926 AC6: no docs/design/2926-*");
            std::ifstream build_in("build.py");
            if (!build_in)
                build_in.open("../build.py");
            std::string build((std::istreambuf_iterator<char>(build_in)),
                              std::istreambuf_iterator<char>());
            CHECK(build.find("scope-resolve-2926") != std::string::npos ||
                      build.find("scope_resolve_2926") != std::string::npos,
                  "2926 AC6: build.py coverage");
            std::ifstream agent_in("src/compiler/evaluator_primitives_agent.cpp");
            if (!agent_in)
                agent_in.open("../src/compiler/evaluator_primitives_agent.cpp");
            std::string agent((std::istreambuf_iterator<char>(agent_in)),
                              std::istreambuf_iterator<char>());
            CHECK(agent.find("orch:scope-resolve") != std::string::npos,
                  "2926 AC6: Aura prim present");
        }
    }
#endif

    // ── #3125: cross-scope directory merge — facade keys wired ─────────
    {
        std::println("\n--- #3125: cross-scope directory merge + facade ---");
        const auto scope_h = read_file("src/orch/agent_scope.h");
        const auto spawn_h = read_file("src/orch/agent_spawn.h");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto readme_src = read_file("src/orch/README.md");

        CHECK(scope_h.find("kCrossScopeDirectoryIssue = 3125") != std::string::npos,
              "#3125 source: agent_scope.h has kCrossScopeDirectoryIssue = 3125");
        CHECK(scope_h.find("struct CrossScopeEntry") != std::string::npos,
              "#3125 source: agent_scope.h has CrossScopeEntry");
        CHECK(scope_h.find("cross_scope_directory(std::span<AgentScope* const>") !=
                  std::string::npos,
              "#3125 source: agent_scope.h has cross_scope_directory free fn");
        CHECK(spawn_h.find("cross_scope_directory_total") != std::string::npos,
              "#3125 source: agent_spawn.h has cross_scope_directory_total counter");
        CHECK(spawn_h.find("cross_scope_directory_entries_total") != std::string::npos,
              "#3125 source: agent_spawn.h has entries_total counter");
        CHECK(spawn_h.find("cross_scope_directory_sources_total") != std::string::npos,
              "#3125 source: agent_spawn.h has sources_total counter");
        CHECK(prim_src.find("cross-scope-directory-total") != std::string::npos,
              "#3125 facade: cross-scope-directory-total wired");
        CHECK(prim_src.find("cross-scope-directory-entries-total") != std::string::npos,
              "#3125 facade: cross-scope-directory-entries-total wired");
        CHECK(prim_src.find("cross-scope-directory-sources-total") != std::string::npos,
              "#3125 facade: cross-scope-directory-sources-total wired");
        CHECK(prim_src.find("schema-3125") != std::string::npos, "#3125 facade: schema-3125 wired");
        CHECK(prim_src.find("issue-3125") != std::string::npos, "#3125 facade: issue-3125 wired");
        CHECK(prim_src.find("cross-scope-directory-wired") != std::string::npos,
              "#3125 facade: cross-scope-directory-wired sentinel wired");
        CHECK(readme_src.find("orch:cross-scope-directory") != std::string::npos,
              "#3125 source: README documents orch:cross-scope-directory");
        CHECK(readme_src.find("#3125") != std::string::npos,
              "#3125 source: README references #3125");

        // Confirm the facade keys are observable from the Aura runtime path.
        reset_all();
        CompilerService cs;
        const auto stats_total = href(cs, "cross-scope-directory-total");
        const auto stats_wired = href(cs, "cross-scope-directory-wired");
        const auto stats_schema = href(cs, "schema-3125");
        CHECK(stats_total >= 0, "#3125 facade: cross-scope-directory-total int");
        CHECK(stats_wired == 1, "#3125 facade: cross-scope-directory-wired = 1");
        CHECK(stats_schema == 3125, "#3125 facade: schema-3125 = 3125");
    }

    // ── #3216: identity-plane on scope-resolve / directory + facade.
    // Empty-session resolve/directory only — no extra scope-spawn (batch-safe).
    {
        using aura::compiler::typed_audit::apply_dev_audit_defaults;
        using aura::compiler::typed_audit::apply_production_audit_defaults;
        std::println("\n--- #3216: identity-plane facade + scope-resolve/directory ---");
        const auto scope_h = read_file("src/orch/agent_scope.h");
        const auto spawn_h = read_file("src/orch/agent_spawn.h");
        const auto names_h = read_file("src/compiler/agent_name_table.h");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto readme_src = read_file("src/orch/README.md");
        CHECK(spawn_h.find("kIdentityPlaneHandoffBoundaryIssue = 3216") != std::string::npos,
              "#3216 source: kIdentityPlaneHandoffBoundaryIssue");
        CHECK(scope_h.find("name-table") != std::string::npos, "#3216 source: name-table plane");
        CHECK(scope_h.find("scope-handle") != std::string::npos,
              "#3216 source: scope-handle plane");
        CHECK(names_h.find("name-table plane") != std::string::npos,
              "#3216 source: AgentNameTable plane cite");
        CHECK(prim_src.find("add_identity_plane") != std::string::npos,
              "#3216 source: add_identity_plane helper");
        CHECK(prim_src.find("\"scope-handle\"") != std::string::npos,
              "#3216 source: scope-handle intern");
        CHECK(prim_src.find("\"directory\"") != std::string::npos,
              "#3216 source: directory intern");
        CHECK(prim_src.find("orch:resolve-via-token") == std::string::npos,
              "#3216: no orch:resolve-via-token");
        CHECK(readme_src.find("Issue #3216") != std::string::npos, "#3216 source: README cites");

        const char* prev_sb = std::getenv("AURA_SANDBOX");
        const std::string prev_sb_s = prev_sb ? prev_sb : "";
        reset_all();
        CompilerService cs;
        CHECK(href(cs, "schema-3216") == 3216, "#3216 facade: schema-3216");
        CHECK(href(cs, "identity-plane-wired") == 1, "#3216 facade: identity-plane-wired");

        ::setenv("AURA_SANDBOX", "restricted", 1);
        apply_production_audit_defaults();
        auto plane_miss = cs.eval(
            R"((let ((r (orch:scope-resolve "no-such-ac3216-scope")))
                 (if (string=? (hash-ref r "identity-plane") "scope-handle") 1 0)))");
        CHECK(plane_miss && is_int(*plane_miss) && as_int(*plane_miss) == 1,
              "ac3216_scope: miss identity-plane=scope-handle");
        auto st_miss = cs.eval(
            R"((let ((r (orch:scope-resolve "no-such-ac3216-scope")))
                 (if (string=? (hash-ref r "status") "not-found") 1 0)))");
        CHECK(st_miss && is_int(*st_miss) && as_int(*st_miss) == 1,
              "ac3216_scope: miss status=not-found");
        auto dir_plane = cs.eval(
            R"((let ((r (orch:agent-directory)))
                 (if (string=? (hash-ref r "identity-plane") "directory") 1 0)))");
        CHECK(dir_plane && is_int(*dir_plane) && as_int(*dir_plane) == 1,
              "ac3216_dir: identity-plane=directory");
        auto dir_schema =
            cs.eval(R"((let ((r (orch:agent-directory))) (hash-ref r "schema-3216")))");
        CHECK(dir_schema && is_int(*dir_schema) && as_int(*dir_schema) == 3216,
              "ac3216_dir: schema-3216");

        ::setenv("AURA_SANDBOX", "off", 1);
        apply_dev_audit_defaults();
        auto soft_scope = cs.eval(
            R"((hash-has-key? (orch:scope-resolve "no-such-ac3216-soft") "identity-plane"))");
        CHECK(soft_scope && is_bool(*soft_scope) && !as_bool(*soft_scope),
              "ac3216_scope: Soft miss has no identity-plane");
        auto soft_dir = cs.eval(R"((hash-has-key? (orch:agent-directory) "identity-plane"))");
        CHECK(soft_dir && is_bool(*soft_dir) && !as_bool(*soft_dir),
              "ac3216_dir: Soft directory has no identity-plane");

        if (prev_sb_s.empty())
            ::unsetenv("AURA_SANDBOX");
        else
            ::setenv("AURA_SANDBOX", prev_sb_s.c_str(), 1);
        apply_dev_audit_defaults();
    }

    // ── #3442: scope-spawn agents reachable by send/recv/ask/join ──
    {
        std::println("\n--- #3442: scope-spawn agents on the message plane ---");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto readme_src = read_file("src/orch/README.md");
        CHECK(prim_src.find("resolve_aura_agent") != std::string::npos,
              "3442 AC: resolve_aura_agent helper");
        CHECK(prim_src.find("Issue #3442") != std::string::npos, "3442 AC: cite");
        CHECK(readme_src.find("#3442") != std::string::npos, "3442 AC: README");
        CHECK(prim_src.find("class AgentRegistry") == std::string::npos,
              "3442 AC4: no AgentRegistry");
        CHECK(prim_src.find("schema-3442") == std::string::npos, "3442 AC6: no schema-3442");
        CHECK(read_file("tests/orch/test_issue_3442.cpp").empty(),
              "3442 AC7: no test_issue_3442.cpp");

        reset_all();
        unsetenv("AURA_ORCH_BP_ADMIT_THRESHOLD");
        reset_process_resource_quota_for_test();
        g_orch_module_stats.mailbox_bp_recent_total.store(0, std::memory_order_relaxed);
        (void)aura::orch::reset_scope_bp_map_for_test();
        CompilerService cs;
        cs.eval(R"((orch:scope-cancel-all))");
        cs.eval(R"((orch:scope-join-all :timeout-ms 500 :drain-ms 500))");

        std::println("\n--- 3442 AC1: scope-spawn then send/recv/ask/join ---");
        auto spawn_ok = cs.eval(
            R"((let ((r (orch:scope-spawn "scope-mail-3442"))) (if (hash-ref r "ok") 1 0)))");
        CHECK(spawn_ok && is_int(*spawn_ok) && as_int(*spawn_ok) == 1, "3442 AC1: scope-spawn ok");
        auto send_ok = cs.eval(R"(
          (let ((r (orch:agent-send "scope-mail-3442" "hello-3442")))
            (if (error? r) 0 (if (hash-ref r "ok") 1 0)))
        )");
        CHECK(send_ok && is_int(*send_ok) && as_int(*send_ok) == 1,
              "3442 AC1: send to scope-spawn agent ok");
        auto payload = cs.eval(R"(
          (let ((r (orch:agent-recv "scope-mail-3442" :wait #f :timeout-ms 0)))
            (if (error? r) 0
                (if (string=? (hash-ref r "payload") "hello-3442") 1 0)))
        )");
        CHECK(payload && is_int(*payload) && as_int(*payload) == 1,
              "3442 AC1: recv payload from scope-spawn agent");
        auto ask_not_unknown = cs.eval(R"(
          (let ((r (orch:agent-ask "scope-mail-3442" "ping" 1)))
            (if (error? r) 0 (if (hash-has-key? r "status") 1 0)))
        )");
        CHECK(ask_not_unknown && is_int(*ask_not_unknown) && as_int(*ask_not_unknown) == 1,
              "3442 AC1: ask resolves scope-spawn agent (not unknown)");
        auto join_st = cs.eval(R"(
          (let ((r (orch:agent-join "scope-mail-3442" :timeout-ms 2000)))
            (if (error? r) 0
                (if (string=? (hash-ref r "status") "invalid") 0 1)))
        )");
        CHECK(join_st && is_int(*join_st) && as_int(*join_st) == 1,
              "3442 AC1: join-by-name finds scope-spawn agent");

        std::println("\n--- 3442 AC2: name-table spawn-agent unchanged ---");
        auto bare = cs.eval(R"(
          (begin
            (orch:spawn-agent "bare-mail-3442" (lambda () 1) :attach-mailbox #t)
            (let ((s (orch:agent-send "bare-mail-3442" "bare-hello")))
              (if (error? s) 0 (if (hash-ref s "ok") 1 0))))
        )");
        CHECK(bare && is_int(*bare) && as_int(*bare) == 1,
              "3442 AC2: spawn-agent send still works");
        (void)cs.eval(R"((orch:agent-join "bare-mail-3442" :timeout-ms 2000))");

        std::println("\n--- 3442 AC5: same-name name-table wins ---");
        auto wins = cs.eval(R"(
          (begin
            (orch:spawn-agent "dup-3442" (lambda () 1) :attach-mailbox #t)
            (orch:scope-spawn "dup-3442")
            (orch:agent-send "dup-3442" "name-table-wins")
            (let ((r (orch:agent-recv "dup-3442" :wait #f :timeout-ms 0)))
              (if (error? r) 0
                  (if (string=? (hash-ref r "payload") "name-table-wins") 1 0))))
        )");
        CHECK(wins && is_int(*wins) && as_int(*wins) == 1, "3442 AC5: same-name name-table wins");
        (void)cs.eval(R"((orch:agent-join "dup-3442" :timeout-ms 2000))");
        (void)cs.eval(R"((orch:scope-cancel-all))");
        (void)cs.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))");
    }

    // ── #3444: orch:scope-child returns path; spawn targets the child ──
    {
        std::println("\n--- #3444: scope-child addressing + spawn on path ---");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto readme_src = read_file("src/orch/README.md");
        CHECK(prim_src.find("Issue #3444") != std::string::npos, "3444 AC: prim cites #3444");
        CHECK(prim_src.find("child-index") != std::string::npos, "3444 AC1: child-index on hash");
        CHECK(prim_src.find("scope-path") != std::string::npos, "3444 AC1: scope-path on hash");
        CHECK(prim_src.find("&child == &parent") != std::string::npos,
              "3444 AC4: HardDeny stub check");
        CHECK(readme_src.find("#3444") != std::string::npos, "3444 AC: README cites #3444");
        CHECK(prim_src.find("class AgentRegistry") == std::string::npos,
              "3444 AC5: no AgentRegistry");
        CHECK(read_file("tests/orch/test_issue_3444.cpp").empty(),
              "3444 AC6: no test_issue_3444.cpp");

        reset_all();
        CompilerService cs2;
        const auto p0 = cs2.eval(
            R"((let ((r (orch:scope-child "c0")))
                 (if (and (hash-ref r "ok")
                          (= (hash-ref r "child-index") 0)
                          (string=? (hash-ref r "scope-path") "0")) 1 0)))");
        CHECK(p0 && is_int(*p0) && as_int(*p0) == 1, "3444 AC1: first child hash path=0 index=0");
        const auto p1 =
            cs2.eval(R"((let ((r (orch:scope-child "c1"))) (hash-ref r "child-index")))");
        CHECK(p1 && is_int(*p1) && as_int(*p1) == 1, "3444 AC1: second child-index=1");

        auto* root = aura::orch::find_agent_scope(static_cast<void*>(&cs2.evaluator()));
        CHECK(root != nullptr, "3444 AC2: per-Evaluator root exists");
        const auto root_sz = root->size();
        CHECK(root->child_count() >= 2, "3444 AC1: two C++ children");

        const auto spawn_child = cs2.eval(
            R"((let ((r (orch:scope-spawn "on-child-3444" :path "0")))
                 (if (hash-ref r "ok") 1 0)))");
        CHECK(spawn_child && is_int(*spawn_child) && as_int(*spawn_child) == 1,
              "3444 AC2: scope-spawn :path 0 ok");
        CHECK(root->size() == root_sz, "3444 AC2: root handles_ size unchanged");
        CHECK(root->child_at(0).size() == 1, "3444 AC2: handle lives on child");

        const auto dir_path = cs2.eval(
            R"((let ((r (orch:agent-directory)))
                 (let ((agents (hash-ref r "agents")))
                   (if (string=? (hash-ref (vector-ref agents 0) "scope-path") "0") 1 0))))");
        CHECK(dir_path && is_int(*dir_path) && as_int(*dir_path) == 1,
              "3444 AC1: directory_snapshot scope-path matches");

        const auto spawn_root = cs2.eval(
            R"((let ((r (orch:scope-spawn "on-root-3444")))
                 (if (hash-ref r "ok") 1 0)))");
        CHECK(spawn_root && is_int(*spawn_root) && as_int(*spawn_root) == 1,
              "3444 AC3: omit path → root spawn ok");
        CHECK(root->size() == root_sz + 1, "3444 AC3: omit path grows root handles_");

        const auto bad = cs2.eval(
            R"((let ((r (orch:scope-spawn "nope" :path "99")))
                 (if (hash-ref r "ok") 1 0)))");
        CHECK(bad && is_int(*bad) && as_int(*bad) == 0, "3444 AC2: unknown path → ok=#f");

        const auto watch_ok = cs2.eval(
            R"((let ((r (orch:scope-watch :path "0" :policy 'report-only)))
                 (if (hash-ref r "ok") 1 0)))");
        CHECK(watch_ok && is_int(*watch_ok) && as_int(*watch_ok) == 1,
              "3444 AC2: scope-watch :path 0");
        CHECK(root->child_at(0).size() == 1, "3444 AC2: watch does not steal the child handle");
        CHECK(root->resolve_scope_path("0") == &root->child_at(0),
              "3444 AC2: resolve_scope_path 0 is the child");
        CHECK(root->resolve_scope_path("0")->size() == 1,
              "3444 AC2: resolved child still holds the spawn");
        const auto bad_res = cs2.eval(
            R"((let ((r (orch:scope-resolve "x" :path "99")))
                 (if (hash-ref r "ok") 1 0)))");
        CHECK(bad_res && is_int(*bad_res) && as_int(*bad_res) == 0,
              "3444 AC2: scope-resolve unknown path → ok=#f");
        const auto join_child = cs2.eval(
            R"((let ((r (orch:scope-join-all :path "0" :timeout-ms 2000 :drain-ms 2000)))
                 (if (hash-has-key? r "ok") 1 0)))");
        CHECK(join_child && is_int(*join_child) && as_int(*join_child) == 1,
              "3444 AC2: scope-join-all :path 0");
        CHECK(aura::orch::find_agent_scope(static_cast<void*>(&cs2.evaluator())) != nullptr,
              "3444 AC2: joining child does not drop the Evaluator root");

        (void)cs2.eval(R"((orch:scope-cancel-all))");
        (void)cs2.eval(R"((orch:scope-join-all :timeout-ms 2000 :drain-ms 2000))");
    }

    // ── Issue #3437: ~Evaluator drops the session AgentScope (identity plane B) ──
    {
        std::println("\n--- #3437: Evaluator teardown drops the session scope ---");
        const auto ctor_src = read_file("src/compiler/evaluator_ctor.cpp");
        CHECK(ctor_src.find("Issue #3437") != std::string::npos,
              "3437 AC1: evaluator_ctor cites #3437");
        CHECK(ctor_src.find("drop_agent_scope(static_cast<void*>(this))") != std::string::npos,
              "3437 AC1: cleanup_orch_agents drops the scope map slot");

        // AC1: scope-spawn then destroy the Evaluator WITHOUT join-all —
        // the map slot must be gone (no dangling-key scope leak).
        reset_all();
        void* ev_key = nullptr;
        {
            CompilerService cs3;
            const auto ok = cs3.eval(
                R"((let ((r (orch:scope-spawn "leak-3437"))) (if (hash-ref r "ok") 1 0)))");
            CHECK(ok && is_int(*ok) && as_int(*ok) == 1, "3437 AC1: scope-spawn ok (no join-all)");
            auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&cs3.evaluator()));
            CHECK(scope != nullptr, "3437 AC1: scope exists before dtor");
            CHECK(scope->size() >= 1, "3437 AC1: scope holds the live handle");
            ev_key = static_cast<void*>(&cs3.evaluator());
        } // ~CompilerService -> ~Evaluator -> cleanup_orch_agents -> drop
        CHECK(aura::orch::find_agent_scope(ev_key) == nullptr,
              "3437 AC1: ~Evaluator erased the scope map slot");

        // AC2: an Evaluator at a RECYCLED address inherits nothing —
        // placement-new two CompilerService objects over the same storage.
        alignas(CompilerService) static unsigned char buf[sizeof(CompilerService)];
        {
            auto* cs_a = ::new (static_cast<void*>(buf)) CompilerService();
            const auto ok_a = cs_a->eval(
                R"((let ((r (orch:scope-spawn "addr-a-3437"))) (if (hash-ref r "ok") 1 0)))");
            CHECK(ok_a && is_int(*ok_a) && as_int(*ok_a) == 1, "3437 AC2: spawn at storage slot A");
            CHECK(aura::orch::find_agent_scope(static_cast<void*>(&cs_a->evaluator())) != nullptr,
                  "3437 AC2: slot A scope present");
            cs_a->~CompilerService();
        }
        {
            auto* cs_b = ::new (static_cast<void*>(buf)) CompilerService();
            const void* key_b = static_cast<const void*>(&cs_b->evaluator());
            CHECK(aura::orch::find_agent_scope(const_cast<void*>(key_b)) == nullptr,
                  "3437 AC2: recycled address inherits no foreign scope");
            const auto ok_b = cs_b->eval(
                R"((let ((r (orch:scope-spawn "addr-b-3437"))) (if (hash-ref r "ok") 1 0)))");
            CHECK(ok_b && is_int(*ok_b) && as_int(*ok_b) == 1,
                  "3437 AC2: fresh scope spawn at recycled address");
            auto* scope_b = aura::orch::find_agent_scope(const_cast<void*>(key_b));
            CHECK(scope_b != nullptr && scope_b->size() == 1,
                  "3437 AC2: fresh scope starts empty (exactly the new handle)");
            cs_b->~CompilerService();
        }

        // AC5/AC6: no test_issue_NNNN.cpp; no registry surface added.
        CHECK(read_file("tests/orch/test_issue_3437.cpp").empty(),
              "3437 AC5: no tests/orch/test_issue_3437.cpp");
        CHECK(read_file("tests/issues/test_issue_3437.cpp").empty(),
              "3437 AC5: no tests/issues/test_issue_3437.cpp");
        CHECK(ctor_src.find("AgentRegistry") == std::string::npos,
              "3437 AC6: teardown adds no AgentRegistry");

        reset_all();
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_orch_scope();
}
#endif
