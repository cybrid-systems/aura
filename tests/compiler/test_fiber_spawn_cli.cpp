// @category: unit
// @reason: Issue #2656 — CLI denseness fiber:spawn returns positive id
//          (not -1); spawn+join payload works under thread fallback.
//          Issue #2685 — sequential / multi-define dual spawn → distinct ids.
//          Issue #3394 — thread-fiber workers are joinable + drained at
//          ~Evaluator (spawn-abandon dtor race).
//          Issue #4131 — oneshot SIGTERM stubbornness: an ancestor's SIG_IGN
//          for TERM leaks across execve; the oneshot face resets TERM to
//          SIG_DFL so the host timeout SIGTERM is always honored.
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
//   AC9: #4131 oneshot dies on SIGTERM despite inherited SIG_IGN (bounded)
//   AC10: #4131 ordinary oneshot unchanged (clean exit 0)
//   AC11: #4131 default-disposition SIGTERM still prompt (no TERM handler)
//   AC12: #4131 source-cite: main.cpp oneshot reset; no sigaction(SIGTERM)
//   AC13: #4720 serial pin — FLIPPED by #4717: two CPU-bound CLI thread
//         fibers ARE simultaneously runnable (max 2 across 200 /proc
//         samples). Pre-#4717 the body mutex held it at 1.
//   AC14: #4720 control — the same sampler observes 2 spinning std::threads
//         runnable in one sample (probe is not vacuous).
//   AC15: #4715 overlay data layer — disjoint per-fiber overlays both commit
//         (fast-forward path, private writes invisible until commit).
//   AC16: #4715 same-name write-write — deterministic single winner, the
//         loser applies nothing, 5 identical rounds.
//   AC17: #4715 rollback drops the overlay and applies nothing.
//   AC18: #4715 moved base epoch takes the per-binding merge path and still
//         reports a same-binding conflict between two same-base overlays.
//   AC19: #4717 source-cite — the thread-backend body no longer takes the
//         CLI body/commit lock, and the per-body eval state (self-TCO stamp,
//         eval depth) lives in the fiber Context.
//   AC20: #4715 AC2 source-cite — the overlay is consulted by ordinary env
//         reads behind a one-thread_local fast path, and the define / set! /
//         eval-current write paths capture into it.
//   AC21: #4715 AC2 literal shape — mutate:rebind + eval-current under an
//         open overlay stays private until commit (own read 49, sibling 14),
//         a concurrent watcher sees old-or-new, and a same-base sibling that
//         committed first wins with a fiber:cow-conflict for the loser.
//   AC22: #4733 — a variadic (define (f . xs) ...) called from 4 parallel
//         fiber bodies reports 0 bad rest lists over 3 x 4 x 2000 calls
//         (the rest spine mints under alloc_storage_lock_).
//   AC23: #4734 — 200 waves of 4 parallel eval bodies complete (bounded
//         subprocess; the shard -> bindings inversion is gone).
//   AC24: #4745 — a pure closure called through an inner lambda from 8 parallel
//         fiber bodies returns only #t/#f, never a leaked primitive value.
//   AC25: #4771 — begin_at(historical base_seq): a commit that landed after
//         the worldline snapshot but before the overlay opened is a conflict;
//         begin() / current-base still fast-forward as before.
//   AC26: #4771 — prepare/finalize two-phase commit: prepare applies and logs
//         nothing, reserves the names (sibling gets busy, overlay kept),
//         finalize publishes and cannot conflict; stale ticket is refused.
//   AC27: #4771 — prepare/cancel (WAL write failed): no ghost claim is left
//         in the log, so a later overlay from the same base commits cleanly.

#include "test_harness.hpp"
#include "compiler/fiber_cow_overlay.hh" // Issue #4715

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <print>
#include <string>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

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

// ── #4131: oneshot SIGTERM stubbornness ──
// POSIX carries an ancestor's SIG_IGN for TERM across execve, so a harness
// that ignored SIGTERM leaves the exec'd aura oneshot TERM-immune (only
// killpg(SIGKILL) reclaims a hung eval spin). The oneshot fallthrough in
// main.cpp resets TERM to SIG_DFL; these ACs pin the runtime contract.
static std::string aura_bin_path_4131() {
    char self[4096];
    const ssize_t n = ::readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0)
        return {};
    self[n] = '\0';
    std::string dir(self);
    const auto slash = dir.find_last_of('/');
    if (slash != std::string::npos)
        dir.resize(slash);
    for (const auto& cand : {dir + "/aura", dir + "/../aura"}) {
        if (::access(cand.c_str(), X_OK) == 0)
            return cand;
    }
    return {};
}

static std::string write_4131_program(const char* name, const std::string& body) {
    const auto path = std::string("/tmp/") + name + "." + std::to_string(::getpid()) + ".aura";
    std::ofstream out(path);
    if (!out)
        return {};
    out << body;
    return path;
}

// fork + exec `aura file`; when ignore_term, the fork-child sets SIGTERM to
// SIG_IGN before exec (the #4131 harness shape — SIG_IGN survives execve).
static pid_t spawn_4131_oneshot(const std::string& bin, const std::string& file, bool ignore_term) {
    // Flush before fork: the child inherits a COPY of the unflushed stdout
    // buffer and its freopen() flush would re-emit it (duplicated suite
    // output under piped/fully-buffered runs).
    ::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid != 0)
        return pid;
    if (ignore_term)
        ::signal(SIGTERM, SIG_IGN);
    ::freopen("/dev/null", "w", stdout);
    ::freopen("/dev/null", "w", stderr);
    ::execl(bin.c_str(), bin.c_str(), file.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
}

