// @category: unit
// @reason: Issue #2078 — per-Evaluator orch agent name bookkeeping
// (replaces process-static OrchAgentNameTable). Tests:
//   AC1: source cites #2078; no process-static OrchAgentNameTable;
//        uses ev.agent_names_->put/find instead.
//   AC2: two independent Evaluators / AgentNameTables can hold the same
//        agent name without cross-talk (drain on one does not affect
//        the other).
//   AC3: drain_for_cleanup returns the AgentHandle vector and clears
//        the table (so cleanup_orch_agents() at ~Evaluator does not
//        double-release arena reservations via the AgentHandle
//        destructor + the table).
//   AC6: test lives under tests/orch/ (src-aligned), not tests/issues/.
//
// Issue #3467: same-name put over a slot that still owes Reclaimed
// cleanup (must_wait_reclaimed / reclaimed_deferred_cleanup) is
// typed-denied (nullptr) — the pending handle is not replaced and its
// reservation stays held. Clean slots still replace (AC2/AC5).
//
// Note: AC4 (check_orch_mvp_scope.py --strict stays green) and
// AC5 (existing agent_primitives_2011.aura + fiber_orch tests remain
// green) are linter/integration checks; not duplicated here.

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>

#include "compiler/agent_name_table.h"
#include "orch/orch.h"

import std;
import aura.compiler.service;

