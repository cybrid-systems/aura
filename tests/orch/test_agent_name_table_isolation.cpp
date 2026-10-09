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
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/agent_name_table.h"
#include "compiler/handoff_token_stash.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "orch/agent_scope.h"
#include "orch/orch.h"

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::AgentNameTable;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::compiler::types::is_string;
using aura::orch::AgentHandle;
using aura::orch::reset_all_agent_scopes_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac3727_set_prod(bool on) {
    // Multi-worker orch scheduler (#3586) FATALs unless AURA_SANDBOX=off
    // or production bootstrap is latched before Scheduler::run.
    ::setenv("AURA_SANDBOX", "off", 1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    aura::compiler::typed_audit::g_typed_mutation_audit_counters.production_defaults_active.store(
        on ? 1u : 0u, std::memory_order_relaxed);
}

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
// Issue #3598: reserved_memory_bytes = 64 keeps the slot NON-clean so find
// resolves it — these tests exercise storage-layer resolution/isolation,
// not retirement. (A fully-clean slot — flags false, fiber null, reserved
// 0 — retires on find per #3598; that contract is covered by the #3598 AC
// below and by test_join_drain_reclaim.cpp.) ok=true makes find() return
// this handle for the test path that gates on ok.
static AgentHandle make_minimal_handle(std::string name, std::uint64_t id) {
    AgentHandle h;
    h.id = id;
    h.name = std::move(name);
    h.ok = true;
    h.reserved_memory_bytes = 64;
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
    // Issue #3671: the guarded-drop variable is tree_settled_now (was
    // all_settled) — same #3496 gate, now also feeding the hash blame
    // fields.
    CHECK(src.find("tree_settled_now") != std::string::npos,
          "3467 AC4: scope-join-all guarded drop present");
    CHECK(read_file("tests/orch/test_issue_3467.cpp").empty() &&
              read_file("tests/issues/test_issue_3467.cpp").empty(),
          "3467 AC6: no test_issue_3467.cpp (src-aligned suites only)");
}

// ── #3598: fully-clean slot retires on find; pending never does ──
static void ac3598_clean_slot_retire() {
    std::println("\n--- #3598: clean slot retires on find (fresh insert after) ---");
    // A fully-clean slot (flags false, fiber null, reserved 0) is retired by
    // the next find (nullptr, ~AgentHandle ran); the same-name put after is
    // a fresh insert. Pending slots (either flag) stay resolvable (#3467).
    {
        AgentNameTable table;
        aura::orch::AgentHandle clean;
        clean.name = "ghost-3598";
        clean.id = 71;
        clean.ok = true; // flags false, fiber null, reserved 0 → clean
        CHECK(table.put(std::move(clean)) != nullptr, "3598: clean put lands");
        CHECK(table.find("ghost-3598") == nullptr, "3598: find retires the clean slot");
        CHECK(table.size() == 0, "3598: slot erased (~AgentHandle ran)");
        CHECK(table.put(make_minimal_handle("ghost-3598", 72)) != nullptr,
              "3598: same-name put after retire = fresh insert");
        CHECK(table.find("ghost-3598") != nullptr && table.find("ghost-3598")->id == 72,
              "3598: fresh slot carries the new handle");
    }
    // Pending slot (must_wait) is never retired by find.
    {
        AgentNameTable table;
        auto pending = make_minimal_handle("pend-3598", 73);
        pending.must_wait_reclaimed = true;
        CHECK(table.put(std::move(pending)) != nullptr, "3598: pending put lands");
        auto* p = table.find("pend-3598");
        CHECK(p != nullptr && p->must_wait_reclaimed,
              "3598: pending slot stays resolvable (#3467)");
    }
}

// ── Issue #3727: pending name-table must not shadow live scope-spawn ──

static void ac3727_mark_pending(CompilerService& cs, const char* name) {
    auto* p = cs.evaluator().agent_names_->find(name);
    CHECK(p != nullptr, "3727: name-table slot exists to mark pending");
    if (!p)
        return;
    p->must_wait_reclaimed = true;
    p->reserved_memory_bytes = 64;
}

static void ac3727_retire_name_table(CompilerService& cs, const char* name) {
    auto* p = cs.evaluator().agent_names_->find(name);
    if (!p)
        return;
    p->must_wait_reclaimed = false;
    p->reclaimed_deferred_cleanup = false;
    p->reserved_memory_bytes = 0;
    p->fiber = nullptr;
    (void)cs.evaluator().agent_names_->find(name); // #3598 retire
}

static void ac3727_1_scope_spawn_denies_pending_name() {
    std::println("\n--- #3727 AC1: pending name-table → scope-spawn deny ---");
    reset_all_agent_scopes_for_test();
    aura::core::provenance::set_multi_tenant_env_active(true);
    ac3727_set_prod(true);
    CompilerService cs;
    auto spawned = cs.eval(R"((hash-ref (orch:spawn-agent "foo-3727") "ok"))");
    CHECK(spawned && is_bool(*spawned) && as_bool(*spawned), "3727 AC1: spawn-agent foo landed");
    ac3727_mark_pending(cs, "foo-3727");
    auto r = cs.eval(R"(
        (let ((h (orch:scope-spawn "foo-3727")))
          (if (and (not (hash-ref h "ok"))
                   (string=? (hash-ref h "deny-detail" "")
                             "name-reuse-while-reclaimed-pending"))
              1 0))
    )");
    CHECK(r && is_int(*r) && as_int(*r) == 1,
          "3727 AC1: scope-spawn ok=#f + deny-detail name-reuse-while-reclaimed-pending");
    ac3727_set_prod(false);
    aura::core::provenance::set_multi_tenant_env_active(false);
    reset_all_agent_scopes_for_test();
}

static void ac3727_2_after_cleanup_scope_spawn_and_send() {
    std::println("\n--- #3727 AC2: after name-table cleanup, scope-spawn + send ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(true);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:spawn-agent "foo-3727b") "ok"))").has_value(),
          "3727 AC2: spawn-agent landed");
    ac3727_mark_pending(cs, "foo-3727b");
    auto denied = cs.eval(R"((hash-ref (orch:scope-spawn "foo-3727b") "ok"))");
    CHECK(denied && is_bool(*denied) && !as_bool(*denied),
          "3727 AC2 pre: still denied while pending");
    ac3727_retire_name_table(cs, "foo-3727b");
    auto r = cs.eval(R"(
        (let ((h (orch:scope-spawn "foo-3727b")))
          (if (hash-ref h "ok")
              (begin
                (orch:agent-send "foo-3727b" "ping-3727")
                (let ((m (orch:agent-recv "foo-3727b" :wait #t :timeout-ms 200)))
                  (if (and (hash-ref m "ok")
                           (string=? (hash-ref m "payload" "") "ping-3727"))
                      1 0)))
              0))
    )");
    CHECK(r && is_int(*r) && as_int(*r) == 1,
          "3727 AC2: scope-spawn succeeds after cleanup; send reaches scope agent");
    ac3727_set_prod(false);
    reset_all_agent_scopes_for_test();
}

static void ac3727_3_touch_poll_export_scope_name() {
    std::println("\n--- #3727 AC3: touch/poll/export resolve a scope-spawned name ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(false);
    CompilerService cs;
    auto r = cs.eval(R"(
        (let ((h (orch:scope-spawn "scope-3727")))
          (if (hash-ref h "ok")
              (let ((t (orch:agent-touch "scope-3727"))
                    (p (orch:agent-poll "scope-3727"))
                    (tok (orch:agent-export-via-token "scope-3727")))
                (if (and (hash-ref t "ok") (hash-ref p "ok") (string? tok)
                         (> (string-length tok) 0))
                    1 0))
              0))
    )");
    CHECK(r && is_int(*r) && as_int(*r) == 1,
          "3727 AC3: touch/poll/export-via-token resolve scope-spawned name");
    reset_all_agent_scopes_for_test();
}

static void ac3727_4_directory_scope_only() {
    std::println("\n--- #3727 AC4: directory lists only Scope agents ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(false);
    CompilerService cs;
    auto r = cs.eval(R"(
        (begin
          (orch:spawn-agent "nt-only-3727")
          (orch:scope-spawn "sc-3727")
          (let ((d (orch:agent-directory)))
            (let ((n (hash-ref d "count")))
              (if (and (> n 0)
                       (not (hash-ref (orch:scope-resolve "nt-only-3727") "ok")))
                  1 0))))
    )");
    CHECK(r && is_int(*r) && as_int(*r) == 1,
          "3727 AC4: directory/scope-resolve see Scope agents; name-table-only is not a scope row");
    reset_all_agent_scopes_for_test();
}

static void ac3727_5_soft_no_extra_deny_and_source() {
    std::println("\n--- #3727 AC5: Soft no extra deny; no new query key; no invent ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(false);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:spawn-agent "soft-3727") "ok"))").has_value(),
          "3727 AC5: spawn-agent under Soft");
    ac3727_mark_pending(cs, "soft-3727");
    // Issue #4238: distinct non-zero region keys — under production this
    // scope now carries two mutate agents, and the region-key-missing
    // admit gate (#4238) would otherwise preempt this Soft-face shadow AC.
    auto r = cs.eval(R"((hash-ref (orch:scope-spawn "soft-3727" :region-key 1) "ok"))");
    CHECK(r && is_bool(*r) && as_bool(*r),
          "3727 AC5: Soft/Off scope-spawn does not extra-deny pending name-table");
    const auto src = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(src.find("Issue #3727") != std::string::npos, "3727 AC5: prim cites #3727");
    CHECK(src.find("name-reuse-while-reclaimed-pending") != std::string::npos,
          "3727 AC5: deny-detail reused");
    const auto touch = src.find("add(\"orch:agent-touch\"");
    CHECK(touch != std::string::npos, "3727 AC5: touch prim located");
    if (touch != std::string::npos) {
        const auto body = src.substr(touch, 800);
        CHECK(body.find("resolve_aura_agent(ev, name)") != std::string::npos,
              "3727 AC5: touch uses resolve_aura_agent");
    }
    CHECK(src.find("query:3727") == std::string::npos, "3727 AC5: no query:3727");
    CHECK(src.find("class AgentRegistry") == std::string::npos, "3727 AC5: no AgentRegistry");
    CHECK(read_file("tests/orch/test_issue_3727.cpp").empty(), "3727 AC5: no test_issue_3727.cpp");
    CHECK(read_file("docs/design/3727-name-table-scope-shadow.md").empty(),
          "3727 AC5: no docs/design/3727-*");
    // Dual-Evaluator: Evaluator-2 pending does not shadow Evaluator-1.
    CompilerService cs2;
    auto h2 = make_minimal_handle("peer-3727", 99);
    h2.must_wait_reclaimed = true;
    cs2.evaluator().agent_names_->put(std::move(h2));
    ac3727_set_prod(true);
    // Issue #4238: key 2 is distinct from soft-3727's key 1 so the admit
    // gate stays out of the way of this dual-Evaluator shadow check.
    auto peer = cs.eval(R"((hash-ref (orch:scope-spawn "peer-3727" :region-key 2) "ok"))");
    CHECK(peer && is_bool(*peer) && as_bool(*peer),
          "3727 AC5: Evaluator-2 pending foo does not shadow Evaluator-1 scope-spawn");
    ac3727_set_prod(false);
    reset_all_agent_scopes_for_test();
}

static void ac3925_1_prod_denies_live_name_table() {
    std::println("\n--- #3925 AC1: production live name-table occupancy denies scope-spawn ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(true);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:spawn-agent "live-3925") "ok"))").has_value(),
          "3925 AC1: spawn-agent live");
    auto r = cs.eval(R"(
        (let ((h (orch:scope-spawn "live-3925")))
          (and (not (hash-ref h "ok"))
               (string=? (hash-ref h "deny-detail" "")
                         "name-reuse-while-live-name-table")))
    )");
    CHECK(r && is_bool(*r) && as_bool(*r),
          "3925 AC1: scope-spawn ok=#f + deny-detail live name-table");
    ac3727_set_prod(false);
    reset_all_agent_scopes_for_test();
}

static void ac3925_2_directory_name_table_count() {
    std::println("\n--- #3925 AC2: directory name-table-count is additive ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(false);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:spawn-agent "nt-3925") "ok"))").has_value(),
          "3925 AC2: spawn-agent");
    auto n = cs.eval(R"((hash-ref (orch:agent-directory) "name-table-count"))");
    CHECK(n && is_int(*n) && as_int(*n) >= 1, "3925 AC2: name-table-count ≥ 1");
    auto c = cs.eval(R"((hash-ref (orch:agent-directory) "count"))");
    CHECK(c && is_int(*c) && as_int(*c) == 0, "3925 AC2: count still Scope-only");
    reset_all_agent_scopes_for_test();
}

static void ac3925_5_soft_and_source() {
    std::println("\n--- #3925 AC5: Soft live dual occupancy; no new query key ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(false);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:spawn-agent "soft-3925") "ok"))").has_value(),
          "3925 AC5: spawn-agent Soft");
    auto r = cs.eval(R"((hash-ref (orch:scope-spawn "soft-3925") "ok"))");
    CHECK(r && is_bool(*r) && as_bool(*r), "3925 AC5: Soft still allows live dual occupancy");
    const auto src = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(src.find("#3925") != std::string::npos, "3925 AC5: cite");
    CHECK(src.find("name-reuse-while-live-name-table") != std::string::npos,
          "3925 AC5: deny-detail");
    CHECK(src.find("name-table-count") != std::string::npos, "3925 AC5: directory field");
    CHECK(src.find("query:3925") == std::string::npos, "3925 AC5: no query key");
    CHECK(read_file("docs/design/3925-identity-plane.md").empty(), "3925: no docs/design");
    CHECK(read_file("tests/orch/test_issue_3925.cpp").empty(), "3925: no test_issue_3925");
    reset_all_agent_scopes_for_test();
}

static void ac3937_1_prod_denies_live_scope() {
    std::println("\n--- #3937 AC1: production live Scope occupancy denies spawn-agent ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(true);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:scope-spawn "live-3937") "ok"))").has_value(),
          "3937 AC1: scope-spawn live");
    auto r = cs.eval(R"(
        (let ((h (orch:spawn-agent "live-3937")))
          (and (not (hash-ref h "ok"))
               (string=? (hash-ref h "deny-detail" "")
                         "name-reuse-while-live-scope")))
    )");
    CHECK(r && is_bool(*r) && as_bool(*r), "3937 AC1: spawn-agent ok=#f + deny-detail live Scope");
    CHECK(cs.evaluator().agent_names_->find("live-3937") == nullptr, "3937 AC1: no name-table put");
    auto n = cs.eval(R"((hash-ref (orch:agent-directory) "name-table-count"))");
    CHECK(n && is_int(*n) && as_int(*n) == 0, "3937 AC1: name-table-count still 0");
    auto c = cs.eval(R"((hash-ref (orch:agent-directory) "count"))");
    CHECK(c && is_int(*c) && as_int(*c) >= 1, "3937 AC1: directory still lists Scope");
    auto* scope = aura::orch::find_agent_scope(static_cast<void*>(&cs.evaluator()));
    CHECK(scope && scope->find("live-3937") && scope->find("live-3937")->ok,
          "3937 AC1: Scope handle unchanged");
    auto ping = cs.eval(R"(
        (begin
          (orch:agent-send "live-3937" "ping-3937")
          (let ((m (orch:agent-recv "live-3937" :wait #t :timeout-ms 200)))
            (and (hash-ref m "ok")
                 (string=? (hash-ref m "payload" "") "ping-3937"))))
    )");
    CHECK(ping && is_bool(*ping) && as_bool(*ping),
          "3937 AC1: send/recv still hit the Scope mailbox");
    ac3727_set_prod(false);
    reset_all_agent_scopes_for_test();
}

static void ac3937_5_soft_and_source() {
    std::println("\n--- #3937 AC5: Soft live dual occupancy; no new query key ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(false);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:scope-spawn "soft-3937") "ok"))").has_value(),
          "3937 AC5: scope-spawn Soft");
    auto r = cs.eval(R"((hash-ref (orch:spawn-agent "soft-3937") "ok"))");
    CHECK(r && is_bool(*r) && as_bool(*r), "3937 AC5: Soft still allows live dual occupancy");
    CHECK(cs.evaluator().agent_names_->find("soft-3937") != nullptr,
          "3937 AC5: Soft spawn-agent still puts");
    const auto src = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(src.find("#3937") != std::string::npos, "3937 AC5: cite");
    CHECK(src.find("name-reuse-while-live-scope") != std::string::npos, "3937 AC5: deny-detail");
    CHECK(src.find("query:3937") == std::string::npos, "3937 AC5: no query key");
    CHECK(src.find("class AgentRegistry") == std::string::npos, "3937 AC5: no AgentRegistry");
    CHECK(read_file("docs/design/3937-identity-plane.md").empty(), "3937: no docs/design");
    CHECK(read_file("tests/orch/test_issue_3937.cpp").empty(), "3937: no test_issue_3937");
    reset_all_agent_scopes_for_test();
}

static void ac3729_1_scope_export_import_recv() {
    std::println("\n--- #3729 AC1: scope-spawn export → import recv ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(false);
    CompilerService cs1;
    CompilerService cs2;
    auto spawned = cs1.eval(R"((hash-ref (orch:scope-spawn "src-3729") "ok"))");
    CHECK(spawned && is_bool(*spawned) && as_bool(*spawned), "3729 AC1: scope-spawn ok");
    auto src_send = cs1.eval(R"((hash-ref (orch:agent-send "src-3729" "ping-3729") "ok"))");
    CHECK(src_send && is_bool(*src_send) && as_bool(*src_send),
          "3729 AC1: source send lands on the scope mailbox");
    auto tok_v = cs1.eval(R"((orch:agent-export-via-token "src-3729"))");
    CHECK(tok_v && is_string(*tok_v), "3729 AC1: export of scope-spawned name returns token");
    const auto idx = as_string_idx(*tok_v);
    const auto heap = cs1.evaluator().string_heap();
    CHECK(idx < heap.size() && !heap[idx].empty(), "3729 AC1: export token interned");
    const auto hash = std::string(heap[idx]);
    CHECK(cs1.evaluator().handoff_tokens_ && cs1.evaluator().handoff_tokens_->contains(hash),
          "3729 AC1: source stash holds the token");
    CHECK(!cs2.evaluator().handoff_tokens_->contains(hash),
          "3729 AC3: Evaluator-2 map does not contain Evaluator-1 hash");
    aura::orch::HandoffToken peeked;
    CHECK(cs1.evaluator().handoff_tokens_->peek(hash, peeked) && peeked.mailbox != nullptr,
          "3729 AC1: staged token carries the shared mailbox");
    auto subst_token = [&](std::string src) {
        const auto pos = src.find("TOKEN");
        CHECK(pos != std::string::npos, "3729 AC1: TOKEN placeholder present");
        src.replace(pos, 5, hash);
        return src;
    };
    auto plen = cs2.eval(subst_token(R"((string-length "TOKEN"))"));
    CHECK(plen && is_int(*plen) && as_int(*plen) == static_cast<std::int64_t>(hash.size()),
          "3729 AC1: Evaluator-2 reads the returned token string");
    auto join_cs2 = cs2.eval(subst_token(R"(
        (let ((r (orch:join-via-token "TOKEN" :timeout-ms 20)))
          (if (string=? (hash-ref r "status") "invalid") 0 1))
    )"));
    CHECK(join_cs2 && is_int(*join_cs2) && as_int(*join_cs2) == 1,
          "3729 AC1: Evaluator-2 join-via-token observes the returned string");
    // Issue #4026 (AC1): recv on an import_proxy handle is a TYPED DENY in
    // every posture (Soft+prod fail-closed, ownership) — the proxy must not
    // dual-consume the shared mailbox with the source body. The original
    // #3729 assertion ("recv succeeds on the shared mailbox") predates the
    // #4026 gate and can never pass again; retarget to the deny contract,
    // keeping the check just as strict (ok=#f, typed status, #4026 stamp).
    auto recv = cs2.eval(subst_token(R"(
        (let ((p (orch:agent-import-via-token "TOKEN")))
          (if (= (string-length p) 0)
              -1
              (let ((m (orch:agent-recv p :wait #t :timeout-ms 500)))
                (if (and (not (hash-ref m "ok"))
                         (string=? (hash-ref m "status" "") "recv-proxy-denied")
                         (= (hash-ref m "schema-4026" 0) 4026))
                    1 0))))
    )"));
    if (!(recv && is_int(*recv) && as_int(*recv) == 1)) {
        std::println("  debug hash={} plen={} join_cs2={} recv_has={} recv_val={} route={} "
                     "src_contains={}",
                     hash, plen && is_int(*plen) ? as_int(*plen) : -2,
                     join_cs2 && is_int(*join_cs2) ? as_int(*join_cs2) : -2, recv.has_value(),
                     recv && is_int(*recv) ? as_int(*recv) : -999,
                     aura::compiler::g_handoff_token_stash.find(hash) != nullptr,
                     cs1.evaluator().handoff_tokens_->contains(hash));
    }
    CHECK(recv && is_int(*recv) && as_int(*recv) == 1,
          "3729 AC1: import_proxy recv is a typed deny (#4026)");
    reset_all_agent_scopes_for_test();
}

static void ac3729_2_stash_bounded() {
    std::println("\n--- #3729 AC2: unimported export is bounded ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(true);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:spawn-agent "cap-3729") "ok"))").has_value(),
          "3729 AC2: spawn-agent");
    for (int i = 0; i < 200; ++i)
        (void)cs.eval(R"((orch:agent-export-via-token "cap-3729"))");
    CHECK(cs.evaluator().handoff_tokens_ &&
              cs.evaluator().handoff_tokens_->size() <= aura::compiler::kHandoffTokenStashCap,
          "3729 AC2: stash depth bounded (evict oldest)");
    ac3727_set_prod(false);
    reset_all_agent_scopes_for_test();
}

static void ac3729_4_join_observe_only() {
    std::println("\n--- #3729 AC4: join-via-token stays observation-only ---");
    reset_all_agent_scopes_for_test();
    ac3727_set_prod(true);
    CompilerService cs;
    CHECK(cs.eval(R"((hash-ref (orch:spawn-agent "join-3729") "ok"))").has_value(),
          "3729 AC4: spawn");
    auto r = cs.eval(R"(
        (let ((tok (orch:agent-export-via-token "join-3729")))
          (let ((h (orch:join-via-token tok :timeout-ms 20)))
            (if (and (hash-ref h "observation-only")
                     (hash-ref h "reservation-held-by-source"))
                1 0)))
    )");
    CHECK(r && is_int(*r) && as_int(*r) == 1,
          "3729 AC4: observation-only / reservation-held-by-source");
    ac3727_set_prod(false);
    reset_all_agent_scopes_for_test();
}

static void ac3729_5_soft_miss_and_source() {
    std::println("\n--- #3729 AC5: Soft miss empty string; no invent ---");
    ac3727_set_prod(false);
    CompilerService cs;
    auto miss = cs.eval(R"(
        (let ((t (orch:agent-export-via-token "no-such-3729")))
          (if (and (string? t) (= (string-length t) 0)) 1 0))
    )");
    CHECK(miss && is_int(*miss) && as_int(*miss) == 1, "3729 AC5: Soft miss stays empty string");
    const auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
    CHECK(prim.find("Issue #3729") != std::string::npos ||
              prim.find("handoff_tokens_") != std::string::npos,
          "3729 AC5: prim cites per-Evaluator stash");
    CHECK(prim.find("g_handoff_token_stash") != std::string::npos,
          "3729 AC5: route table name retained (not AgentRegistry)");
    CHECK(prim.find("class AgentRegistry") == std::string::npos, "3729 AC5: no AgentRegistry");
    CHECK(prim.find("query:3729") == std::string::npos, "3729 AC5: no new query key");
    CHECK(read_file("tests/orch/test_issue_3729.cpp").empty(), "3729 AC5: no test_issue_3729.cpp");
    CHECK(read_file("docs/design/3729-handoff-stash.md").empty(), "3729 AC5: no docs/design");
}

} // namespace

// ── Issue #3944: production put() sweeps cross-name Done husks ──
// A fully-clean slot (flags false, fiber null, reserved 0) is a Done husk
// per slot_is_reclaimable_clean. Long-run unique-name spawn+join left
// such husks in the table until ~Evaluator; the put-path sweep (#3944)
// erases them cross-name under production. Soft keeps them (zero cost).
// Issue #4280: bare orch:spawn-agent shares the #4238 region-key admit
// deny. Production + ≥2 live name-table mutate slots with <2 distinct
// non-zero keys → typed deny, no put. Soft admits. Distinct keys admit.
static void ac4280_name_table_region_key_admit() {
    std::println("\n--- #4280 AC1–AC6: name-table region-key-missing admit deny ---");
    struct RkeysGuard {
        bool had = false;
        std::string prev;
        RkeysGuard() {
            const char* e = std::getenv("AURA_PARALLEL_REQUIRE_REGION_KEYS");
            had = e != nullptr;
            if (had)
                prev = e;
        }
        ~RkeysGuard() {
            if (had)
                ::setenv("AURA_PARALLEL_REQUIRE_REGION_KEYS", prev.c_str(), 1);
            else
                ::unsetenv("AURA_PARALLEL_REQUIRE_REGION_KEYS");
        }
    } rkeys_guard;
    (void)rkeys_guard;
    auto miss_total = [] {
        return aura::serve::parallel_orch::g_parallel_orch_stats.region_key_missing_serialized_total
            .load(std::memory_order_relaxed);
    };

    // AC1: Soft — N keyless spawn-agent still admit. Counter untouched.
    {
        reset_all_agent_scopes_for_test();
        ac3727_set_prod(false);
        ::unsetenv("AURA_PARALLEL_REQUIRE_REGION_KEYS");
        CompilerService cs;
        const auto m0 = miss_total();
        auto a = cs.eval(R"((hash-ref (orch:spawn-agent "4280-soft-a" (lambda () 1)) "ok"))");
        auto b = cs.eval(R"((hash-ref (orch:spawn-agent "4280-soft-b" (lambda () 1)) "ok"))");
        CHECK(a && is_bool(*a) && as_bool(*a), "4280 AC1: Soft keyless pair admits");
        CHECK(b && is_bool(*b) && as_bool(*b), "4280 AC1: Soft second keyless spawn admits");
        CHECK(cs.evaluator().agent_names_->find("4280-soft-a") != nullptr, "4280 AC1: first put");
        CHECK(cs.evaluator().agent_names_->find("4280-soft-b") != nullptr, "4280 AC1: second put");
        CHECK(miss_total() == m0, "4280 AC1: deny counter untouched in Soft");
        (void)cs.eval(R"((orch:agent-join "4280-soft-a" :timeout-ms 2000))");
        (void)cs.eval(R"((orch:agent-join "4280-soft-b" :timeout-ms 2000))");
        reset_all_agent_scopes_for_test();
    }

    // AC2: production + second keyless name → typed deny, no put.
    {
        reset_all_agent_scopes_for_test();
        ac3727_set_prod(true);
        ::unsetenv("AURA_PARALLEL_REQUIRE_REGION_KEYS");
        CompilerService cs;
        const auto m0 = miss_total();
        auto r = cs.eval(R"ac2(
            (begin
              (orch:spawn-agent "4280-prod-a" (lambda () 1))
              (let ((d (orch:spawn-agent "4280-prod-b" (lambda () 1))))
                (let ((err (hash-ref d "error" "")))
                  (orch:agent-join "4280-prod-a" :timeout-ms 2000)
                  (if (and (not (hash-ref d "ok"))
                           (= (hash-ref d "id") 0)
                           (string=? (hash-ref d "deny-detail" "")
                                     "missing-or-overlap-keys")
                           (string=? (substring err
                                                (- (string-length err) 7)
                                                (string-length err))
                                     "(#4280)"))
                      1 0))))
        )ac2");
        CHECK(r && is_int(*r) && as_int(*r) == 1,
              "4280 AC2: second keyless spawn ok=#f + missing-or-overlap-keys");
        CHECK(cs.evaluator().agent_names_->find("4280-prod-b") == nullptr,
              "4280 AC2: denied spawn is not put");
        CHECK(miss_total() == m0 + 1, "4280 AC2: region_key_missing_serialized_total +1");
        ac3727_set_prod(false);
        reset_all_agent_scopes_for_test();
    }

    // AC3: AURA_PARALLEL_REQUIRE_REGION_KEYS=0 admits the second keyless spawn.
    {
        reset_all_agent_scopes_for_test();
        ac3727_set_prod(true);
        ::setenv("AURA_PARALLEL_REQUIRE_REGION_KEYS", "0", 1);
        CompilerService cs;
        const auto m0 = miss_total();
        auto a = cs.eval(R"((hash-ref (orch:spawn-agent "4280-esc-a" (lambda () 1)) "ok"))");
        auto b = cs.eval(R"((hash-ref (orch:spawn-agent "4280-esc-b" (lambda () 1)) "ok"))");
        CHECK(a && is_bool(*a) && as_bool(*a), "4280 AC3: env=0 admits the keyless pair");
        CHECK(b && is_bool(*b) && as_bool(*b), "4280 AC3: env=0 second keyless spawn admits");
        CHECK(miss_total() == m0, "4280 AC3: escape does not bump the deny counter");
        (void)cs.eval(R"((orch:agent-join "4280-esc-a" :timeout-ms 2000))");
        (void)cs.eval(R"((orch:agent-join "4280-esc-b" :timeout-ms 2000))");
        ::unsetenv("AURA_PARALLEL_REQUIRE_REGION_KEYS");
        ac3727_set_prod(false);
        reset_all_agent_scopes_for_test();
    }

    // AC4: distinct non-zero keys admit. Same-name replace of the only
    // live slot also admits (exclude_name — one agent, not two).
    {
        reset_all_agent_scopes_for_test();
        ac3727_set_prod(true);
        ::unsetenv("AURA_PARALLEL_REQUIRE_REGION_KEYS");
        CompilerService cs;
        const auto m0 = miss_total();
        auto a = cs.eval(
            R"((hash-ref (orch:spawn-agent "4280-key-a" (lambda () 1) :region-key 11) "ok"))");
        auto b = cs.eval(
            R"((hash-ref (orch:spawn-agent "4280-key-b" (lambda () 1) :region-key 22) "ok"))");
        CHECK(a && is_bool(*a) && as_bool(*a), "4280 AC4: distinct keys admit");
        CHECK(b && is_bool(*b) && as_bool(*b), "4280 AC4: second distinct key admits");
        CHECK(miss_total() == m0, "4280 AC4: distinct keys do not deny");
        // Same-name respawn is one agent: the walk drops that name. A
        // completed production slot is #3467, so this is the table helper.
        {
            AgentNameTable table;
            auto only = make_minimal_handle("only-4280", 71);
            only.region_key = 7;
            CHECK(table.put(std::move(only)) != nullptr, "4280 AC4: helper slot put");
            std::vector<std::uint64_t> excluded;
            table.append_live_region_keys(excluded, "only-4280");
            CHECK(excluded.empty(), "4280 AC4: same-name replace is not a second agent");
            std::vector<std::uint64_t> kept;
            table.append_live_region_keys(kept, "other-4280");
            CHECK(kept.size() == 1 && kept[0] == 7, "4280 AC4: other live keys stay");
        }
        (void)cs.eval(R"((orch:agent-join "4280-key-a" :timeout-ms 2000))");
        (void)cs.eval(R"((orch:agent-join "4280-key-b" :timeout-ms 2000))");
        ac3727_set_prod(false);
        reset_all_agent_scopes_for_test();
    }

    // AC5: Scope #4238 path still denies a second keyless scope-spawn.
    {
        reset_all_agent_scopes_for_test();
        ac3727_set_prod(true);
        ::unsetenv("AURA_PARALLEL_REQUIRE_REGION_KEYS");
        CompilerService cs;
        auto r = cs.eval(R"(
            (begin
              (orch:scope-spawn "4280-scope-a")
              (let ((h (orch:scope-spawn "4280-scope-b")))
                (if (not (hash-ref h "ok")) 1 0)))
        )");
        CHECK(r && is_int(*r) && as_int(*r) == 1, "4280 AC5: Scope #4238 deny unchanged");
        (void)cs.eval(R"((orch:scope-cancel-all))");
        (void)cs.eval(R"((orch:scope-join-all :timeout-ms 800))");
        ac3727_set_prod(false);
        reset_all_agent_scopes_for_test();
    }

    // AC6: source cite. No new query key, no AgentRegistry, no key synthesis.
    {
        ac3727_set_prod(false);
        const auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(prim.find("Issue #4280") != std::string::npos, "4280 AC6: prim cites #4280");
        CHECK(prim.find("append_live_region_keys") != std::string::npos,
              "4280 AC6: name-table key walk");
        CHECK(prim.find("decide_isolation") != std::string::npos,
              "4280 AC6: decide_isolation SSOT");
        CHECK(prim.find("region_key_missing_serialized") != std::string::npos,
              "4280 AC6: #3243 predicate reused");
        CHECK(prim.find("parallel_require_region_keys_deny") != std::string::npos,
              "4280 AC6: #3353 deny face reused");
        CHECK(prim.find("region-key-missing spawn deny (#4280)") != std::string::npos,
              "4280 AC6: typed error");
        CHECK(prim.find("class AgentRegistry") == std::string::npos, "4280 AC6: no AgentRegistry");
        CHECK(prim.find("auto_region_key") == std::string::npos, "4280 AC6: keys never invented");
        CHECK(prim.find("query:4280") == std::string::npos, "4280 AC6: no new query key");
        const auto spawn = read_file("src/orch/agent_spawn.h");
        CHECK(spawn.find("h.region_key = spec.region_key") != std::string::npos,
              "4280 AC6: handle stamps region_key");
        const auto table = read_file("src/compiler/agent_name_table.h");
        CHECK(table.find("append_live_region_keys") != std::string::npos, "4280 AC6: table helper");
        const auto scope = read_file("src/orch/agent_scope.h");
        CHECK(scope.find("region_key_missing_admit_deny_unlocked_") != std::string::npos,
              "4280 AC6: Scope #4238 gate remains");
        CHECK(read_file("tests/orch/test_issue_4280.cpp").empty(),
              "4280 AC6: no test_issue_4280.cpp");
        CHECK(read_file("docs/design/4280-name-table-region-admit.md").empty(),
              "4280 AC6: no docs/design/4280-*");
        CHECK(read_file("build.py").find("check_name_table_region_admit_4280") != std::string::npos,
              "4280 AC6: build.py wires the linter");
        CHECK(read_file("scripts/coverage/root_check_allowlist.txt")
                      .find("check_name_table_region_admit_4280.py") != std::string::npos,
              "4280 AC6: allowlist entry");
    }
}

static void ac3944_1_prod_sweeps_cross_name_husks() {
    std::println("\n--- #3944: cross-name Done-husk sweep (production) ---");
    ac3727_set_prod(true);
    {
        AgentNameTable table;
        AgentHandle husk;
        husk.id = 1;
        husk.name = "husk-3944";
        husk.ok = true;
        table.put(std::move(husk));
        AgentHandle live;
        live.id = 2;
        live.name = "live-3944";
        live.ok = true;
        live.reserved_memory_bytes = 64; // non-clean slot must survive
        table.put(std::move(live));
        CHECK(table.find("husk-3944") == nullptr,
              "3944 AC1: cross-name Done husk swept by the next put");
        CHECK(table.find("live-3944") != nullptr,
              "3944 AC1: non-clean different-name slot survives");
        // Same-name retire path unchanged: clean same-name put still
        // fresh-inserts (#3598) — the sweep excludes the incoming name.
        AgentHandle same1;
        same1.id = 3;
        same1.name = "same-3944";
        same1.ok = true;
        table.put(std::move(same1));
        AgentHandle same2;
        same2.id = 4;
        same2.name = "same-3944";
        same2.ok = true;
        auto* slot = table.put(std::move(same2));
        CHECK(slot != nullptr && slot->id == 4,
              "3944 AC1: same-name put still fresh-inserts over the husk");
    }
    // Soft: sweep skipped — husks stay (zero cost, directory unchanged).
    ac3727_set_prod(false);
    {
        AgentNameTable table;
        AgentHandle husk;
        husk.id = 5;
        husk.name = "soft-husk-3944";
        husk.ok = true;
        table.put(std::move(husk));
        AgentHandle live;
        live.id = 6;
        live.name = "soft-live-3944";
        live.ok = true;
        live.reserved_memory_bytes = 64;
        table.put(std::move(live));
        // Probe with size(), NOT find(): find() itself retires a clean
        // slot per the #3598 contract — the probe must not disturb the
        // state it asserts.
        CHECK(!aura::compiler::typed_audit::production_defaults_active(),
              "3944 AC2: Soft face latched");
        CHECK(table.size() == 2, "3944 AC2: Soft keeps Done husks (sweep skipped)");
    }
    ac3727_set_prod(false);
    const auto src = read_file("src/compiler/agent_name_table.h");
    CHECK(src.find("Issue #3944") != std::string::npos, "3944 AC3: header cites #3944");
    CHECK(src.find("production_defaults_active()") != std::string::npos,
          "3944 AC3: sweep is production-gated");
}

// Issue #4393: production same-name spawn over a still-running body
// fails closed before a second fiber. Soft still replaces / appends.
// Pending deny (#3467/#3497) and abandon retire (#3805) stay as they are.
static void ac4393_live_name_reuse() {
    using aura::orch::AgentDenyClass;
    using aura::orch::AgentScope;
    using aura::orch::AgentSpec;
    using aura::orch::JoinPolicy;
    using aura::serve::Scheduler;
    std::println("\n--- #4393: live same-name spawn fails closed ---");

    auto active = [] {
        return aura::orch::g_orch_module_stats.agents_active.load(std::memory_order_relaxed);
    };
    auto stop_handle = [](aura::orch::AgentHandle& hh) {
        if (!hh.fiber)
            return;
        hh.fiber->request_cancel();
        JoinPolicy policy;
        policy.primary_ms = 2000;
        policy.drain_ms = 200;
        (void)aura::orch::join_agent(hh, policy);
    };
    auto sleeping = [](const char* name) {
        AgentSpec spec;
        spec.name = name;
        spec.body = [] { aura::orch::fiber_sleep_ms(5000); };
        return spec;
    };
    // Workers start inside Scheduler::run. Without the IO thread a spawned
    // fiber stays !is_done forever, join waits out the timeout, and
    // on_fiber_done never consumes the transferred agents_active one-shot.
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

    // AC1: production put does not move-assign over !is_done. The refused
    // spawn is cancelled and its agents_active one-shot is transferred.
    {
        ac3727_set_prod(true);
        Scheduler sched(2);
        SchedRunner runner(sched);
        AgentNameTable table;
        auto first = aura::orch::spawn_agent_with_mailbox(sched, sleeping("put-4393"));
        CHECK(first.ok && first.fiber && !first.fiber->is_done(), "4393 AC1: first spawn live");
        const auto id0 = first.id;
        const auto res0 = first.reserved_memory_bytes;
        CHECK(table.put(std::move(first)) != nullptr, "4393 AC1: first put");
        const auto n_active = active();
        auto second = aura::orch::spawn_agent_with_mailbox(sched, sleeping("put-4393"));
        CHECK(second.ok && second.fiber, "4393 AC1: second spawn exists for the belt");
        CHECK(active() == n_active + 1, "4393 AC1: belt spawn did bump agents_active");
        CHECK(table.put(std::move(second)) == nullptr, "4393 AC1: put over live returns nullptr");
        aura::orch::release_refused_spawn(second);
        CHECK(!second.agents_active_held, "4393 AC1: handle gave up the agents_active one-shot");
        CHECK(second.fiber && second.fiber->is_cancel_requested(),
              "4393 AC1: refused body cancelled");
        auto* slot = table.find("put-4393");
        CHECK(slot && slot->id == id0, "4393 AC1: occupant id unchanged");
        CHECK(slot && slot->reserved_memory_bytes == res0,
              "4393 AC1: occupant reservation untouched");
        CHECK(table.size() == 1, "4393 AC1: table size stays 1");
        if (second.fiber)
            (void)aura::serve::Fiber::join(second.fiber, std::optional<std::uint64_t>{2000});
        for (int i = 0; i < 50 && active() != n_active; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(active() == n_active, "4393 AC1: refused spawn released its agents_active");
        if (slot)
            stop_handle(*slot);
        ac3727_set_prod(false);
    }

    // AC2: AgentScope::spawn denies before emplace. Directory stays one row.
    {
        ac3727_set_prod(true);
        Scheduler sched(2);
        SchedRunner runner(sched);
        AgentScope scope(sched);
        auto& h1 = scope.spawn(sleeping("scope-4393"));
        CHECK(h1.ok && h1.fiber && !h1.fiber->is_done(), "4393 AC2: scope spawn live");
        const auto id0 = h1.id;
        const auto res0 = h1.reserved_memory_bytes;
        const auto n0 = scope.size();
        const auto n_active = active();
        auto& h2 = scope.spawn(sleeping("scope-4393"));
        CHECK(!h2.ok && h2.id == 0, "4393 AC2: second spawn ok=#f id=0");
        CHECK(h2.deny_class == AgentDenyClass::Other, "4393 AC2: deny-class other");
        CHECK(h2.quota_dimension == "name-reuse-while-live", "4393 AC2: deny detail");
        CHECK(h2.error.find("name-reuse-while-live") != std::string::npos, "4393 AC2: error");
        CHECK(scope.size() == n0, "4393 AC2: handles_ unchanged");
        CHECK(scope.handles()[0].id == id0, "4393 AC2: first id stays");
        CHECK(scope.handles()[0].reserved_memory_bytes == res0, "4393 AC2: reservation untouched");
        CHECK(active() == n_active, "4393 AC2: no agents_active bump");
        auto snap = scope.directory_snapshot({});
        CHECK(snap.entries.size() == 1, "4393 AC2: directory one row");
        CHECK(scope.find("scope-4393") && scope.find("scope-4393")->id == id0,
              "4393 AC2: resolve is the first fiber");
        stop_handle(scope.handles_mut()[0]);
        ac3727_set_prod(false);
    }

    // AC3: Soft still appends a live namesake.
    {
        ac3727_set_prod(false);
        Scheduler sched(2);
        SchedRunner runner(sched);
        AgentScope scope(sched);
        auto& h1 = scope.spawn(sleeping("soft-4393"));
        CHECK(h1.ok, "4393 AC3: soft first ok");
        const auto id1 = h1.id;
        auto& h2 = scope.spawn(sleeping("soft-4393"));
        CHECK(h2.ok && h2.id != id1, "4393 AC3: soft still appends");
        CHECK(scope.size() == 2, "4393 AC3: soft size 2");
        auto hs = scope.handles_mut();
        stop_handle(hs[0]);
        stop_handle(hs[1]);
    }

    // AC4: orch:spawn-agent / orch:scope-spawn typed deny, id 0, no bump.
    {
        reset_all_agent_scopes_for_test();
        ac3727_set_prod(true);
        Scheduler sched(2);
        SchedRunner runner(sched);
        CompilerService cs;
        const auto n_active = active();
        auto seeded = aura::orch::spawn_agent_with_mailbox(sched, sleeping("w4393"));
        CHECK(seeded.ok && seeded.fiber && !seeded.fiber->is_done(), "4393 AC4: seeded live");
        const auto id0 = seeded.id;
        CHECK(cs.evaluator().agent_names_->put(std::move(seeded)) != nullptr, "4393 AC4: seed put");
        const auto after_seed = active();
        CHECK(after_seed == n_active + 1, "4393 AC4: seed bumped once");
        auto denied = cs.eval(R"(
            (let ((d (orch:spawn-agent "w4393" (lambda () 1))))
              (and (not (hash-ref d "ok"))
                   (= (hash-ref d "id") 0)
                   (string=? (hash-ref d "deny-class" "") "other")
                   (string=? (hash-ref d "deny-detail" "") "name-reuse-while-live")))
        )");
        CHECK(denied && is_bool(*denied) && as_bool(*denied),
              "4393 AC4: spawn-agent ok=#f id=0 deny-detail name-reuse-while-live");
        CHECK(active() == after_seed, "4393 AC4: spawn-agent did not bump agents_active");
        auto* slot = cs.evaluator().agent_names_->find("w4393");
        CHECK(slot && slot->id == id0, "4393 AC4: name resolve still the first fiber");

        auto& scope =
            aura::orch::get_or_create_agent_scope(static_cast<void*>(&cs.evaluator()), sched);
        auto& sh = scope.spawn(sleeping("s4393"));
        CHECK(sh.ok && sh.fiber && !sh.fiber->is_done(), "4393 AC4: scope seed live");
        const auto sid = sh.id;
        const auto scope_active = active();
        auto sdenied = cs.eval(R"(
            (let ((d (orch:scope-spawn "s4393" (lambda () 1))))
              (and (not (hash-ref d "ok"))
                   (= (hash-ref d "id") 0)
                   (string=? (hash-ref d "deny-class" "") "other")
                   (string=? (hash-ref d "deny-detail" "") "name-reuse-while-live")))
        )");
        CHECK(sdenied && is_bool(*sdenied) && as_bool(*sdenied),
              "4393 AC4: scope-spawn ok=#f id=0 deny-detail name-reuse-while-live");
        CHECK(active() == scope_active, "4393 AC4: scope-spawn did not bump agents_active");
        CHECK(scope.size() == 1, "4393 AC4: scope size stays 1");
        auto resolved = cs.eval(R"((hash-ref (orch:scope-resolve "s4393") "id"))");
        CHECK(resolved && is_int(*resolved) && as_int(*resolved) == static_cast<std::int64_t>(sid),
              "4393 AC4: scope-resolve is the first id");
        auto rows = cs.eval(R"((hash-ref (orch:agent-directory) "count"))");
        CHECK(rows && is_int(*rows) && as_int(*rows) == 1, "4393 AC4: directory one alive row");
        if (slot)
            stop_handle(*slot);
        stop_handle(scope.handles_mut()[0]);
        reset_all_agent_scopes_for_test();
        ac3727_set_prod(false);
    }

    // AC5: source cite. No new query key, no invented test/docs file.
    {
        const auto prim = read_file("src/compiler/evaluator_primitives_agent.cpp");
        const auto table = read_file("src/compiler/agent_name_table.h");
        const auto scope = read_file("src/orch/agent_scope.h");
        const auto spawn = read_file("src/orch/agent_spawn.h");
        CHECK(prim.find("name-reuse-while-live") != std::string::npos, "4393 AC5: spawn detail");
        CHECK(prim.find("Issue #4393") != std::string::npos, "4393 AC5: prim cite");
        CHECK(table.find("slot_is_live_running") != std::string::npos, "4393 AC5: put belt");
        CHECK(table.find("Issue #4393") != std::string::npos, "4393 AC5: table cite");
        CHECK(scope.find("name-reuse-while-live") != std::string::npos, "4393 AC5: scope detail");
        CHECK(spawn.find("release_refused_spawn") != std::string::npos, "4393 AC5: refused spawn");
        CHECK(prim.find("query:4393") == std::string::npos, "4393 AC5: no new query key");
        CHECK(read_file("tests/orch/test_issue_4393.cpp").empty(), "4393 AC5: no test_issue_4393");
        CHECK(read_file("docs/design/4393-live-name-reuse.md").empty(), "4393 AC5: no design doc");
    }
}

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
    ac3598_clean_slot_retire();
    ac3727_1_scope_spawn_denies_pending_name();
    ac3727_2_after_cleanup_scope_spawn_and_send();
    ac3727_3_touch_poll_export_scope_name();
    ac3727_4_directory_scope_only();
    ac3727_5_soft_no_extra_deny_and_source();
    ac3925_1_prod_denies_live_name_table();
    ac3925_2_directory_name_table_count();
    ac3925_5_soft_and_source();
    ac3937_1_prod_denies_live_scope();
    ac3937_5_soft_and_source();
    ac4393_live_name_reuse();
    ac3729_1_scope_export_import_recv();
    ac3729_2_stash_bounded();
    ac3729_4_join_observe_only();
    ac3729_5_soft_miss_and_source();
    ac3944_1_prod_sweeps_cross_name_husks();
    ac4280_name_table_region_key_admit();
    std::println(
        "\n=== #2078/#3125/#3442/#3467/#3598/#3727/#3729/#3944/#4280: passed={} failed={} ===",
        g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_name_table_isolation();
}
#endif
