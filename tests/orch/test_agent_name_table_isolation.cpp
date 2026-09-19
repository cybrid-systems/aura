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
    auto r = cs.eval(R"((hash-ref (orch:scope-spawn "soft-3727") "ok"))");
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
    auto peer = cs.eval(R"((hash-ref (orch:scope-spawn "peer-3727") "ok"))");
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
    auto recv = cs2.eval(subst_token(R"(
        (let ((p (orch:agent-import-via-token "TOKEN")))
          (if (= (string-length p) 0)
              -1
              (let ((m (orch:agent-recv p :wait #t :timeout-ms 500)))
                (if (and (hash-ref m "ok")
                         (string=? (hash-ref m "payload" "") "ping-3729"))
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
          "3729 AC1: import on Evaluator-2 recvs the shared mailbox");
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
    ac3729_1_scope_export_import_recv();
    ac3729_2_stash_bounded();
    ac3729_4_join_observe_only();
    ac3729_5_soft_miss_and_source();
    std::println("\n=== #2078/#3125/#3442/#3467/#3598/#3727/#3729: passed={} failed={} ===",
                 g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_agent_name_table_isolation();
}
#endif
