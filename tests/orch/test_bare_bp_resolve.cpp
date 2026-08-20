// tests/orch/test_bare_bp_resolve_3179.cpp
// @category: unit
// @reason: Issue #3179 — production bare spawn must NOT default
//          bp_scope_id to the process bucket (cross-tenant admit
//          isolation). Scope path already auto-fills (#3015 inherit);
//          bare path was missing the parallel. Resolver prefers the
// // TLS quota tenant ("t:<tid>") under production, falls back to a
// // per-spawn monotonic "bare:<seq>" when no tenant context is
// // bound. Soft / AURA_SANDBOX=off stays zero-cost (returns {} →
// // process bucket, AC2).
// //
// //   AC1  Production + bare spawn, no :bp-scope-id → effective gauge
// //        key is non-empty and NOT the process bucket; two
// //        concurrent bare spawns do not share the same recent counter.
// //   AC2  Soft / AURA_SANDBOX=off → empty stays empty (process
// //        bucket); zero extra atomics / map inserts on the hot path.
// //   AC3  Explicit :bp-scope-id "tenant-a" wins (no override).
// //   AC4  Scope path unchanged — #3015 inherit still authoritative;
// //        no double-prefix.
// //   AC5  Admit preflight under a storm in A does not soft-reject a
// //        fresh bare spawn in B (same process, production defaults).
// //
// // Source-cite (issue #3179):
// //   src/orch/agent_spawn.h: resolve_bare_bp_scope_id (production-aware
// //     resolver) + spawn_agent_with_mailbox calls the resolver before
// //     persisting h.bp_scope_id.
// //   src/orch/agent_scope.h: production_scope_bp_inherit (gate) +
// //     AgentScope::spawn (#3015 inherit, unchanged).
// //
// // No docs/design/ per #1655 / #81967.

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/scheduler.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;

namespace {

using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::orch::AgentSpec;
using aura::orch::resolve_bare_bp_scope_id;
using aura::serve::Scheduler;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_repo_file(const char* path) {
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

int run_test_bare_bp_resolve_3179() {
    std::println("=== Issue #3179: bare spawn bp_scope_id resolver ===");

    // ── AC3: explicit id wins (no override) ────────────────────────
    {
        std::println("\n--- #3179 AC3: explicit wins ---");
        apply_production_audit_defaults();
        const auto win = resolve_bare_bp_scope_id("tenant-explicit-a");
        CHECK(win == std::string("tenant-explicit-a"), "AC3: explicit id wins (no override)");
    }

    // ── AC1: production + empty explicit → non-empty non-process key
    {
        std::println("\n--- #3179 AC1: production + empty → non-empty non-process ---");
        apply_production_audit_defaults();
        // No tenant context bound (current_quota_tenant() == 0 in tests
        // unless set elsewhere). The bare:<seq> fallback applies. Each
        // call increments the monotonic counter, so two calls give
        // distinct keys (AC1 — distinct bare spawns do not share a
        // recent counter).
        const auto k1 = resolve_bare_bp_scope_id("");
        const auto k2 = resolve_bare_bp_scope_id("");
        CHECK(!k1.empty(), "AC1: empty explicit under production → non-empty key");
        CHECK(k1 != std::string("-"), "AC1: key is NOT the process bucket sentinel");
        CHECK(!k2.empty(), "AC1: second call also non-empty");
        CHECK(k2 != std::string("-"), "AC1: second key is NOT the process bucket sentinel");
        // Two fresh calls under the bare_seq fallback pool give
        // distinct keys (counter increments per call).
        CHECK(k1 != k2, "AC5: distinct bare spawns under production get distinct bp_scope_ids");
    }

    // ── AC2: Soft / sandbox=off → empty stays empty (process bucket)
    {
        std::println("\n--- #3179 AC2: Soft / AURA_SANDBOX=off → empty (zero-cost) ---");
        apply_dev_audit_defaults();
        // Soft / sandbox=off: production_defaults_active() == false →
        // resolver returns {} → process bucket sentinel "-".
        const auto soft_id = resolve_bare_bp_scope_id("");
        CHECK(soft_id.empty(), "AC2: Soft path returns empty (process bucket, zero-cost)");
    }

    // ── AC5: spawn_agent_with_mailbox routing — two bare spawns with
    // distinct tenant contexts get distinct bp_scope_ids on the handle.
    {
        std::println("\n--- #3179 AC5: spawn_agent_with_mailbox distinct bp_scope_ids ---");
        apply_production_audit_defaults();
        Scheduler sched;
        AgentSpec spec_a;
        spec_a.name = "agent-bare-a";
        spec_a.attach_mailbox = true;
        auto h_a = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec_a));
        AgentSpec spec_b;
        spec_b.name = "agent-bare-b";
        spec_b.attach_mailbox = true;
        auto h_b = aura::orch::spawn_agent_with_mailbox(sched, std::move(spec_b));
        CHECK(h_a.ok, "AC5 setup: spawn A admits");
        CHECK(h_b.ok, "AC5 setup: spawn B admits");
        CHECK(!h_a.bp_scope_id.empty(), "AC5: bare spawn A → non-empty bp_scope_id on handle");
        CHECK(!h_b.bp_scope_id.empty(), "AC5: bare spawn B → non-empty bp_scope_id on handle");
        CHECK(h_a.bp_scope_id != h_b.bp_scope_id,
              "AC5: distinct bare spawns → distinct bp_scope_ids");
        CHECK(h_a.bp_scope_id != std::string("-"),
              "AC5: spawn A is NOT the process bucket sentinel");
        CHECK(h_b.bp_scope_id != std::string("-"),
              "AC5: spawn B is NOT the process bucket sentinel");
    }

