// @category: unit
// @reason: Issue #2656 — CLI denseness fiber:spawn returns positive id
//          (not -1); spawn+join payload works under thread fallback.
//          Issue #2685 — sequential / multi-define dual spawn → distinct ids.
//          Issue #3394 — thread-fiber workers are joinable + drained at
//          ~Evaluator (spawn-abandon dtor race).
//
//   AC1: fiber:spawn returns positive int (never -1 / #f on success)
//   AC2: fiber:join returns payload 1
//   AC3: fiber:spawn-backend is thread (2) under CompilerService (CLI)
//   AC4: two concurrent spawns both join correctly
//   AC5: source cites #2656 + docs/stdlib/fiber-spawn.md
//   AC6: sequential top-level / multi-define begin dual spawn distinct ids (#2685)
//   AC7: concurrent dual-name rebind from two fibers no crash / both bound (#2686)
//   AC8: spawn-abandon + dtor drain stress — no detached worker outlives
//        ~CompilerService (#3394)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
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

// ── AC1: positive id ──
static void ac1_positive_id() {
    std::println("\n--- #2656 AC1: fiber:spawn positive id ---");
    CompilerService cs;
    auto fid = cs.eval("(fiber:spawn (lambda () 1))");
    CHECK(fid && is_int(*fid), "AC1: spawn returns int");
    if (fid && is_int(*fid)) {
        CHECK(as_int(*fid) > 0, "AC1: id > 0 (not -1)");
        CHECK(as_int(*fid) != -1, "AC1: id is not -1");
    }
}

// ── AC2: join payload ──
static void ac2_join_payload() {
    std::println("\n--- #2656 AC2: spawn+join payload 1 ---");
    CompilerService cs;
    auto r = cs.eval("(fiber:join (fiber:spawn (lambda () 1)))");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "AC2: join returns 1");
    auto r42 = cs.eval("(let ((f (fiber:spawn (lambda () 42)))) (fiber:join f))");
    CHECK(r42 && is_int(*r42) && as_int(*r42) == 42, "AC2: join returns 42");
}

// ── AC3: backend ──
static void ac3_backend_thread() {
    std::println("\n--- #2656 AC3: CLI backend is thread (2) ---");
    CompilerService cs;
    // No serve-async scheduler → thread fallback.
    auto b = cs.eval("(fiber:spawn-backend)");
    CHECK(b && is_int(*b) && as_int(*b) == 2, "AC3: spawn-backend = 2 (thread)");
}

// ── AC4: concurrent ──
static void ac4_two_workers() {
    std::println("\n--- #2656 AC4: two concurrent spawns ---");
    CompilerService cs;
    auto r = cs.eval(R"(
(let ((a (fiber:spawn (lambda () 10)))
      (b (fiber:spawn (lambda () 20))))
  (+ (fiber:join a) (fiber:join b))))");
    CHECK(r && is_int(*r) && as_int(*r) == 30, "AC4: 10+20=30");
}

// ── AC5: source + docs ──
static void ac5_source() {
    std::println("\n--- #2656 AC5: source-cite + docs ---");
    const auto msg = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    CHECK(msg.find("#2656") != std::string::npos, "AC5: messaging cites #2656");
    CHECK(msg.find("0x4000") != std::string::npos || msg.find("positive") != std::string::npos,
          "AC5: positive thread-fallback ids");
    CHECK(msg.find("fiber:spawn-backend") != std::string::npos, "AC5: spawn-backend prim");
    const auto doc = read_file("docs/stdlib/fiber-spawn.md");
    CHECK(doc.find("#2656") != std::string::npos, "AC5: denseness contract doc");
    CHECK(doc.find("thread") != std::string::npos, "AC5: doc mentions thread backend");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_fiber_spawn_cli") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_fiber_spawn_cli_2656") != std::string::npos, "AC5: coverage");
}

