// tests/orch/test_scope_join_tree_visibility.cpp
// @category: integration
// @reason: Issue #3671 — orch:scope-join-all computed tree_settled for the
//          #3496 drop gate and threw it away: the hash said ok=#t while
//          descendants still lived (three-plane collision with directory /
//          scope-resolve). The hash now carries tree-settled /
//          descendants-live blame fields, and production + !tree + live
//          descendants fail-closed (ok=#f, deny-class=other /
//          deny-detail=descendants-live, #3251 intern). Soft/Off is
//          observe-only (bools populate, no deny intern). C++ join_all
//          default stays local (#3496 AC1 unchanged); drop gate stays
//          settled-only.
//
// Member placement: this is a run_test_* member of test_orch_agent_batch.
// It deliberately does NOT live in test_orch_scope.cpp — that member is
// skip-listed by the batch driver (in-process hang history) and its checks
// would never execute.
//
// Liveness note: the AC1 descendant is driven through the C++ AgentScope
// API (spawn_child + a yield-spinning body behind a hold flag) because the
// language spawn face has no long-lived body primitive; empty-language
// bodies complete before the join window opens. The surface under test is
// still the language orch:scope-join-all deny path.
//
// Source-cite (issue #3671):
//   - src/compiler/evaluator_primitives_agent.cpp: orch:scope-join-all
//     tree_settled_now / descendants_live / join_guard_deny + deny intern
//     (by-value add_deny_class capture — a reference capture of the
//     register-scope local dangles after registration returns).
//   - scripts/check_scope_join_tree_visibility_3671.py: ship linter.
//
// No docs/design/ per #1655 / #1485.

#include "test_harness.hpp"

#include "compiler/typed_mutation_audit.h"
#include "orch/agent_scope.h"
#include "orch/agent_spawn.h"
#include "serve/fiber.h"

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <print>
#include <string>
#include <thread>

import std;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
using aura::orch::find_agent_scope;
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