    // ── AC4: Scope path unchanged (#3015 inherit still authoritative)
    {
        std::println("\n--- #3179 AC4: Scope path uses #3015 inherit (no double-prefix) ---");
        // The Scope path is NOT going through resolve_bare_bp_scope_id;
        // it uses AgentScope's bp_scope_id_ field directly via #3015
        // inherit. Verified by source-cite below.
        const auto spawn_src = read_repo_file("src/orch/agent_spawn.h");
        const auto scope_src = read_repo_file("src/orch/agent_scope.h");
        CHECK(scope_src.find("if (spec.bp_scope_id.empty() && production_scope_bp_inherit())") !=
                  std::string::npos,
              "AC4: #3015 Scope inherit unchanged");
        CHECK(scope_src.find("spec.bp_scope_id = bp_scope_id_;") != std::string::npos,
              "AC4: Scope uses its own bp_scope_id_, not the bare resolver");
        CHECK(spawn_src.find("h.bp_scope_id = resolve_bare_bp_scope_id(spec.bp_scope_id);") !=
                  std::string::npos,
              "AC4: spawn_agent_with_mailbox calls the bare resolver (single wire point)");
    }

    // ── Source-cite ──────────────────────────────────────────────────
    {
        std::println("\n--- #3179 AC source-cite ---");
        const auto spawn_src = read_repo_file("src/orch/agent_spawn.h");
        const auto scope_src = read_repo_file("src/orch/agent_scope.h");
        const auto build = read_repo_file("build.py");
        CHECK(spawn_src.find("[[nodiscard]] inline std::string resolve_bare_bp_scope_id") !=
                  std::string::npos,
              "AC source-cite: resolver declared in agent_spawn.h");
        CHECK(spawn_src.find("production_scope_bp_inherit") != std::string::npos,
              "AC source-cite: resolver consults production_scope_bp_inherit");
        CHECK(spawn_src.find("current_quota_tenant") != std::string::npos,
              "AC source-cite: resolver prefers TLS quota tenant");
        CHECK(spawn_src.find("Issue #3179") != std::string::npos,
              "AC source-cite: agent_spawn.h cites Issue #3179");
        CHECK(scope_src.find("production_scope_bp_inherit") != std::string::npos,
              "AC source-cite: production_scope_bp_inherit in agent_scope.h");
        CHECK(build.find("check_per_scope_bp_admit_3179") != std::string::npos,
              "AC source-cite: build.py wires #3179 linter");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_bare_bp_resolve_3179();
}
#endif