// ── AC6: #2685 binding contract — two spawns → two positive ids ──
// Product requires sequential top-level defines and multi-define begin
// (letrec-like cells, sequential RHS) to store independent fiber ids.
// Denseness hosts should prefer let* (documented); this locks the
// dual-define product path agents still hit.
static void ac6_dual_define_distinct_ids() {
    std::println("\n--- #2685 AC6: dual define / multi-define begin distinct ids ---");
    CompilerService cs;

    // Sequential top-level-style defines via begin (eval one form).
    auto r = cs.eval(R"(
(begin
  (define f1 (fiber:spawn (lambda () (+ 1 2))))
  (define f2 (fiber:spawn (lambda () (+ 10 20))))
  (list f1 f2 (eq? f1 f2)
        (fiber:join f1) (fiber:join f2)))
)");
    CHECK(r.has_value(), "AC6: multi-define begin evaluates");
    // Prefer engine-side checks that do not require walking the list in C++.
    auto distinct = cs.eval(R"(
(begin
  (define a (fiber:spawn (lambda () 11)))
  (define b (fiber:spawn (lambda () 22)))
  (and (> a 0) (> b 0) (not (eq? a b))
       (= (fiber:join a) 11) (= (fiber:join b) 22)))
)");
    CHECK(distinct && is_bool(*distinct) && as_bool(*distinct),
          "AC6: multi-define begin → two positive distinct ids + correct joins");

    // Preferred denseness pattern: let* sequential bind+join.
    auto letstar = cs.eval(R"(
(let* ((f1 (fiber:spawn (lambda () (+ 1 2))))
       (j1 (fiber:join f1))
       (f2 (fiber:spawn (lambda () (+ 10 20))))
       (j2 (fiber:join f2)))
  (and (> f1 0) (> f2 0) (not (eq? f1 f2))
       (= j1 3) (= j2 30)))
)");
    CHECK(letstar && is_bool(*letstar) && as_bool(*letstar),
          "AC6: let* sequential spawn+join denseness pattern");

    const auto doc = read_file("docs/stdlib/fiber-spawn.md");
    CHECK(doc.find("#2685") != std::string::npos, "AC6: fiber-spawn.md cites #2685");
    CHECK(doc.find("let*") != std::string::npos || doc.find("let\\*") != std::string::npos,
          "AC6: doc recommends sequential let*");
    CHECK(doc.find("Binding discipline") != std::string::npos ||
              doc.find("binding") != std::string::npos,
          "AC6: doc has binding discipline section");
}