int run_test_scope_join_tree_visibility() {
    // ── Issue #3671: join-all publishes tree_settled; production
    // fail-closed on non-tree joins with live descendants ──
    std::atomic<bool> hold{true};
    {
        std::println("\n--- #3671: join-all tree visibility + descendants-live guard ---");
        reset_all_agent_scopes_for_test();
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CompilerService cs;

        // Root scope via language, then a live descendant driven from C++:
        // child scope + yield-spinning body held open by `hold`.
        CHECK(
            cs.eval(R"((let ((r (orch:scope-spawn "3671-root"))) (hash-ref r "ok")))").has_value(),
            "3671: root spawn");
        auto* root = find_agent_scope(static_cast<void*>(&cs.evaluator()));
        CHECK(root != nullptr, "3671: root scope resolvable");
        if (root) {
            auto& child = root->spawn_child();
            child.spawn({.name = std::format("3671-child-agent"), .body = [&] {
                             while (hold.load(std::memory_order_relaxed)) {
                                 if (aura::serve::g_current_fiber &&
                                     aura::serve::g_current_fiber->is_cancel_requested())
                                     break;
                                 aura::serve::Fiber::yield(aura::serve::YieldReason::Explicit);
                             }
                         }});
            CHECK(child.size() == 1, "3671: live descendant in child scope");
        }

        // AC1: local join (no :tree) under production — the root handle
        // joins but the descendant stays live: ok=#f, blame fields
        // populated, deny interned (#3251), and NO drop (#3496 AC1).
        const auto ok1 = cs.eval(R"((hash-ref (orch:scope-join-all :timeout-ms 200) "ok"))");
        CHECK(ok1 && is_bool(*ok1) && !as_bool(*ok1),
              "3671 AC1: ok=#f while descendants live (production, no :tree)");
        const auto settled1 =
            cs.eval(R"((hash-ref (orch:scope-join-all :timeout-ms 200) "tree-settled"))");
        CHECK(settled1 && is_bool(*settled1) && !as_bool(*settled1),
              "3671 AC1: tree-settled=#f (descendant live)");
        const auto desc1 =
            cs.eval(R"((hash-ref (orch:scope-join-all :timeout-ms 200) "descendants-live"))");
        CHECK(desc1 && is_bool(*desc1) && as_bool(*desc1), "3671 AC1: descendants-live=#t");
        const auto deny1 = cs.eval(
            R"((let ((r (orch:scope-join-all :timeout-ms 200))) (hash-ref r "schema-3251")))");
        CHECK(deny1 && is_int(*deny1) && as_int(*deny1) == 3251,
              "3671 AC1: deny-class interned (#3251 — descendants-live deny fired)");
        // No drop: the child scope still resolves (spawn into :path 0 again).
        CHECK(
            cs.eval(
                  R"((let ((r (orch:scope-spawn :path 0 "3671-child-agent-2"))) (hash-ref r "ok")))")
                .has_value(),
            "3671 AC1: no drop_agent_scope — child scope still live (#3496)");

        // AC2: :tree #t still folds the subtree (#3643 preserved). The
        // held descendant makes the subtree join wait to its timeout — the
        // "tree" flag row is present regardless of the aggregate status.
        const auto tree_flag =
            cs.eval(R"((hash-ref (orch:scope-join-all :tree #t :timeout-ms 200) "tree"))");
        CHECK(tree_flag && is_bool(*tree_flag) && as_bool(*tree_flag),
              "3671 AC2: tree join flag propagates");

        // Drain: release the held descendant, cancel, and tree-join so the
        // spinner exits before AC3 resets the tree.
        hold.store(false, std::memory_order_relaxed);
        CHECK(cs.eval(R"((orch:scope-cancel-all))").has_value(), "3671: cancel-all for drain");
        CHECK(cs.eval(R"((orch:scope-join-all :tree #t :timeout-ms 5000))").has_value(),
              "3671: tree join drains held descendant");
    }

    // ── AC3: fresh root with no children — settled tree, no deny. The
    // join drops the settled scope (#3496 AC2); the returned hash still
    // carries the fields. No cancel: the empty body completes → Ok. ──
    {
        reset_all_agent_scopes_for_test();
        CompilerService cs3;
        CHECK(cs3.eval(R"((let ((r (orch:scope-spawn "3671-root-3"))) (hash-ref r "ok")))")
                  .has_value(),
              "3671 AC3: fresh root spawn");
        const auto j3 = cs3.eval(
            R"((let ((r (orch:scope-join-all :timeout-ms 1000)))
               (list (hash-ref r "tree-settled")
                     (hash-ref r "descendants-live")
                     (hash-ref r "deny-detail" "")
                     (hash-ref r "ok"))))");
        CHECK(j3.has_value(), "3671 AC3: join-all hash readable");
        {
            // List walk: (tree-settled descendants-live deny-detail ok).
            using aura::compiler::types::as_pair_idx;
            using aura::compiler::types::as_string_idx;
            using aura::compiler::types::is_pair;
            using aura::compiler::types::is_string;
            auto& ev3 = cs3.evaluator();
            auto cur = *j3;
            int guard = 0;
            int seen = 0;
            bool tree_settled_v = false, descendants_v = false, ok_v = false;
            std::string deny_v = "unset";
            auto& pairs = ev3.pairs();
            auto heap = ev3.string_heap();
            while (is_pair(cur) && guard++ < 16) {
                const auto i = as_pair_idx(cur);
                if (i >= pairs.size())
                    break;
                const auto& car = pairs[i].car;
                if (is_bool(car)) {
                    ++seen;
                    if (seen == 1)
                        tree_settled_v = as_bool(car);
                    if (seen == 2)
                        descendants_v = as_bool(car);
                    if (seen == 4)
                        ok_v = as_bool(car);
                } else if (is_string(car)) {
                    ++seen;
                    const auto si = as_string_idx(car);
                    if (si < heap.size())
                        deny_v = std::string(heap[si]);
                }
                cur = pairs[i].cdr;
            }
            CHECK(seen == 4, "3671 AC3: all four hash fields readable");
            CHECK(tree_settled_v, "3671 AC3: tree-settled=#t (no children)");
            CHECK(!descendants_v, "3671 AC3: descendants-live=#f");
            CHECK(deny_v.empty(), "3671 AC3: no deny-detail (no deny interned)");
            CHECK(ok_v, "3671 AC3: ok=#t (settled join, no deny)");
        }
    }
    // ── Issue #4725 / #4726 / #4724: scope waiter wake + cancel budget ──
    {
        std::println("\n--- #4725 AC1: yield waiters survive orch:supervise-batch ---");
        reset_all_agent_scopes_for_test();
        // The reported repro runs on the Soft face; the production
        // region-key admit gate (#4238) would reject six unkeyed scope
        // spawns before the waiter wake is exercised. Dev defaults here,
        // production restored by the member tail (#3671 AC1-AC3 above
        // already pinned the production face).
        aura::compiler::typed_audit::apply_dev_audit_defaults();
        CompilerService cs;
        // Repro shape from the issue: six scope tasks cooperatively wait on a
        // parent flag (fiber:yield), spawned AFTER a batch/workflow run. The
        // first K (= batch size) used to be silently dropped: the spawn site
        // released the body closure temporary, no fiber root segment
        // referenced it, and the #4713 tree-walker reclaim erased it before
        // the worker fiber reached apply_spawn_closure_maybe_locked
        // (agent_cid_live() false -> body never ran) while join-all still
        // reported ok. #4725 AC2 pins the closure from spawn until apply.
        const std::string waiter_prog = R"AURA((define BB (hash))
(define (bb! k v) (hash-set! BB k v) v)
(define (bbg k) (if (hash-has-key? BB k) (hash-ref BB k) #f))
(define (st n) (if (equal? (bbg n) "done") "d" "x"))
(define (mk n)
  (orch:scope-spawn (string-append "w" n)
    (lambda ()
      (let wait ((i 0))
        (cond ((equal? (bbg "go") "go") (bb! n "done"))
              ((> i 200000) (bb! n "aborted"))
              (else (fiber:yield) (wait (+ i 1))))))))
__PREFIX__
(for-each mk (list "1" "2" "3" "4" "5" "6"))
(bb! "go" "go")
(define j (orch:scope-join-all :timeout-ms 10000))
(string-append (hash-ref j "status") ":" (st "1") (st "2") (st "3") (st "4") (st "5") (st "6"))
)AURA";
        auto run_waiters = [&](std::string_view prefix) -> std::string {
            std::string prog = waiter_prog;
            const auto marker = prog.find("__PREFIX__");
            if (marker == std::string::npos)
                return {};
            prog.replace(marker, std::string("__PREFIX__").size(), prefix);
            auto r = cs.eval(prog);
            if (!r || !is_string(*r))
                return {};
            const auto idx = as_string_idx(*r);
            const auto heap = cs.evaluator().string_heap();
            if (idx >= heap.size())
                return {};
            return std::string(heap[idx]);
        };
        const std::string supervise_prefix =
            "(orch:supervise-batch (list (lambda () 1) (lambda () 2)) "
            "(orch:compose-workflow (quote collect-all) :max-retries 0) :watch-scope #f)";
        int supervised_ok = 0;
        for (int run = 0; run < 5; ++run) {
            const auto got = run_waiters(supervise_prefix);
            if (got == "ok:dddddd")
                ++supervised_ok;
            else
                std::println("  #4725 run {} -> {}", run, got);
        }
        CHECK(supervised_ok == 5,
              "4725 AC1: all six yield waiters ran after supervise-batch (5/5 runs)");

        std::println("\n--- #4726 AC1: yield waiters survive orch:run-workflow ---");
        // Same contract on the workflow entry (the issue saw it 1-in-4 as a
        // join timeout with no waiter body ever running). Five runs pin the
        // deterministic post-fix behavior.
        const std::string workflow_prefix =
            "(orch:run-workflow (vector (hash \"tasks\" (vector (lambda () 1) (lambda () 2))) "
            "(hash \"tasks\" (vector (lambda () 3)))))";
        int workflow_ok = 0;
        for (int run = 0; run < 5; ++run) {
            const auto got = run_waiters(workflow_prefix);
            if (got == "ok:dddddd")
                ++workflow_ok;
            else
                std::println("  #4726 run {} -> {}", run, got);
        }
        CHECK(workflow_ok == 5,
              "4726 AC1: all six yield waiters ran after run-workflow (5/5 runs)");

        std::println("\n--- #4725 AC2: pending-body closure pin (source-cite) ---");
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        const auto prim_src_scope = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(spawn_src.find("aura_fiber_root_pending_body(pin_ev, pin_cid, pin_token)") !=
                  std::string::npos,
              "4725 AC2: spawn roots the pending body closure before any admit allocation");
        CHECK(spawn_src.find("aura_fiber_release_pending_body(ev, token)") != std::string::npos,
              "4725 AC2: body guard releases the pending-body pin after apply");
        CHECK(spawn_src.find("pending_body_pin.handed_off = true;") != std::string::npos,
              "4725 AC2: spawn hands the pin to the body (deny paths release it)");
        CHECK(spawn_src.find("std::uint64_t body_closure_id = 0;") != std::string::npos,
              "4725 AC2: AgentSpec carries the body closure id");
        CHECK(prim_src_scope.find("spec.body_closure_id = cid.value_or(0);") != std::string::npos,
              "4725 AC2: scope-spawn / spawn-agent stamp the closure id");

        std::println("\n--- #4724 AC1: cancel-all + join-all :timeout-ms budget ---");
        // (a) scope-cancel-all reports 2 and the yield-looping bodies STOP at
        // their next fiber:yield (the safepoint now observes Fiber::request_cancel,
        // not only (fiber:cancel f)); join-all then settles quickly instead of
        // timing out after the drain window.
        const auto cancel_prog = R"AURA((define BB (hash))
(define (spin n) (if (> n 0) (begin (fiber:yield) (spin (- n 1))) (quote done)))
(orch:scope-spawn "exec-1" (lambda () (begin (spin 100000000) (hash-set! BB "exec1" (quote ran)) 1)))
(orch:scope-spawn "exec-2" (lambda () (begin (spin 100000000) (hash-set! BB "exec2" (quote ran)) 1)))
(define ca (orch:scope-cancel-all))
(define t0 (monotonic-ms))
(define j (orch:scope-join-all :timeout-ms 400))
(string-append
  (if (equal? (hash-ref ca "cancelled-count") 2) "2" "x")
  ":" (if (equal? (hash-ref j "status") "ok") "ok" "no")
  ":" (if (hash-has-key? BB "exec1") "ran" "stopped")
  ":" (if (< (- (monotonic-ms) t0) 1500) "fast" "slow"))
)AURA";
        auto cancel_r = cs.eval(cancel_prog);
        std::string cancel_got;
        if (cancel_r && is_string(*cancel_r)) {
            const auto idx = as_string_idx(*cancel_r);
            const auto heap = cs.evaluator().string_heap();
            if (idx < heap.size())
                cancel_got = std::string(heap[idx]);
        }
        std::println("  #4724 cancel probe -> {}", cancel_got);
        CHECK(cancel_got == "2:ok:stopped:fast",
              "4724 AC1: cancel-all stops yield-looping bodies and join-all settles");
        // (b) :timeout-ms bounds the WHOLE join: the 2 s default drain no
        // longer stacks after the deadline (3000 ms used to take 5002 ms).
        const auto budget_prog = R"AURA((define BB (hash))
(define (spin n) (if (> n 0) (begin (fiber:yield) (spin (- n 1))) (quote done)))
(orch:scope-spawn "slow" (lambda () (begin (spin 100000000) 1)))
(define t0 (monotonic-ms))
(define j (orch:scope-join-all :timeout-ms 400))
(define ms (- (monotonic-ms) t0))
(orch:scope-cancel-all)
(orch:scope-join-all :timeout-ms 2000)
(string-append (if (equal? (hash-ref j "status") "timeout") "timeout" "other")
               ":" (if (< ms 1500) "bounded" "overrun"))
)AURA";
        auto budget_r = cs.eval(budget_prog);
        std::string budget_got;
        if (budget_r && is_string(*budget_r)) {
            const auto idx = as_string_idx(*budget_r);
            const auto heap = cs.evaluator().string_heap();
            if (idx < heap.size())
                budget_got = std::string(heap[idx]);
        }
        std::println("  #4724 budget probe -> {}", budget_got);
        CHECK(budget_got == "timeout:bounded",
              "4724 AC1: :timeout-ms bounds join-all primary+drain");
        const auto msg_src_cancel = read_file("src/compiler/evaluator_primitives_messaging.cpp");
        CHECK(msg_src_cancel.find("g_current_fiber->is_cancel_requested()") != std::string::npos,
              "4724 AC1: yield safepoint observes the fiber cancel flag");
        CHECK(read_file("tests/orch/test_issue_4725.cpp").empty() &&
                  read_file("tests/issues/test_issue_4725.cpp").empty(),
              "4725 AC3: no test_issue_4725.cpp");
    }
    aura::compiler::typed_audit::apply_dev_audit_defaults();

    // ── Issues #4783 / #4784 / #4785 (aura-scm-agents validation) ─────
    {
        reset_all_agent_scopes_for_test();
        aura::compiler::typed_audit::apply_production_audit_defaults();
        CompilerService cs;
        const auto eval_str = [&cs](const char* prog) -> std::string {
            auto r = cs.eval(prog);
            if (!r || !is_string(*r))
                return {};
            const auto idx = as_string_idx(*r);
            const auto heap = cs.evaluator().string_heap();
            if (idx >= heap.size())
                return {};
            return std::string(heap[idx]);
        };

        std::println("\n--- #4784 AC1: supervise-batch retry-n re-applies a throwing task ---");
        const auto retry_got = eval_str(R"AURA((define c4784 (atomic:counter 0))
(define r (orch:supervise-batch
            (vector (lambda () (if (< (atomic:add! c4784 1) 3) (error "flaky") (quote ok))))
            (orch:compose-workflow (quote retry-n) :max-retries 2)))
(string-append (hash-ref r "status")
  ":" (number->string (hash-ref r "ok-count"))
  ":" (number->string (hash-ref r "err-count"))
  ":" (number->string (hash-ref r "retry-count"))
  ":" (number->string (atomic:counter-value c4784)))
)AURA");
        std::println("  #4784 retry probe -> {}", retry_got);
        CHECK(retry_got == "ok:1:0:2:3", "4784 AC1: flaky task recovers on the 3rd attempt");
        const auto exhaust_got = eval_str(R"AURA((define c4784b (atomic:counter 0))
(define r (orch:supervise-batch
            (vector (lambda () (begin (atomic:add! c4784b 1) (error "always"))))
            (orch:compose-workflow (quote retry-n) :max-retries 1)))
(string-append (hash-ref r "status")
  ":" (number->string (hash-ref r "err-count"))
  ":" (number->string (atomic:counter-value c4784b)))
)AURA");
        std::println("  #4784 exhaust probe -> {}", exhaust_got);
        CHECK(exhaust_got == "partial:1:2", "4784 AC1: exhausted retries stay partial (1 + 1 attempts)");
        const auto ign_got = eval_str(R"AURA((define w (orch:compose-workflow (quote collect-all) :max-retries 2))
(string-append (if (hash-ref w "max-retries-ignored") "ignored" "kept")
  ":" (number->string (hash-ref w "max-retries-requested")))
)AURA");
        std::println("  #4784 collect-all probe -> {}", ign_got);
        CHECK(ign_got == "ignored:2", "4784 AC2: collect-all flags :max-retries as ignored");

        std::println("\n--- #4785 AC1: same-name respawn keeps the queued mailbox backlog ---");
        const auto mb_got = eval_str(R"AURA((define (spawn4785 n) (orch:spawn-agent n (lambda () (quote idle)) :attach-mailbox #t))
(spawn4785 "risk4785")
(orch:agent-send "risk4785" "queued-before")
(define h2 (spawn4785 "risk4785"))
(orch:agent-send "risk4785" "queued-after")
(define m1 (orch:agent-recv "risk4785" :wait #f))
(define m2 (orch:agent-recv "risk4785" :wait #f))
(string-append (if (hash-ref h2 "ok") "t" "f")
  ":" (number->string (hash-ref h2 "inherited-messages"))
  ":" (number->string (hash-ref h2 "dropped-messages"))
  ":" (if (hash-ref m1 "ok") (hash-ref m1 "payload") "none")
  ":" (if (hash-ref m2 "ok") (hash-ref m2 "payload") "none"))
)AURA");
        std::println("  #4785 handover probe -> {}", mb_got);
        CHECK(mb_got == "t:1:0:queued-before:queued-after",
              "4785 AC1: respawn inherits queued-before ahead of queued-after");
        const auto spawn_src = read_file("src/orch/agent_spawn.h");
        CHECK(spawn_src.find("mailbox_bp_admit_retry_after_ms(scope_id)") != std::string::npos,
              "4785 AC2: BP admit retry-after-ms tracks the remaining quiet window");
        CHECK(aura::orch::mailbox_bp_admit_retry_after_ms("") >= 1,
              "4785 AC2: retry hint is positive");
        (void)cs.eval("(orch:scope-join-all :timeout-ms 2000)");
    }
    aura::compiler::typed_audit::apply_dev_audit_defaults();

    // ── Issue #4726: the orch scheduler must survive an idle gap ──────
    // Reported face (reopened on e6f4169e0): after orch:run-workflow the
    // next yield-waiting scope tasks NEVER run and scope-join-all times out
    // with every waiter still #f, and the process lingers for seconds.
    //
    // Mechanism: the orch primitives keep one process-wide Scheduler in a
    // static holder (OrchSchedHolder) and never recreate it. The IO loop's
    // idle auto-stop ("all fibers done, no spawn for ~3 ticks -> stop the
    // workers and exit the loop") is a SERVE-lifecycle convenience; when it
    // fired on that owned scheduler, every later scope-spawn was enqueued
    // onto STOPPED workers, so no body ever ran (no apply, no closure loss —
    // instrumented runs showed "DIAG scope-spawn" lines and zero body
    // entries) and the joins timed out. The holder now disables the auto-stop
    // (`sched->set_idle_auto_stop(false)`), which is the fix this AC pins.
    //
    // Placement: last block of this member on purpose. Pre-fix the wedge is
    // process-wide (the stopped workers are never replaced), so a failing
    // run poisons the rest of the batch; keeping it last confines that.
    {
        std::println("\n--- #4726 AC2: orch scheduler survives an idle gap ---");
        reset_all_agent_scopes_for_test();
        CompilerService cs_idle;
        const auto idle_run = [&cs_idle]() -> std::string {
            auto r = cs_idle.eval(R"AURA((define BB (hash))
(define (bbg k) (if (hash-has-key? BB k) (hash-ref BB k) #f))
(define (bb! k v) (hash-set! BB k v) v)
(define (st n) (if (equal? (bbg n) "done") "d" "x"))
(define (mk n)
  (orch:scope-spawn (string-append "i" n)
    (lambda ()
      (let wait ((i 0))
        (cond ((equal? (bbg "go") "go") (bb! n "done"))
              ((> i 200000) (bb! n "aborted"))
              (else (fiber:yield) (wait (+ i 1))))))))
(for-each mk (list "1" "2" "3" "4" "5" "6"))
(bb! "go" "go")
(define j (orch:scope-join-all :timeout-ms 6000))
(string-append (hash-ref j "status") ":"
  (st "1") (st "2") (st "3") (st "4") (st "5") (st "6"))
)AURA");
            if (!r || !is_string(*r))
                return {};
            const auto idx = as_string_idx(*r);
            const auto heap = cs_idle.evaluator().string_heap();
            if (idx >= heap.size())
                return {};
            return std::string(heap[idx]);
        };
        // Warm the process-wide orch scheduler, then idle past the auto-stop
        // window (3 IO ticks) before spawning anything else.
        const auto warm = idle_run();
        std::println("  #4726 warm -> {}", warm);
        std::this_thread::sleep_for(std::chrono::milliseconds(3600));
        const auto after_idle = idle_run();
        std::println("  #4726 after idle gap -> {}", after_idle);
        CHECK(warm == "ok:dddddd", "4726 AC2: waiters run before the idle gap");
        CHECK(after_idle == "ok:dddddd",
              "4726 AC2: waiters still run after a >3s idle gap (orch scheduler not self-stopped)");
        const auto sched_h = read_file("src/serve/scheduler.h");
        const auto sched_cpp = read_file("src/serve/scheduler.cpp");
        const auto prim_src = read_file("src/compiler/evaluator_primitives_agent.cpp");
        CHECK(sched_h.find("void set_idle_auto_stop(bool on) noexcept") != std::string::npos,
              "4726 AC2: Scheduler exposes the idle auto-stop switch");
        CHECK(sched_cpp.find("!spawned_since_tick && idle_auto_stop()") != std::string::npos,
              "4726 AC2: the IO loop consults the switch before self-stopping");
        CHECK(prim_src.find("sched->set_idle_auto_stop(false);") != std::string::npos,
              "4726 AC2: the process-lifetime orch holder disables the auto-stop");
    }

    std::println("\n=== results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_scope_join_tree_visibility();
}
#endif