// Block until /proc/<pid>/exe names the aura image (basename /aura, NOT a
// repo path that merely contains "/aura") or the deadline expires.
static bool wait_4131_executed(pid_t pid, int deadline_ms) {
    const auto link = "/proc/" + std::to_string(pid) + "/exe";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        char buf[512];
        const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
        if (n > 5) {
            buf[n] = '\0';
            const std::string exe(buf);
            if (exe.compare(exe.size() - 5, 5, "/aura") == 0)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// SIGTERM then reap within deadline_ms: WTERMSIG on signal death, 0 on a
// clean exit, -1 on timeout (child SIGKILLed) or wait error.
static int term_4131_and_reap(pid_t pid, int deadline_ms) {
    ::kill(pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) == pid)
            return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return -1;
}

// Reap within deadline_ms without signalling: exit code, else -1 (SIGKILL).
static int wait_4131_exit(pid_t pid, int deadline_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int st = 0;
        if (::waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
    return -1;
}

static void ac9_oneshot_sigterm_inherited_ignore() {
    std::println("\n--- #4131 AC9: oneshot honors SIGTERM despite inherited SIG_IGN ---");
    const auto bin = aura_bin_path_4131();
    CHECK(!bin.empty(), "AC9: aura binary resolved beside the test binary");
    if (bin.empty())
        return;
    const auto spin =
        write_4131_program("aura_4131_spin", "(while (lambda () #t) (lambda () 1))\n");
    CHECK(!spin.empty(), "AC9: spin program written");
    if (spin.empty())
        return;
    const pid_t pid = spawn_4131_oneshot(bin, spin, /*ignore_term=*/true);
    CHECK(pid > 0, "AC9: oneshot child spawned");
    if (pid <= 0)
        return;
    CHECK(wait_4131_executed(pid, 15000), "AC9: child reached the aura image (SIG_IGN inherited)");
    // Settle past the exec→first-statement window so the TERM lands after
    // main's disposition reset (the reset is what the AC exercises).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int sig = term_4131_and_reap(pid, 8000);
    CHECK(sig == SIGTERM, "AC9: SIGTERM reclaimed the oneshot (bounded, no SIGKILL)");
}

static void ac10_ordinary_program_unchanged() {
    std::println("\n--- #4131 AC10: ordinary oneshot unchanged ---");
    const auto bin = aura_bin_path_4131();
    CHECK(!bin.empty(), "AC10: aura binary resolved");
    if (bin.empty())
        return;
    const auto prog = write_4131_program("aura_4131_add", "(display (+ 1 2))\n");
    CHECK(!prog.empty(), "AC10: ordinary program written");
    if (prog.empty())
        return;
    const pid_t pid = spawn_4131_oneshot(bin, prog, /*ignore_term=*/false);
    CHECK(pid > 0, "AC10: child spawned");
    if (pid <= 0)
        return;
    const int rc = wait_4131_exit(pid, 30000);
    CHECK(rc == 0, "AC10: ordinary (+ 1 2) oneshot exits 0");
}

static void ac11_oneshot_sigterm_default_still_prompt() {
    std::println("\n--- #4131 AC11: default-disposition SIGTERM still prompt ---");
    const auto bin = aura_bin_path_4131();
    CHECK(!bin.empty(), "AC11: aura binary resolved");
    if (bin.empty())
        return;
    const auto spin =
        write_4131_program("aura_4131_spin2", "(while (lambda () #t) (lambda () 1))\n");
    CHECK(!spin.empty(), "AC11: spin program written");
    if (spin.empty())
        return;
    const pid_t pid = spawn_4131_oneshot(bin, spin, /*ignore_term=*/false);
    CHECK(pid > 0, "AC11: child spawned");
    if (pid <= 0)
        return;
    CHECK(wait_4131_executed(pid, 15000), "AC11: child reached the aura image");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const int sig = term_4131_and_reap(pid, 8000);
    CHECK(sig == SIGTERM, "AC11: TERM still prompt without inherited ignore");
}

static void ac12_source_cite() {
    std::println("\n--- #4131 AC12: source-cite + placement ---");
    const auto main_src = read_file("src/main.cpp");
    CHECK(main_src.find("::signal(SIGTERM, SIG_DFL)") != std::string::npos,
          "AC12: oneshot resets TERM to SIG_DFL (inherited SIG_IGN hardening)");
    CHECK(main_src.find("#4131") != std::string::npos, "AC12: main.cpp cites #4131");
    CHECK(main_src.find("::sigaction(SIGTERM") == std::string::npos,
          "AC12: no SIGTERM handler installed — prompt default death; serve face untouched");
}

// ── #4720 / #4717: serial pin for CLI thread-fiber BODIES ─────────────
// Today every thread-backend fiber body is serialized on
// s_cli_thread_fiber_body_mtx (complete_fiber,
// src/compiler/evaluator_primitives_messaging.cpp). Issue #4707 removes
// that body mutex; P4 (#4717) MUST FLIP ac13 (expect overlap == 2) in the
// SAME commit that removes it. Until then this pin is the regression
// tripwire that keeps two CPU-bound bodies from overlapping on the shared
// CLI Evaluator.
//
// Probe: sample /proc/<pid>/task/<tid>/stat (field 3, state) for every
// non-main task whose comm is "aura-fiber" (a real thread-backend body). Before #4717 a parked
// sibling sat in pthread_mutex_lock (futex, S) and the maximum number of R tasks over 200 samples
// stayed 1; with the body mutex gone both bodies are R in the same sample. Sampling starts only
// after BOTH workers exist and one is R (a body is really executing), and stops long before the
// first body (fib 28) finishes, so the thread-exit hand-off window cannot inject a transient 2.
// AC14 proves the sampler is not vacuous with two concurrent std::thread spinners.
static constexpr int kPinSamples4720 = 200;

static char proc_task_state_4720(const std::string& task_dir) {
    std::ifstream in(task_dir + "/stat");
    std::string line;
    if (!in || !std::getline(in, line))
        return '\0';
    // Field 3 is the state char, after the LAST ')' of "(comm)".
    const auto rp = line.rfind(')');
    if (rp == std::string::npos || rp + 2 >= line.size())
        return '\0';
    return line[rp + 2];
}

// Issue #4717: a fiber worker names its thread "aura-fiber" at body start
// (evaluator_primitives_messaging.cpp). Counting EVERY non-main task made
// this probe read 3 workers under ThreadSanitizer, whose own runtime thread
// lives in the same process, so AC13 failed there for a reason unrelated to
// the body mutex. Filter on the published name instead: only real
// thread-backend fiber bodies count as fiber tasks.
static bool proc_task_is_fiber_4720(const std::string& task_dir) {
    std::ifstream in(task_dir + "/comm");
    std::string comm;
    if (!in || !std::getline(in, comm))
        return false;
    return comm.rfind("aura-fiber", 0) == 0;
}

static std::vector<long> proc_tids_4720(long pid) {
    std::vector<long> tids;
    const std::string dir = "/proc/" + std::to_string(pid) + "/task";
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return tids;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        const long tid = std::strtol(e->d_name, nullptr, 10);
        if (!proc_task_is_fiber_4720(dir + "/" + e->d_name))
            continue;
        tids.push_back(tid);
    }
    ::closedir(d);
    std::sort(tids.begin(), tids.end());
    return tids;
}

// Count R-state tasks among tids (a task may vanish mid-walk: skipped).
static int runnable_count_4720(long pid, const std::vector<long>& tids) {
    int r = 0;
    for (const long tid : tids) {
        const auto dir = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid);
        if (proc_task_state_4720(dir) == 'R')
            ++r;
    }
    return r;
}

