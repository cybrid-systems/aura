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

} // namespace

int main() {
    std::println("=== Issue #2078: per-Evaluator orch agent name table ===");
    ac1_source_and_no_static();
    ac2_two_tables_isolation();
    ac2b_compiler_service_isolation();
    ac3_drain_clears_table();
    ac3b_same_name_overrides();
    std::println("\n=== #2078: passed={} failed={} ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}