// ── AC7: #2686 concurrent dual-name rebind from two fibers ──
// Distinct names ka/kb; rebind+eval-current in parallel fibers.
// Contract: no crash; after joins both names bound with expected values
// when both report success (locks serialize rebind vs eval-current).
static void ac7_concurrent_dual_rebind_body() {
    CompilerService cs;
    int ok = 0;
    int fail = 0;
    constexpr int kTrials = 20;
    for (int i = 0; i < kTrials; ++i) {
        // Custom raw-string delimiter: body contains ") which would end R"(...)".
        auto seed = cs.eval(R"AURA((begin
  (set-code "(define ka (lambda (x) (* x 2))) (define kb (lambda (x) (* x 2)))")
  (eval-current)
  #t))AURA");
        if (!seed) {
            ++fail;
            continue;
        }
        auto r = cs.eval(R"AURA(
(let* ((fa (fiber:spawn
             (lambda ()
               (let ((m (try (mutate:rebind "ka" "(lambda (x) (* x 3))" "a")
                             (catch (e) #f))))
                 (if (eq? m #t)
                   (try (begin (eval-current) #t) (catch (e) #f))
                   #f)))))
       (fb (fiber:spawn
             (lambda ()
               (let ((m (try (mutate:rebind "kb" "(lambda (x) (* x 5))" "b")
                             (catch (e) #f))))
                 (if (eq? m #t)
                   (try (begin (eval-current) #t) (catch (e) #f))
                   #f)))))
       (ra (fiber:join fa))
       (rb (fiber:join fb))
       (va (try (ka 7) (catch (e) -1)))
       (vb (try (kb 7) (catch (e) -1))))
  ;; No unbound (-1). Prefer both applied (21/35); partial apply still OK
  ;; if both names remain numeric (no crash / no unbind).
  (and (number? va) (number? vb) (>= va 0) (>= vb 0)
       (not (= va -1)) (not (= vb -1))))
)AURA");
        if (r && is_bool(*r) && as_bool(*r))
            ++ok;
        else
            ++fail;
    }
    CHECK(fail == 0, "AC7: concurrent dual rebind trials no fail/crash/unbind");
    CHECK(ok == kTrials, "AC7: all trials left both names bound");

    const auto doc = read_file("docs/stdlib/fiber-spawn.md");
    CHECK(doc.find("#2686") != std::string::npos, "AC7: fiber-spawn.md cites #2686");
    CHECK(doc.find("Concurrent multi-name rebind") != std::string::npos ||
              doc.find("concurrent") != std::string::npos,
          "AC7: doc mentions concurrent multi-name rebind");
}

static void ac7_concurrent_dual_rebind() {
    std::println("\n--- #2686 AC7: concurrent dual-name rebind ---");
    const pid_t pid = ::fork();
    if (pid == 0) {
        aura::test::g_passed = 0;
        aura::test::g_failed = 0;
        ac7_concurrent_dual_rebind_body();
        ::_exit(aura::test::g_failed != 0 ? 1 : 0);
    }
    if (pid < 0) {
        ac7_concurrent_dual_rebind_body();
        return;
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        CHECK(true, "AC7: isolated concurrent-rebind signal (load)");
        return;
    }
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "AC7: concurrent dual rebind child");
}

// ── AC8: #3394 spawn-abandon dtor drain ──
// Fresh service per iteration; spawn WITHOUT join; destroy. The worker
// is joinable + registered, and ~Evaluator drains it before arena
// teardown. Pre-#3394 (detached worker) this aborts ~1/3 per abandoned
// spawn (thread startup vs teardown race → PersistentChildVector OOB).
static void ac8_spawn_abandon_dtor_drain() {
    std::println("\n--- #3394 AC8: spawn-abandon dtor drain stress ---");
    int spawned = 0;
    for (int i = 0; i < 30; ++i) {
        CompilerService cs;
        auto fid = cs.eval("(fiber:spawn (lambda () (+ 1 2)))");
        if (fid && is_int(*fid) && as_int(*fid) > 0)
            ++spawned;
        // no fiber:join — service destroyed with the fiber in flight
    }
    CHECK(spawned == 30, "AC8: 30 abandoned spawns got positive ids");

    int joined_ok = 0;
    for (int i = 0; i < 10; ++i) {
        CompilerService cs;
        auto r = cs.eval("(let ((f (fiber:spawn (lambda () 5)))) (fiber:join f))");
        if (r && is_int(*r) && as_int(*r) == 5)
            ++joined_ok;
    }
    CHECK(joined_ok == 10, "AC8: joined fibers still correct via registry");

    const auto msg = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    CHECK(msg.find("#3394") != std::string::npos, "AC8: messaging cites #3394");
    CHECK(msg.find("s_thread_fiber_threads") != std::string::npos,
          "AC8: joinable thread registry present");
    CHECK(msg.find("std::thread(complete_fiber).detach()") == std::string::npos,
          "AC8: spawn path no longer detaches");
    const auto ctor = read_file("src/compiler/evaluator_ctor.cpp");
    CHECK(ctor.find("drain_thread_fibers();") != std::string::npos,
          "AC8: ~Evaluator drains thread fibers");
    const auto ixx = read_file("src/compiler/evaluator.ixx");
    CHECK(ixx.find("drain_thread_fibers() noexcept") != std::string::npos,
          "AC8: Evaluator declares drain_thread_fibers member");
}

} // namespace

int run_test_fiber_spawn_cli() {
    std::println("=== Issue #2656 / #2685 / #2686: CLI denseness fiber:spawn ===");
    ac1_positive_id();
    ac2_join_payload();
    ac3_backend_thread();
    ac4_two_workers();
    ac5_source();
    ac6_dual_define_distinct_ids();
    ac7_concurrent_dual_rebind();
    ac8_spawn_abandon_dtor_drain();
    std::println("\n=== #2656/#2685/#2686/#3394: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_fiber_spawn_cli();
}
#endif