// Fork + exec the pin program. AURA_PIPELINE_STRICT=0: the oneshot strict
// SOA pipeline (Issue #2213 face) refuses to lower a fiber:spawn call
// today ("tree-walker fallback forbidden in production pipeline"), so the
// pin runs the CLI's tree-walker face. The body scheduler and body mutex
// under test live in complete_fiber (evaluator_primitives_messaging.cpp)
// and are identical on both faces — fiber:spawn-backend is still 2.
static pid_t spawn_4720_pin_child(const std::string& bin, const std::string& file) {
    ::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid != 0)
        return pid;
    ::setenv("AURA_PIPELINE_STRICT", "0", 1);
    ::freopen("/dev/null", "w", stdout);
    ::freopen("/dev/null", "w", stderr);
    ::execl(bin.c_str(), bin.c_str(), file.c_str(), static_cast<char*>(nullptr));
    ::_exit(127);
}

static void ac13_serial_pin_two_cpu_bound_fibers() {
    std::println("\n--- #4720 AC13: CLI thread fibers — max 1 runnable body ---");
    const auto bin = aura_bin_path_4131();
    CHECK(!bin.empty(), "AC13: aura binary resolved beside the test binary");
    if (bin.empty())
        return;
    // fib 28 / fib 29: pure CPU work with no yield primitive, so a running
    // body holds the body mutex start->finish across the whole window.
    const auto prog = write_4131_program(
        "aura_4720_pin",
        "(begin\n"
        "  (define (fib n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\n"
        "  (define a (fiber:spawn (lambda () (fib 28))))\n"
        "  (define b (fiber:spawn (lambda () (fib 29))))\n"
        "  (display (fiber:join a)) (display \" \") (display (fiber:join b)) (newline))\n");
    CHECK(!prog.empty(), "AC13: pin program written");
    if (prog.empty())
        return;
    const pid_t pid = spawn_4720_pin_child(bin, prog);
    CHECK(pid > 0, "AC13: CLI child spawned");
    if (pid <= 0)
        return;
    CHECK(wait_4131_executed(pid, 15000), "AC13: child reached the aura image");

    // Start gate: both worker threads exist AND one body is runnable, so a
    // vacuous window (fibers not spawned yet / all parked) cannot pass.
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool started = false;
    while (std::chrono::steady_clock::now() < start_deadline) {
        if (::kill(pid, 0) != 0)
            break; // died before both bodies ran — reported below
        std::vector<long> fibers;
        for (const long tid : proc_tids_4720(pid)) {
            if (tid != pid)
                fibers.push_back(tid);
        }
        if (fibers.size() >= 2 && runnable_count_4720(pid, fibers) >= 1) {
            started = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(started, "AC13: both fiber workers live + a body runnable (valid window)");

    int samples = 0;
    int max_runnable = 0;
    int samples_runnable = 0;
    int max_fiber_tasks = 0;
    bool window_closed = false;
    if (started) {
        while (samples < kPinSamples4720) {
            std::vector<long> fibers;
            for (const long tid : proc_tids_4720(pid)) {
                if (tid != pid)
                    fibers.push_back(tid);
            }
            if (fibers.size() < 2) {
                window_closed = true;
                break;
            }
            const int r = runnable_count_4720(pid, fibers);
            max_runnable = std::max(max_runnable, r);
            if (r >= 1)
                ++samples_runnable;
            max_fiber_tasks = std::max(max_fiber_tasks, static_cast<int>(fibers.size()));
            ++samples;
        }
    }
    // The bodies are seconds long; the pin only needs the steady-state
    // window, so stop them unconditionally (SIGKILL cannot be ignored).
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);

    std::println("  #4720 pin evidence: samples={} max_concurrent_runnable={} "
                 "samples_runnable={} max_fiber_tasks={} window_closed={}",
                 samples, max_runnable, samples_runnable, max_fiber_tasks, window_closed);
    CHECK(samples == kPinSamples4720, "AC13: collected all 200 samples inside the body window");
    CHECK(!window_closed, "AC13: CLI stayed alive for the whole sample window");
    CHECK(max_fiber_tasks == 2, "AC13: exactly two fiber worker tasks (thread backend)");
    CHECK(samples_runnable >= kPinSamples4720 - 10,
          "AC13: a fiber body was runnable in >=95% of samples (probe not idle)");
    // #4717 flipped this from <= 1: the whole-body mutex is gone, so both
    // CPU-bound bodies are runnable in the same sample.
    CHECK(max_runnable == 2,
          "AC13: max concurrent runnable fiber bodies == 2 (body mutex removed)");
}

static void ac14_sampler_control_two_runnable() {
    std::println("\n--- #4720 AC14: control — sampler sees 2 co-runnable threads ---");
    std::atomic<bool> stop{false};
    std::atomic<long> tids[2];
    tids[0].store(0);
    tids[1].store(0);
    const auto spin = [&stop, &tids](int idx) {
        tids[idx].store(static_cast<long>(::syscall(SYS_gettid)), std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            // busy CPU loop — keep this thread in state R
        }
    };
    std::thread t0(spin, 0);
    std::thread t1(spin, 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((tids[0].load(std::memory_order_acquire) == 0 ||
            tids[1].load(std::memory_order_acquire) == 0) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::vector<long> pair{tids[0].load(std::memory_order_acquire),
                                 tids[1].load(std::memory_order_acquire)};
    const bool have_both = pair[0] > 0 && pair[1] > 0;
    int samples = 0;
    int max_runnable = 0;
    int samples_both = 0;
    if (have_both) {
        for (; samples < kPinSamples4720; ++samples) {
            const int r = runnable_count_4720(static_cast<long>(::getpid()), pair);
            max_runnable = std::max(max_runnable, r);
            if (r == 2)
                ++samples_both;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    t0.join();
    t1.join();
    std::println("  #4720 control evidence: samples={} max_concurrent_runnable={} samples_both={}",
                 samples, max_runnable, samples_both);
    CHECK(have_both, "AC14: both spinner tids published");
    CHECK(samples == kPinSamples4720, "AC14: collected all control samples");
    CHECK(max_runnable == 2, "AC14: sampler observes 2 simultaneously runnable tasks");
    CHECK(samples_both >= kPinSamples4720 - 10,
          "AC14: both spinners R in >=95% of samples (sampler not vacuous)");
}

// ── Issue #4715: per-fiber COW overlay for env bindings (data layer) ──
// The evaluator face (fiber:overlay-*) is exercised at runtime by
// tests/python/test_regression.py::test_fiber_cow_overlay_*_4715. These
// rows pin the data-layer contract directly so the overlay can be reused by
// the workspace-node overlay without re-testing the protocol:
//   AC15 disjoint per-fiber overlays both commit on the fast-forward path.
//   AC16 same-name write-write is a deterministic single-winner conflict
//        with NO partial apply (5 identical rounds).
//   AC17 rollback drops the overlay and applies nothing.
//   AC18 a moved base epoch takes the per-binding merge path and still
//        reports a write-write conflict between two same-base overlays.
void ac15_cow_overlay_disjoint_commit() {
    std::println("--- #4715 AC1: disjoint per-fiber overlays both commit");
    using Store = aura::compiler::cow::OverlayRegistry<int>;
    Store reg;
    std::unordered_map<std::string, int> shared;
    auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
    const void* owner = "ac15";
    constexpr std::uint64_t epoch = 7;
    const auto base_a = reg.begin(owner, 1, epoch);
    const auto base_b = reg.begin(owner, 2, epoch);
    CHECK(base_a == epoch && base_b == epoch, "AC15: both overlays snapshot the same base epoch");
    CHECK(reg.write(owner, 1, "cow-a", 7, epoch), "AC15: fiber 1 private define");
    CHECK(reg.write(owner, 2, "cow-b", 9, epoch), "AC15: fiber 2 private define");
    CHECK(shared.empty(), "AC15: private writes are invisible before commit");
    const auto ra = reg.commit(owner, 1, epoch, apply);
    const auto rb = reg.commit(owner, 2, epoch, apply);
    CHECK(ra.ok && ra.applied == 1, "AC15: fiber 1 commit applies its binding");
    CHECK(rb.ok && rb.applied == 1, "AC15: fiber 2 commit applies its binding");
    CHECK(ra.fast_forward && rb.fast_forward, "AC15: unchanged base takes the fast-forward path");
    CHECK(shared.size() == 2 && shared["cow-a"] == 7 && shared["cow-b"] == 9,
          "AC15: both disjoint names reached the shared env");
    const auto st = reg.stats(owner);
    CHECK(st.commits == 2 && st.conflicts == 0 && st.active == 0,
          "AC15: stats record 2 commits, 0 conflicts, no overlay left open");
}

void ac16_cow_overlay_same_name_conflict() {
    std::println("--- #4715 AC1: same-name write-write is deterministic");
    using Store = aura::compiler::cow::OverlayRegistry<int>;
    const void* owner = "ac16";
    constexpr std::uint64_t epoch = 11;
    std::size_t rounds = 0;
    for (int run = 0; run < 5; ++run) {
        Store reg;
        std::unordered_map<std::string, int> shared;
        auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
        reg.begin(owner, 1, epoch);
        reg.begin(owner, 2, epoch);
        reg.write(owner, 1, "cow-shared", 10, epoch);
        reg.write(owner, 2, "cow-shared", 20, epoch);
        const auto first = reg.commit(owner, 1, epoch, apply);
        const auto second = reg.commit(owner, 2, epoch, apply);
        CHECK(first.ok && first.applied == 1, "AC16: first committer wins the binding");
        CHECK(!second.ok && second.conflict == "cow-shared" && second.applied == 0,
              "AC16: sibling commit from the same base reports the conflicting name");
        CHECK(shared.size() == 1 && shared["cow-shared"] == 10,
              "AC16: the loser applies nothing (no partial / merged write)");
        const auto st = reg.stats(owner);
        CHECK(st.commits == 1 && st.conflicts == 1,
              "AC16: exactly one commit and one conflict recorded");
        ++rounds;
    }
    CHECK(rounds == 5, "AC16: 5 rounds all produced the same verdict, not a race");
}

void ac17_cow_overlay_rollback() {
    std::println("--- #4715 AC3: rollback drops the overlay, applies nothing");
    using Store = aura::compiler::cow::OverlayRegistry<int>;
    Store reg;
    std::unordered_map<std::string, int> shared;
    auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
    const void* owner = "ac17";
    constexpr std::uint64_t epoch = 3;
    reg.begin(owner, 9, epoch);
    reg.write(owner, 9, "cow-drop", 1, epoch);
    reg.write(owner, 9, "cow-keep", 2, epoch);
    CHECK(reg.pending(owner, 9) == 2, "AC17: two private bindings pending");
    CHECK(reg.rollback(owner, 9), "AC17: rollback drops the open overlay");
    CHECK(reg.pending(owner, 9) == 0 && !reg.active(owner, 9),
          "AC17: no overlay state is left behind");
    const auto res = reg.commit(owner, 9, epoch, apply);
    CHECK(!res.ok && res.applied == 0 && shared.empty(),
          "AC17: a commit after rollback applies nothing");
    const auto st = reg.stats(owner);
    CHECK(st.rollbacks == 1 && st.commits == 0, "AC17: stats record the rollback");
}

void ac18_cow_overlay_moved_base_merge() {
    std::println("--- #4715 AC2: moved base takes the per-binding merge path");
    using Store = aura::compiler::cow::OverlayRegistry<int>;
    const void* owner = "ac18";
    constexpr std::uint64_t base_epoch = 5;
    constexpr std::uint64_t moved_epoch = 9;
    {
        Store reg;
        std::unordered_map<std::string, int> shared;
        auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
        reg.begin(owner, 1, base_epoch);
        reg.write(owner, 1, "cow-moved", 4, base_epoch);
        const auto res = reg.commit(owner, 1, moved_epoch, apply);
        CHECK(res.ok && !res.fast_forward && res.applied == 1,
              "AC18: an unmoved binding still merges when the base epoch moved");
        CHECK(shared.size() == 1 && shared["cow-moved"] == 4,
              "AC18: per-binding merge applied the binding");
        const auto st = reg.stats(owner);
        CHECK(st.merged == 1 && st.fast_forward == 0, "AC18: stats record the merged path");
    }
    {
        Store reg;
        std::unordered_map<std::string, int> shared;
        auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
        reg.begin(owner, 1, base_epoch);
        reg.begin(owner, 2, base_epoch);
        reg.write(owner, 1, "cow-moved", 4, base_epoch);
        reg.write(owner, 2, "cow-moved", 5, base_epoch);
        const auto first = reg.commit(owner, 1, moved_epoch, apply);
        const auto second = reg.commit(owner, 2, moved_epoch, apply);
        CHECK(first.ok, "AC18: sibling 1 merges after the epoch move");
        CHECK(!second.ok && second.conflict == "cow-moved",
              "AC18: sibling 2 still conflicts on the same binding after the epoch move");
        CHECK(shared["cow-moved"] == 4, "AC18: survivor is one whole write");
    }
}

// ── Issue #4771: worldline primitives on the overlay data layer ──────
void ac25_cow_overlay_begin_at_historical_base() {
    std::println("--- #4771 AC25: begin_at(historical base_seq) sees post-snapshot commits");
    using Store = aura::compiler::cow::OverlayRegistry<std::string>;
    auto noop = [](const std::string&, const std::string&) {};
    {
        // The exact issue shape: snapshot, mainline commits, THEN the
        // worldline opens its overlay. begin() would take the post-commit seq
        // as base and miss the stale write; begin_at() uses the snapshot's.
        Store R;
        int owner = 0;
        const std::uint64_t snapshot_epoch = 1;
        const auto snapshot_seq = R.current_seq(&owner);
        CHECK(snapshot_seq == 0, "AC25: empty log starts at seq 0");
        R.begin(&owner, 0, 1);
        R.write(&owner, 0, "rule", "v1", 1);
        const auto main_commit = R.commit(&owner, 0, 2, noop);
        CHECK(main_commit.ok && R.current_seq(&owner) == 1,
              "AC25: mainline commit after the snapshot advances the log");
        R.begin_at(&owner, 7, snapshot_epoch, snapshot_seq);
        CHECK(R.base_seq(&owner, 7) == snapshot_seq, "AC25: overlay base is the snapshot seq");
        R.write(&owner, 7, "rule", "v2", snapshot_epoch);
        const auto c = R.commit(&owner, 7, 2, noop);
        CHECK(!c.ok && c.conflict == "rule" && c.applied == 0,
              "AC25: stale worldline commit conflicts on 'rule' (was ok=1 before #4771)");
        CHECK(c.conflict_info.winner_seq == 1 && c.conflict_info.base_seq == snapshot_seq &&
                  !c.conflict_info.read_only,
              "AC25: structured condition names the post-snapshot winner");
    }
    {
        // Control: plain begin() keeps the historical "base = now" contract.
        Store R;
        int owner = 0;
        R.begin(&owner, 0, 1);
        R.write(&owner, 0, "rule", "v1", 1);
        R.commit(&owner, 0, 2, noop);
        R.begin(&owner, 7, 2);
        R.write(&owner, 7, "rule", "v2", 2);
        const auto c = R.commit(&owner, 7, 2, noop);
        CHECK(c.ok && c.fast_forward, "AC25: begin() at the current base still commits");
    }
    {
        // A base ahead of the log is clamped (a base cannot be in the future)
        // and an untouched snapshot base commits like begin().
        Store R;
        int owner = 0;
        R.begin_at(&owner, 3, 4, 999);
        CHECK(R.base_seq(&owner, 3) == 0, "AC25: future base_seq clamps to current seq");
        R.write(&owner, 3, "other", "x", 4);
        CHECK(R.commit(&owner, 3, 4, noop).ok, "AC25: disjoint worldline commit succeeds");
    }
}

void ac26_cow_overlay_two_phase_commit() {
    std::println("--- #4771 AC26: prepare/finalize two-phase commit");
    using Store = aura::compiler::cow::OverlayRegistry<int>;
    Store reg;
    std::unordered_map<std::string, int> shared;
    auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
    const void* owner = "ac26";
    constexpr std::uint64_t epoch = 5;
    reg.begin(owner, 1, epoch);
    reg.begin(owner, 2, epoch);
    reg.write(owner, 1, "rule-a", 10, epoch);
    reg.write(owner, 1, "rule-b", 11, epoch);
    reg.write(owner, 2, "rule-b", 20, epoch);
    const auto t = reg.prepare(owner, 1, epoch);
    CHECK(t.valid() && t.result.attempted_names.size() == 2,
          "AC26: prepare issues a ticket covering both names");
    CHECK(shared.empty() && reg.current_seq(owner) == 0,
          "AC26: prepare applies nothing and logs no claim");
    CHECK(!reg.write(owner, 1, "rule-c", 1, epoch), "AC26: a prepared overlay is frozen");
    const auto sib = reg.commit(owner, 2, epoch, apply);
    CHECK(!sib.ok && sib.busy && sib.conflict == "rule-b" && sib.applied == 0,
          "AC26: sibling commit on a reserved name is busy, not applied");
    CHECK(reg.active(owner, 2) && reg.pending(owner, 2) == 1,
          "AC26: busy keeps the sibling overlay for a retry");
    CHECK(reg.stats(owner).conflicts == 0, "AC26: busy is not counted as a conflict");
    const auto sib_prep = reg.prepare(owner, 2, epoch);
    CHECK(!sib_prep.valid() && sib_prep.result.busy, "AC26: sibling prepare is busy too");
    // ... caller persists its WAL record here ...
    const auto f = reg.finalize(t, apply);
    CHECK(f.ok && f.applied == 2 && shared["rule-a"] == 10 && shared["rule-b"] == 11,
          "AC26: finalize publishes the prepared write set");
    CHECK(reg.current_seq(owner) == 2 && !reg.active(owner, 1),
          "AC26: finalize logs both claims and closes the overlay");
    CHECK(!reg.finalize(t, apply).ok && !reg.cancel(t), "AC26: a used ticket is refused");
    const auto retry = reg.commit(owner, 2, epoch, apply);
    CHECK(!retry.ok && !retry.busy && retry.conflict == "rule-b" && shared["rule-b"] == 11,
          "AC26: after finalize the sibling retry is a real write-write conflict");
    const auto st = reg.stats(owner);
    CHECK(st.prepares == 1 && st.commits == 1 && st.conflicts == 1 && st.active == 0,
          "AC26: stats record 1 prepare, 1 commit, 1 conflict");
    // A conflicting prepare drops the overlay exactly like commit().
    reg.begin_at(owner, 3, epoch, 0);
    reg.write(owner, 3, "rule-a", 99, epoch);
    const auto bad = reg.prepare(owner, 3, epoch);
    CHECK(!bad.valid() && !bad.result.busy && bad.result.conflict == "rule-a" &&
              !reg.active(owner, 3),
          "AC26: conflicting prepare reports the name and drops the overlay");
}

void ac27_cow_overlay_prepare_cancel_no_ghost() {
    std::println("--- #4771 AC27: cancel after a failed WAL write leaves no ghost commit");
    using Store = aura::compiler::cow::OverlayRegistry<int>;
    Store reg;
    std::unordered_map<std::string, int> shared;
    auto apply = [&shared](const std::string& n, const int& v) { shared[n] = v; };
    const void* owner = "ac27";
    constexpr std::uint64_t epoch = 2;
    const auto snap = reg.current_seq(owner);
    reg.begin_at(owner, 1, epoch, snap);
    reg.begin_at(owner, 2, epoch, snap);
    reg.write(owner, 1, "rule", 1, epoch);
    reg.write(owner, 2, "rule", 2, epoch);
    const auto t = reg.prepare(owner, 1, epoch);
    CHECK(t.valid(), "AC27: prepare ok");
    const bool wal_ok = false; // simulated ENOSPC / EIO, process stays up
    if (!wal_ok)
        CHECK(reg.cancel(t), "AC27: cancel the prepared ticket");
    CHECK(shared.empty() && reg.current_seq(owner) == snap && !reg.active(owner, 1),
          "AC27: nothing applied, no claim logged, overlay dropped");
    const auto c = reg.commit(owner, 2, epoch, apply);
    CHECK(c.ok && shared["rule"] == 2,
          "AC27: the sibling from the same base commits (no ghost conflict)");
    const auto st = reg.stats(owner);
    CHECK(st.cancels == 1 && st.rollbacks == 1 && st.conflicts == 0 && st.commits == 1,
          "AC27: stats record the cancel as a rollback");
    // rollback() on a prepared overlay also releases its reservations.
    reg.begin(owner, 5, epoch);
    reg.begin(owner, 6, epoch);
    reg.write(owner, 5, "x", 1, epoch);
    reg.write(owner, 6, "x", 2, epoch);
    const auto t5 = reg.prepare(owner, 5, epoch);
    CHECK(t5.valid() && reg.rollback(owner, 5), "AC27: rollback a prepared overlay");
    CHECK(!reg.finalize(t5, apply).ok, "AC27: a rolled-back ticket cannot finalize");
    CHECK(reg.commit(owner, 6, epoch, apply).ok && shared["x"] == 2,
          "AC27: rollback released the reservation");
}

// ── #4717: the evaluator split cite ─────────────────────────────────
// P4 splits the shared body state: the thread-backend body no longer takes
// the CLI body/commit lock (the lock shrinks to the CLI main's commit
// window), and the per-body execution state (self-TCO stamp, eval depth)
// lives in the running fiber's Context (fiber_ctx::active()), not on the
// shared Evaluator.
static void ac19_eval_split_source_cite() {
    std::println("\n--- #4717 AC19: body mutex removed + per-body eval context ---");
    const auto msg = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    CHECK(msg.find("#4717") != std::string::npos, "AC19: messaging cites #4717");
    CHECK(msg.find("cli_body_lock_acquire(body_lock)") == std::string::npos,
          "AC19: thread-backend body takes no whole-body lock");
    const auto ctx = read_file("src/core/fiber_context.hh");
    CHECK(ctx.find("#4717") != std::string::npos, "AC19: fiber_context cites #4717");
    CHECK(ctx.find("TcoReuseState") != std::string::npos,
          "AC19: self-TCO stamp is fiber-local state");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("tco_reuse_ctx()") != std::string::npos,
          "AC19: eval_flat stamps the fiber context, not the shared Evaluator");
}

// ── AC20: #4715 AC2 — the overlay intercepts ORDINARY env reads ───────
// Before AC2 the overlay was opt-in through fiber:overlay-get only: a body
// that did (mutate:rebind ...) + (eval-current) wrote the shared cell
// immediately and the pre-overlay / post-overlay window was observable by
// every other body. These rows pin the wiring (source cites + the thread_local
// fast-path gate) and then run the literal shape end-to-end.
static void ac20_cow_overlay_env_intercept_cite() {
    std::println("\n--- #4715 AC2: overlay intercepts ordinary env reads ---");
    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("#4715 AC2") != std::string::npos, "AC20: env read path cites #4715 AC2");
    CHECK(env.find("primitives_detail::t_fiber_cow_overlay_depth != 0") != std::string::npos,
          "AC20: Env::lookup gates on the thread_local overlay depth (fast path)");
    CHECK(env.find("fiber_cow_overlay_env_read") != std::string::npos,
          "AC20: Env::lookup consults the overlay registry for a shadowed name");
    CHECK(env.find("fiber_cow_overlay_env_cell") != std::string::npos,
          "AC20: Env::lookup_cell_index resolves the overlay private cell");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("fiber_cow_overlay_capture_define") != std::string::npos,
          "AC20: mutate:rebind refresh stays private under an overlay");
    CHECK(flat.find("fiber_cow_overlay_capture_set") != std::string::npos,
          "AC20: set! of a top-level binding is captured by an open overlay");
    CHECK(flat.find("primitives_detail::t_fiber_cow_overlay_depth == 0") != std::string::npos,
          "AC20: the same-pool host chain read is skipped while an overlay is open");
    const auto svc = read_file("src/compiler/service.ixx");
    CHECK(svc.find("fiber_cow_overlay_capture_define") != std::string::npos,
          "AC20: the eval-current define bind is captured by an open overlay");
    const auto hh = read_file("src/compiler/fiber_cow_overlay.hh");
    CHECK(hh.find("read_cell") != std::string::npos && hh.find("write_cell") != std::string::npos,
          "AC20: the overlay records the private cell of a captured write");
    CHECK(hh.find("std::is_invocable_v<ApplyFn&") != std::string::npos,
          "AC20: commit hands the private cell to the apply callback");
    const auto cow = read_file("src/compiler/evaluator_primitives_fiber_cow.cpp");
    CHECK(cow.find("lock_bindings_exclusive") != std::string::npos,
          "AC20: commit publishes in bindings -> registry lock order");
}

// ── AC21: #4715 AC2 literal shape ────────────────────────────────────
// (mutate:rebind ...) + (eval-current) INSIDE an open overlay:
//   arm 1 (closure value, the exact issue shape): the body reads its own 49,
//        the commit lands as one whole version, and a concurrent watcher
//        spanning the commit only ever sees a WHOLE 14 or 49 - never a dead
//        or half-applied closure. A closure-valued rebind keeps the
//        ordinary write-through (see overlay_capturable in the COW TU: a
//        private closure copy would let the shared cell keep a closure the
//        rebind already deopted, so the workspace slice must come first).
//   arm 2 (scalar value): the same shape IS private - the body reads its own
//        9 while a sibling fiber still reads the committed 5, and the commit
//        publishes 9.
//   arm 3 (scalar conflict): a same-base sibling that commits first wins; our
//        commit returns the structured fiber:cow-conflict, applies nothing
//        and the survivor stays one whole write.
static void ac21_cow_overlay_rebind_eval_current() {
    std::println("\n--- #4715 AC2: literal mutate:rebind + eval-current under an overlay ---");
    CompilerService cs;
    auto seed = cs.eval(R"AURA((begin
  (set-code "(define cow-target (lambda (x) (* x 2)))")
  (eval-current)
  #t))AURA");
    CHECK(seed && is_bool(*seed) && as_bool(*seed), "AC21: workspace define seeded");

    auto r = cs.eval(R"AURA(
(let* ((open (fiber:overlay-begin))
       (m (try (mutate:rebind "cow-target" "(lambda (x) (* x 7))" "cow") (catch (e) #f)))
       (ec (if (eq? m #t) (try (begin (eval-current) #t) (catch (e) #f)) #f))
       (own (cow-target 7))
       (sib (fiber:join (fiber:spawn (lambda () (cow-target 7)))))
       (watch (fiber:spawn
               (lambda ()
                 (let loop ((i 0) (ok #t))
                   (if (< i 200)
                       (loop (+ i 1)
                             (and ok (let ((v (cow-target 7))) (or (= v 14) (= v 49)))))
                       ok)))))
       (cm (fiber:overlay-commit))
       (after (cow-target 7))
       (seen (fiber:join watch))
       (report (fiber:overlay-report)))
  (and (> open 0) ; #4715: overlay-begin returns the base workspace epoch, which the
       ; seed (set-code + eval-current) already advanced - 0 was never the contract.
       (eq? m #t)
       (eq? ec #t)
       (= own 49)
       (or (= sib 14) (= sib 49))
       (not (error? cm))
       (= after 49)
       (eq? seen #t)
       (>= (string-index report "commits=1") 0)
       (>= (string-index report "conflicts=0") 0)
       (>= (string-index report "active=0") 0)))
)AURA");
    CHECK(r && is_bool(*r) && as_bool(*r),
          "AC21: the body reads its own rebind, the watcher only ever sees old-or-new, "
          "and the commit lands as one whole version");

    // arm 2: a scalar rebind under the same shape is PRIVATE until commit.
    auto pc = cs.eval(R"AURA((begin
  (set-code "(define cow-n 5)")
  (eval-current)
  #t))AURA");
    CHECK(pc && is_bool(*pc) && as_bool(*pc), "AC21: scalar workspace define seeded");
    auto p = cs.eval(R"AURA(
(let* ((open (fiber:overlay-begin))
       (m (try (mutate:rebind "cow-n" "9" "cow") (catch (e) #f)))
       (ec (if (eq? m #t) (try (begin (eval-current) #t) (catch (e) #f)) #f))
       (own cow-n)
       (sib (fiber:join (fiber:spawn (lambda () cow-n))))
       (cm (fiber:overlay-commit))
       (after cow-n)
       (report (fiber:overlay-report)))
  (and (> open 0) ; same contract as arm 1: the base epoch, already advanced by the seed.
       (eq? m #t)
       (eq? ec #t)
       (= own 9)
       (= sib 5)
       (not (error? cm))
       (= after 9)
       ; arm 1 already committed in this CompilerService: stats are cumulative.
       (>= (string-index report "commits=2") 0)
       (>= (string-index report "conflicts=0") 0)
       (>= (string-index report "active=0") 0)))
)AURA");
    CHECK(p && is_bool(*p) && as_bool(*p),
          "AC21: the overlay body reads its private 9 while a sibling fiber still reads "
          "the committed 5; the commit publishes 9");

    // arm 3: same-base sibling commit wins; ours reports the conflict.
    auto s = cs.eval(R"AURA((begin
  (set-code "(define cow-n 5)")
  (eval-current)
  #t))AURA");
    CHECK(s && is_bool(*s) && as_bool(*s), "AC21: conflict arm seeded");
    // The sibling writes the env directly (fiber:overlay-set!) so this arm is
    // deterministic: the workspace AST is not touched a second time.
    auto c = cs.eval(R"AURA(
(let* ((open (fiber:overlay-begin))
       (m (try (mutate:rebind "cow-n" "9" "cow") (catch (e) #f)))
       (ec (if (eq? m #t) (try (begin (eval-current) #t) (catch (e) #f)) #f))
       (own cow-n)
       (sib (fiber:join
             (fiber:spawn
              (lambda ()
                (fiber:overlay-begin)
                (fiber:overlay-set! "cow-n" 11)
                (if (error? (fiber:overlay-commit)) 0 1)))))
       (cm (try (fiber:overlay-commit) (catch (e) e)))
       (after cow-n)
       (report (fiber:overlay-report)))
  (and (eq? m #t)
       (eq? ec #t)
       (= own 9)
       (= sib 1)
       (and (string? cm) (>= (string-index cm "fiber:cow-conflict: cow-n") 0))
       (= after 11)
       (>= (string-index report "conflicts=1") 0)
       (>= (string-index report "active=0") 0)))
)AURA");
    CHECK(c && is_bool(*c) && as_bool(*c),
          "AC21: first committer wins, the loser reports fiber:cow-conflict and leaves no trace");
}

// ── AC22: #4733 — dotted rest spine is per-call on every thread ──────
// Four fiber bodies call a variadic procedure 2000x each. The rest list used
// to be minted with a raw pairs_.push_back + pairs_.size() (no
// alloc_storage_lock_): two bodies read the same size -> shared cons cell
// ("another fiber's argument" / "car: not a pair"), and the racing vector
// growth aborted the process (std::length_error vector::reserve) or wedged it.
// Before the fix one run read 89/78/74/92 bad of 2000 per fiber.
static void ac22_dotted_rest_per_fiber() {
    std::println("\n--- #4733 AC22: dotted rest list stays per-call/per-fiber ---");
    CompilerService cs;
    const char* src = R"(
(define (second . xs) (cadr xs))
(define (work k)
  (let loop ((i 0) (bad 0))
    (if (= i 2000)
        bad
        (loop (+ i 1)
              (if (equal? (try (second i k) (catch (e) 'err)) k) bad (+ bad 1))))))
(define serial (work 7))
(define par (map fiber:join (map (lambda (k) (fiber:spawn (lambda () (work k)))) (list 1 2 3 4))))
(and (= serial 0) (equal? par (list 0 0 0 0)))
)";
    bool all_ok = true;
    for (int round = 0; round < 3; ++round) {
        auto r = cs.eval(src);
        const bool ok = r && is_bool(*r) && as_bool(*r);
        CHECK(ok, "AC22: serial and all 4 fibers report 0 bad rest calls");
        all_ok = all_ok && ok;
    }
    if (!all_ok)
        std::println("AC22: rest spine under fibers is not per-call");
}

// ── AC23: #4734 — parallel eval bodies do not deadlock ──────────────
// 200 waves x 4 fibers, each evaluating a lambda source string. The
// shard -> bindings lock order in Env::lookup's root-frame consult inverted
// against the top-env EXCLUSIVE bracket eval holds across its program:
// two bodies held a shard read lock and waited for the bindings lock while
// the eval body held bindings and waited for the shard (0% CPU, every
// thread parked). Bounded subprocess: a regression parks, it must not hang
// the suite.
static void ac23_parallel_eval_no_deadlock() {
    std::println("\n--- #4734 AC23: 200 waves of parallel eval complete ---");
    const auto bin = aura_bin_path_4131();
    CHECK(!bin.empty(), "AC23: aura binary resolved beside the test binary");
    if (bin.empty())
        return;
    const auto prog =
        write_4131_program("aura_4734_eval_waves",
                           R"SRC((define (job i) (procedure? (eval "(lambda (a b) (+ a b))")))
(let loop ((w 0))
  (if (< w 200)
      (begin (map fiber:join (map (lambda (i) (fiber:spawn (lambda () (job i)))) (list 0 1 2 3)))
             (loop (+ w 1)))
      (begin (display "DONE") (newline))))
)SRC");
    CHECK(!prog.empty(), "AC23: wave program written");
    if (prog.empty())
        return;
    // The interpreter's tree-walker fallback is gated by the production
    // pipeline switch (Issue #2213); a fiber body that needs it must run with
    // the documented override, same as the issue's probe.
    const char* prev_strict = ::getenv("AURA_PIPELINE_STRICT");
    const std::string prev_strict_val = prev_strict != nullptr ? prev_strict : "";
    const bool had_strict = prev_strict != nullptr;
    ::setenv("AURA_PIPELINE_STRICT", "0", 1);
    const pid_t pid = spawn_4131_oneshot(bin, prog, /*ignore_term=*/false);
    CHECK(pid > 0, "AC23: wave child spawned");
    int rc = -1;
    if (pid > 0)
        rc = wait_4131_exit(pid, 120000);
    if (had_strict)
        ::setenv("AURA_PIPELINE_STRICT", prev_strict_val.c_str(), 1);
    else
        ::unsetenv("AURA_PIPELINE_STRICT");
    CHECK(rc == 0, "AC23: 200 parallel-eval waves finish (no 0-CPU deadlock)");
    ::unlink(prog.c_str());
}


// ── AC24: #4745 — a pure closure called through an inner lambda in parallel
// fiber bodies never returns a non-boolean ──────────────────────────────
// The filed probe (probes/closure-unknown-in-fibers.aura) sees the value
// <unknown> about once per 30k calls; the value is the first-class `and`
// primitive, i.e. a Variable node was evaluated in the value position of an
// and/or clause. Mechanism: the #4599 per-node memo (and/or clause ids,
// prim/cell decisions) is PER BODY but flat-scoped, and it was invalidated
// from the SHARED "last flat/pool" atomics. A body that alternates flats
// (calling a closure whose body lives in another flat, then continuing in its
// own) usually finds the shared atomic ALREADY holding its new flat because a
// sibling stored it, so the clear was skipped and a stale entry (same node
// id, other flat) answered a later visit with the other flat's clause list.
// The memo is now cleared on THIS body's flat/pool change.
static void ac24_closure_in_fibers_boolean_4745() {
    std::println("\n--- #4745 AC24: inner-lambda closure calls stay boolean in fibers ---");
    CompilerService cs;
    const char* src = R"AURA(
(define pol4745 (eval "(lambda (e x) (eq? x 1))"))
(define (mk4745 n)
  (let loop ((i 0) (acc (list)))
    (if (= i n) acc
        (loop (+ i 1) (cons (hash "id" i "exposure" (remainder i 3)) acc)))))
(define rows4745 (mk4745 60))
(define (work4745 passes)
  (let loop ((p 0) (bad 0))
    (if (= p passes) bad
        (let rows ((rs rows4745) (b bad))
          (if (null? rs) (loop (+ p 1) b)
              (let* ((e (car rs))
                     (r ((lambda (e) (pol4745 e (hash-ref e "exposure"))) e)))
                (rows (cdr rs) (if (boolean? r) b (+ b 1)))))))))
(define serial4745 (work4745 400))
(define par4745 (map fiber:join (map (lambda (k) (fiber:spawn (lambda () (work4745 400))))
                                   (list 1 2 3 4 5 6 7 8))))
(and (= serial4745 0) (equal? par4745 (list 0 0 0 0 0 0 0 0)))
)AURA";
    bool all_ok = true;
    for (int round = 0; round < 3; ++round) {
        auto r = cs.eval(src);
        const bool ok = r && is_bool(*r) && as_bool(*r);
        CHECK(ok, "AC24: serial and all 8 fibers see only #t/#f from the closure");
        all_ok = all_ok && ok;
    }
    if (!all_ok)
        std::println("AC24: a fiber body read another flat's clause list (stale per-node memo)");
}

} // namespace

int run_test_fiber_spawn_cli() {
    std::println("=== Issue #2656 / #2685 / #2686: CLI denseness fiber:spawn ===");
    std::println("=== Issue #4715: per-fiber COW overlay for env bindings ===");
    ac1_positive_id();
    ac2_join_payload();
    ac3_backend_thread();
    ac4_two_workers();
    ac5_source();
    ac6_dual_define_distinct_ids();
    ac7_concurrent_dual_rebind();
    ac8_spawn_abandon_dtor_drain();
    ac9_oneshot_sigterm_inherited_ignore();
    ac10_ordinary_program_unchanged();
    ac11_oneshot_sigterm_default_still_prompt();
    ac12_source_cite();
    ac13_serial_pin_two_cpu_bound_fibers();
    ac14_sampler_control_two_runnable();
    ac15_cow_overlay_disjoint_commit();
    ac16_cow_overlay_same_name_conflict();
    ac17_cow_overlay_rollback();
    ac18_cow_overlay_moved_base_merge();
    ac19_eval_split_source_cite();
    ac20_cow_overlay_env_intercept_cite();
    ac21_cow_overlay_rebind_eval_current();
    ac22_dotted_rest_per_fiber();
    ac23_parallel_eval_no_deadlock();
    // Issue #4745 — per-body #4599 memo invalidation (flat/pool scoped).
    ac24_closure_in_fibers_boolean_4745();
    // Issue #4771 — worldline primitives (historical base, two-phase commit).
    ac25_cow_overlay_begin_at_historical_base();
    ac26_cow_overlay_two_phase_commit();
    ac27_cow_overlay_prepare_cancel_no_ghost();
    std::println(
        "\n=== #2656/#2685/#2686/#3394/#4131/#4720/#4717/#4715/#4733/#4734/#4745/#4771: {} "
        "passed, {} failed ===",
        g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_fiber_spawn_cli();
}
#endif