namespace {

using aura::compiler::AgentNameTable;
using aura::compiler::CompilerService;
using aura::orch::AgentHandle;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// Build a minimal AgentHandle for storage-layer testing (no actual fiber).
// reserved_memory_bytes = 0 so the destructor is a no-op (no quota release).
// ok=true makes find() return this handle for the test path that gates on ok.
static AgentHandle make_minimal_handle(std::string name, std::uint64_t id) {
    AgentHandle h;
    h.id = id;
    h.name = std::move(name);
    h.ok = true;
    return h;
}

// ── AC1: source cites #2078 + no process-static + uses ev.agent_names_ ──
static void ac1_source_and_no_static() {
    std::println("\n--- AC1: source cites #2078 + no process-static ---");
    auto src = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(!src.empty(), "evaluator_primitives_agent.cpp readable");
    CHECK(src.find("Issue #2078") != std::string::npos, "source cites #2078");
    CHECK(src.find("ev.agent_names_->put") != std::string::npos,
          "orch:spawn-agent uses ev.agent_names_->put");
    CHECK(src.find("ev.agent_names_->find") != std::string::npos,
          "orch:agent-join/send/recv use ev.agent_names_->find");
    CHECK(src.find("resolve_aura_agent") != std::string::npos,
          "3442 AC: send/recv/ask/join resolve via resolve_aura_agent");
    CHECK(src.find("Issue #3442") != std::string::npos, "3442 AC: source cites #3442");
    CHECK(src.find("static OrchAgentNameTable orch_agent_names") == std::string::npos,
          "no process-static OrchAgentNameTable");

    auto ev_src = read_file("src/compiler/evaluator.ixx");
    CHECK(ev_src.find("Issue #2078") != std::string::npos, "evaluator.ixx cites #2078");
    CHECK(ev_src.find("std::unique_ptr<aura::compiler::AgentNameTable> agent_names_") !=
              std::string::npos,
          "Evaluator has std::unique_ptr<AgentNameTable> agent_names_ member");
    CHECK(ev_src.find("cleanup_orch_agents") != std::string::npos,
          "Evaluator declares cleanup_orch_agents()");

    auto header_src = read_file("src/compiler/agent_name_table.h");
    CHECK(!header_src.empty(), "agent_name_table.h exists");
    CHECK(header_src.find("drain_for_cleanup") != std::string::npos,
          "header exposes drain_for_cleanup");
    CHECK(header_src.find("Issue #3467") != std::string::npos, "3467 AC: put guard cites #3467");
}

// ── AC2: two AgentNameTables with same name are isolated ──────────────
static void ac2_two_tables_isolation() {
    std::println("\n--- AC2: two tables same-name isolation ---");
    AgentNameTable t1;
    AgentNameTable t2;

    auto h1 = make_minimal_handle("alpha", 100);
    auto h2 = make_minimal_handle("alpha", 200);

    t1.put(std::move(h1));
    t2.put(std::move(h2));

    auto* p1 = t1.find("alpha");
    auto* p2 = t2.find("alpha");

    CHECK(p1 != nullptr, "t1 finds alpha");
    CHECK(p2 != nullptr, "t2 finds alpha");
    CHECK(p1->id == 100, "t1 alpha has id 100");
    CHECK(p2->id == 200, "t2 alpha has id 200");
    CHECK(p1 != p2, "different memory (independent storage)");

    CHECK(t1.size() == 1, "t1 size = 1");
    CHECK(t2.size() == 1, "t2 size = 1");

    // Drain t1: t2 must remain unaffected.
    auto drained = t1.drain_for_cleanup();
    CHECK(drained.size() == 1, "t1 drained 1 handle");
    CHECK(drained[0].id == 100, "drained handle is the t1 one (id 100)");
    CHECK(t1.size() == 0, "t1 size = 0 after drain");
    CHECK(t2.size() == 1, "t2 size still 1 (cross-talk check)");
    auto* p2_after = t2.find("alpha");
    CHECK(p2_after != nullptr && p2_after->id == 200,
          "t2 still finds alpha with id 200 after t1 drain");
}

// ── AC2b: per-Evaluator isolation via two CompilerService instances ───
static void ac2b_compiler_service_isolation() {
    std::println("\n--- AC2b: two CompilerService instances ---");
    CompilerService cs1;
    CompilerService cs2;
    auto& ev1 = cs1.evaluator();
    auto& ev2 = cs2.evaluator();

    auto h1 = make_minimal_handle("beta", 11);
    auto h2 = make_minimal_handle("beta", 22);

    ev1.agent_names_->put(std::move(h1));
    ev2.agent_names_->put(std::move(h2));

    auto* p1 = ev1.agent_names_->find("beta");
    auto* p2 = ev2.agent_names_->find("beta");

    CHECK(p1 != nullptr && p1->id == 11, "ev1.beta has id 11");
    CHECK(p2 != nullptr && p2->id == 22, "ev2.beta has id 22");
    CHECK(p1 != p2, "different memory (independent Evaluator members)");

    // Drain ev1: ev2 unaffected.
    auto drained = ev1.agent_names_->drain_for_cleanup();
    CHECK(drained.size() == 1, "ev1 drained 1");
    CHECK(ev1.agent_names_->size() == 0, "ev1 empty after drain");
    CHECK(ev2.agent_names_->size() == 1, "ev2 still 1 (isolation)");
    CHECK(ev2.agent_names_->find("beta") != nullptr, "ev2 still finds beta after ev1 drain");
}

// ── AC3: drain_for_cleanup returns handles and clears table ───────────
static void ac3_drain_clears_table() {
    std::println("\n--- AC3: drain_for_cleanup behavior ---");
    AgentNameTable table;
    table.put(make_minimal_handle("agent1", 1));
    table.put(make_minimal_handle("agent2", 2));
    table.put(make_minimal_handle("agent3", 3));
    CHECK(table.size() == 3, "table has 3 handles");

    auto drained = table.drain_for_cleanup();
    CHECK(drained.size() == 3, "drained 3 handles");
    CHECK(table.size() == 0, "table empty after drain");

    // After drain: find returns nullptr (so ~Evaluator cleanup doesn't
    // double-release via both drain + AgentHandle destructor).
    CHECK(table.find("agent1") == nullptr, "find agent1 after drain = null");
    CHECK(table.find("agent2") == nullptr, "find agent2 after drain = null");
    CHECK(table.find("agent3") == nullptr, "find agent3 after drain = null");

    // After drain: AgentHandle destructors run when drained vector goes
    // out of scope, releasing any reserved_memory_bytes. Our minimal
    // handles have reserved_memory_bytes = 0, so this is a no-op (the
    // production path uses real AgentHandles from spawn_agent_with_mailbox
    // which carry the arena reservation).
}

// ── AC3b: same-name spawn overrides prior (insert path uses try_emplace) ─
static void ac3b_same_name_overrides() {
    std::println("\n--- AC3b: same-name spawn overrides prior ---");
    AgentNameTable table;
    table.put(make_minimal_handle("dup", 1));
    auto* p = table.find("dup");
    CHECK(p != nullptr && p->id == 1, "first spawn id=1");

    table.put(make_minimal_handle("dup", 2));
    CHECK(table.size() == 1, "still size 1 after override");
    p = table.find("dup");
    CHECK(p != nullptr && p->id == 2, "second spawn id=2 (override)");
}

// ── AC4: #3125 cross-scope directory merge — separation from #2078 name table ──
static void ac3125_cross_scope_isolation() {
    std::println("\n--- AC4: #3125 cross-scope directory merge — separation from #2078 ---");
    // Source-cite: cross_scope_directory is in agent_scope.h (not agent_name_table.h).
    // Per #2078, AgentNameTable is per-Evaluator storage. Cross-scope merge
    // (#3125) walks an explicit span<AgentScope* const> caller-owned list —
    // it does NOT consult AgentNameTable. Verify the separation:
    //   - agent_scope.h owns cross_scope_directory + CrossScope* types
    //   - agent_name_table.h owns AgentNameTable (no cross-scope surface)
    //   - README documents both #2078 and #3125 as distinct concerns
    auto scope_h = read_file("src/orch/agent_scope.h");
    auto name_h = read_file("src/compiler/agent_name_table.h");
    auto readme = read_file("src/orch/README.md");
    CHECK(scope_h.find("cross_scope_directory(std::span<AgentScope* const>") != std::string::npos,
          "AC4: agent_scope.h owns cross_scope_directory free fn");
    CHECK(scope_h.find("struct CrossScopeEntry") != std::string::npos,
          "AC4: agent_scope.h owns CrossScopeEntry");
    CHECK(scope_h.find("struct CrossScopeFilter") != std::string::npos,
          "AC4: agent_scope.h owns CrossScopeFilter");
    CHECK(scope_h.find("struct CrossScopeSnapshot") != std::string::npos,
          "AC4: agent_scope.h owns CrossScopeSnapshot");
    CHECK(name_h.find("cross_scope_directory") == std::string::npos,
          "AC4: agent_name_table.h does NOT reference cross_scope_directory");
    CHECK(name_h.find("CrossScopeEntry") == std::string::npos,
          "AC4: agent_name_table.h does NOT reference CrossScopeEntry");
    CHECK(name_h.find("AgentNameTable") != std::string::npos,
          "AC4: agent_name_table.h owns AgentNameTable (#2078 surface)");
    CHECK(readme.find("#3125") != std::string::npos,
          "AC4: README documents #3125 cross-scope merge");
    CHECK(readme.find("#2078") != std::string::npos,
          "AC4: README references #2078 per-Evaluator name table");
    CHECK(readme.find("cross-scope directory merge") != std::string::npos,
          "AC4: README has 'cross-scope directory merge' section");
}

// ── #3442: message prims resolve name-table then session-local scope ──
static void ac3442_message_plane_resolve() {
    std::println("\n--- #3442: name-table-then-scope message resolve ---");
    auto src = read_file("src/compiler/evaluator_primitives_agent.cpp");
    auto names = read_file("src/compiler/agent_name_table.h");
    auto readme = read_file("src/orch/README.md");
    CHECK(src.find("resolve_aura_agent") != std::string::npos,
          "3442 AC: resolve_aura_agent helper present");
    const auto helper = src.find("resolve_aura_agent");
    const auto name_find = src.find("ev.agent_names_->find(name)", helper);
    const auto scope_find = src.find("find_agent_scope(static_cast<void*>(&ev))", helper);
    CHECK(helper != std::string::npos && name_find != std::string::npos &&
              scope_find != std::string::npos && name_find < scope_find,
          "3442 AC2: name-table find sits BEFORE AgentScope::find");
    CHECK(src.find("class AgentRegistry") == std::string::npos,
          "3442 AC4: no AgentRegistry in agent prims");
    CHECK(names.find("never auto-puts scope handles") != std::string::npos,
          "3442 AC5: AgentNameTable documents no auto-put of scope handles");
    CHECK(readme.find("name-table wins") != std::string::npos,
          "3442 AC5: README documents name-table wins");
    CHECK(src.find("schema-3442") == std::string::npos, "3442 AC6: no schema-3442 query key");
    CHECK(read_file("tests/orch/test_issue_3442.cpp").empty() &&
              read_file("tests/issues/test_issue_3442.cpp").empty(),
          "3442 AC7: no test_issue_3442.cpp");
    CHECK(read_file("docs/design/3442-scope-message-resolve.md").empty(),
          "3442 AC7: no docs/design/3442-*");

    // Plane isolation: same name in two tables stays independent.
    // Resolve prefers the name-table handle (documented AC5).
    AgentNameTable table;
    table.put(make_minimal_handle("dup-3442", 11));
    auto* nt = table.find("dup-3442");
    CHECK(nt != nullptr && nt->id == 11, "3442 AC5: name-table holds dup-3442 id=11");
}

// ── #3467: same-name put over reclaimed-pending slot is typed-denied ──
static void ac3467_put_deny_pending() {
    std::println("\n--- #3467: put deny over reclaimed-pending slot ---");

    // AC1: must_wait_reclaimed slot is not replaced; old handle intact.
    {
        AgentNameTable table;
        auto old_h = make_minimal_handle("dup-3467", 1);
        old_h.must_wait_reclaimed = true;
        table.put(std::move(old_h));
        {
            auto denied = make_minimal_handle("dup-3467", 2);
            auto* slot = table.put(std::move(denied));
            CHECK(slot == nullptr,
                  "3467 AC1: put over must_wait slot returns nullptr (typed deny)");
        }
        auto* p = table.find("dup-3467");
        CHECK(p != nullptr && p->id == 1, "3467 AC1: pending handle NOT replaced");
        CHECK(p->must_wait_reclaimed, "3467 AC1: pending flags intact on old handle");
        CHECK(table.size() == 1, "3467 AC1: table size unchanged after deny");

        // AC5: after the pending flag clears (Done-path wait_reclaimed_body /
        // ensure_reclaimed_cleanup effect), same-name put is allowed again.
        auto* done = table.find("dup-3467");
        CHECK(done != nullptr, "3467 AC5: slot findable before retry");
        done->must_wait_reclaimed = false; // Done-path cleanup effect
        auto after = make_minimal_handle("dup-3467", 7);
        auto* slot = table.put(std::move(after));
        CHECK(slot != nullptr && slot->id == 7,
              "3467 AC5: put allowed after cleanup (flags cleared)");
        CHECK(table.find("dup-3467") != nullptr && table.find("dup-3467")->id == 7,
              "3467 AC5: replacement visible after cleanup");
    }

    // AC1b: reclaimed_deferred_cleanup slot is not replaced either.
    {
        AgentNameTable t2;
        auto deferred = make_minimal_handle("deferred-3467", 3);
        deferred.reclaimed_deferred_cleanup = true;
        t2.put(std::move(deferred));
        {
            auto denied = make_minimal_handle("deferred-3467", 4);
            CHECK(t2.put(std::move(denied)) == nullptr,
                  "3467 AC1: put over deferred-cleanup slot denied");
        }
        auto* dp = t2.find("deferred-3467");
        CHECK(dp != nullptr && dp->id == 3 && dp->reclaimed_deferred_cleanup,
              "3467 AC1: deferred slot intact after deny");
    }

    // AC2: flags false → same-name put still replaces (today's behavior;
    // the guard is two bool loads, no atomic, no state).
    {
        AgentNameTable t3;
        t3.put(make_minimal_handle("clean-3467", 5));
        auto* cp = t3.find("clean-3467");
        CHECK(cp != nullptr && !cp->must_wait_reclaimed && !cp->reclaimed_deferred_cleanup,
              "3467 AC2: clean slot flags false");
        auto* slot = t3.put(make_minimal_handle("clean-3467", 6));
        CHECK(slot != nullptr && slot->id == 6, "3467 AC2: clean slot still replaces");
        CHECK(t3.find("clean-3467") != nullptr && t3.find("clean-3467")->id == 6,
              "3467 AC2: replacement visible");
        CHECK(t3.size() == 1, "3467 AC2: size stays 1 after replace");
    }

    // Source-cite: spawn pre-check + guarded drop in the agent prims.
    auto src = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(src.find("Issue #3467") != std::string::npos,
          "3467 AC: evaluator_primitives_agent.cpp cites #3467");
    CHECK(src.find("name-reuse-while-reclaimed-pending") != std::string::npos,
          "3467 AC1: spawn deny carries deny-detail (AgentDenyClass::Other)");
    CHECK(src.find("all_settled") != std::string::npos,
          "3467 AC4: scope-join-all guarded drop present");
    CHECK(read_file("tests/orch/test_issue_3467.cpp").empty() &&
              read_file("tests/issues/test_issue_3467.cpp").empty(),
          "3467 AC6: no test_issue_3467.cpp (src-aligned suites only)");
}

} // namespace

int run_test_agent_name_table_isolation() {
    std::println("=== Issue #2078: per-Evaluator orch agent name table ===");
    ac1_source_and_no_static();
    ac2_two_tables_isolation();
    ac2b_compiler_service_isolation();
    ac3_drain_clears_table();
    ac3b_same_name_overrides();
    ac3125_cross_scope_isolation();
    ac3442_message_plane_resolve();
    ac3467_put_deny_pending();
    std::println("\n=== #2078/#3125/#3442/#3467: passed={} failed={} ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_name_table_isolation();
}
#endif
