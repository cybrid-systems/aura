// Issue #5399: the bus task board. A host publishes a task; a worker
// claims it with a lease; a second worker's double claim is refused;
// after the holder is killed the lease expires and the second worker
// claims and completes the task. The tasks channel is readable as
// channel 6. A host unhost/rehost resumes the board from its checkpoint
// and a post-restart worker claims the still-open task.

#include "bus/transport.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failed = 0;
void check(bool c, const char* msg) {
    if (!c) {
        ++g_failed;
        std::printf("FAIL: %s\n", msg);
    }
}

using namespace aura::bus;

std::string aura_binary() {
    char exe[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        const std::string self(exe, static_cast<std::size_t>(n));
        const auto slash = self.find_last_of('/');
        if (slash != std::string::npos)
            return self.substr(0, slash + 1) + "aura";
    }
    return "./aura";
}

bool write_file(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fputs(text, f);
    std::fclose(f);
    return true;
}

pid_t run_script(const std::string& script, int* out_fd, int* in_fd) {
    int out[2], in[2];
    if (pipe(out) != 0 || pipe(in) != 0)
        return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        ::close(out[0]);
        ::close(in[1]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        dup2(in[0], STDIN_FILENO);
        ::close(out[1]);
        ::close(in[0]);
        setenv("AURA_SANDBOX", "off", 1);
        const std::string exe = aura_binary();
        execl(exe.c_str(), "aura", script.c_str(), static_cast<char*>(nullptr));
        _exit(24);
    }
    ::close(out[1]);
    ::close(in[0]);
    *out_fd = out[0];
    *in_fd = in[1];
    return pid;
}

std::map<int, std::string> g_pending;

std::string read_until(int fd, const char* marker, int timeout_ms) {
    std::string& buffer = g_pending[fd];
    const std::size_t len = std::strlen(marker);
    int waited = 0;
    while (buffer.find(marker) == std::string::npos && waited < timeout_ms) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 50);
        waited += 50;
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char buf[512];
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            buffer.append(buf, static_cast<std::size_t>(n));
        }
    }
    const std::size_t at = buffer.find(marker);
    if (at == std::string::npos)
        return std::string();
    std::string out = buffer.substr(0, at + len);
    buffer.erase(0, at + len);
    return out;
}

std::string read_all(int fd) {
    std::string s = g_pending[fd];
    g_pending.erase(fd);
    char buf[512];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        s.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    return s;
}

bool has(const std::string& out, const char* line) {
    return out.find(line) != std::string::npos;
}

bool wait_socket(const std::string& sock) {
    for (int i = 0; i < 200; ++i) {
        struct stat st{};
        if (::stat(sock.c_str(), &st) == 0)
            return true;
        usleep(50000);
    }
    return false;
}

const char* kHostScript = R"AURA(
(require "std/bus" all:)
(define h (bus:host 8903))
(display (if (bus:ok? h) "host-ready" "host-FAIL")) (newline)
(define msid 0)
(define ms (bus:attach "" 8903 0 255 255))
(set! msid (car (cdr ms)))
(define gated-id 0)
(define (show tag r) (display tag) (display "=") (display r) (newline))
; #5407: an unknown channel is the same refusal from subscribe and query.
(define sub0 (bus:subscribe msid 0))
(display "sub0=") (display (bus:code sub0)) (display ":") (display (bus:message sub0)) (newline)
(define q0 (bus:query msid 0 0 0))
(display "q0=") (display (bus:code q0)) (display ":") (display (bus:message q0)) (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "unhost")
       (bus:detach msid)
       (bus:unhost 8903)
       (display "unhosted") (newline)
       (loop))
      ((string=? l "host")
       (define h2 (bus:host 8903))
       (define ms2 (bus:attach "" 8903 0 255 255))
       (set! msid (car (cdr ms2)))
       (display "hosted") (newline)
       (loop))
      ((string=? l "post")
       (show "posted" (bus:post-task msid 0 77))
       (loop))
      ((string=? l "status1")
       (show "status1" (bus:task-status msid 1))
       (loop))
      ((string=? l "status2")
       (show "status2" (bus:task-status msid 2))
       (loop))
      ((string=? l "events")
       (show "events" (bus:query msid 6 0 0))
       (loop))
      ((string=? l "gated")
       (define p (bus:post-task msid 0 88 1))
       (set! gated-id (list-ref p 1))
       (show "gated-post" p)
       (show "gated-contract" (bus:contract msid gated-id 42 100))
       (show "gated-approve" (bus:approve msid gated-id 1 0))
       (show "gated-claim" (bus:claim msid 900 gated-id))
       (show "gated-charge" (bus:budget-charge msid gated-id 5000))
       (loop))
      ((string=? l "gates")
       (show "gates-contract" (bus:contract-status msid gated-id))
       (show "gates-budget" (bus:budget-report msid gated-id))
       (show "gates-claim" (bus:claim msid 5000 gated-id))
       (show "gates-complete" (bus:complete msid gated-id 0))
       (loop))
      ((string=? l "sources")
       (define (fill i)
         (if (< i 1026)
             (let ((r (bus:publish-source msid "x")))
               (if (bus:ok? r)
                   (fill (+ i 1))
                   (show "source-stop" (bus:message r))))
             (show "source-stop" "unexpected")))
       (fill 1)
       (show "capacity-src" (bus:capacity-plan msid))
       (loop))
      ((string=? l "quit")
       (bus:detach msid)
       (bus:unhost 8903)
       (display "host-quit") (newline))
      (else (loop)))))
)AURA";

const char* kWorkerA = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8903 0 127 255))
(define sid (car (cdr s)))
(define t (bus:post-task sid 0 77))
(display "posted=") (write t) (newline)
(define c (bus:claim sid 500))
(display "claim=") (display c) (newline)
(let loop ()
  (let ((l (read-line)))
    (if (eof-object? l) #f (loop))))
)AURA";

const char* kWorkerB = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8903 0 127 255))
(define sid (car (cdr s)))
(define l1 (read-line))
(define id (string->number (substring l1 3 (string-length l1))))
(define bad (bus:claim sid 500 id))
(display "double=") (display (bus:code bad)) (display ":") (display (bus:message bad)) (newline)
(define l2 (read-line))
(define c2 (bus:claim sid 500 id))
(display "reclaim=") (display c2) (newline)
(define done (bus:complete sid id 42))
(display "done=") (display done) (newline)
(bus:detach sid)
(display "B-DONE") (newline)
)AURA";

const char* kWorkerC = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8903 0 127 255))
(define sid (car (cdr s)))
(define st (bus:task-status sid 2))
(display "status2=") (display st) (newline)
(define c (bus:claim sid 500 2))
(display "claim2=") (display c) (newline)
(define done (bus:complete sid 2 99))
(display "done2=") (display done) (newline)
(bus:detach sid)
(display "C-DONE") (newline)
)AURA";

// #5440: a fresh board with one open task, then the host stops so the
// test can remove the gate ledger.
const char* kGateSeed = R"AURA(
(require "std/bus" all:)
(define h (bus:host 8904))
(display "seed-host=") (display (bus:ok? h)) (newline)
(define s (car (cdr (bus:attach "" 8904 0 255 255))))
(define t (bus:post-task s 0 1))
(display "seed-task=") (display (list-ref t 1)) (newline)
(bus:detach s)
(display "seed-unhost=") (display (bus:ok? (bus:unhost 8904))) (newline)
)AURA";

// The first restart after the ledger loss: claim 1 fails closed, then a
// re-issued contract + approval recovers it. Task 2 is posted while the
// host is in the lost state and must survive the next restart.
const char* kGateReissue = R"AURA(
(require "std/bus" all:)
(define (show label value) (display label) (display "=") (display value) (display "|") (newline))
(define h (bus:host 8904))
(show "reissue-host" (bus:ok? h))
(define s (car (cdr (bus:attach "" 8904 0 255 255))))
(define lost (bus:claim s 5000 1))
(show "lost-claim" (bus:message lost))
(define c (bus:contract s 1 42 100))
(show "reissue-contract" (car c))
(define ap (bus:approve s 1 1 0))
(show "reissue-approve" (car ap))
(define after (bus:claim s 5000 1))
(show "recovered-claim" (if (bus:ok? after) (list-ref after 1) (bus:message after)))
(define ev (bus:contract-evidence s 1 1 100))
(show "recovered-evidence" (car ev))
(define done (bus:complete s 1 7))
(show "recovered-complete" (car done))
(define t2 (bus:post-task s 0 2))
(show "new-task" (list-ref t2 1))
(bus:detach s)
(show "reissue-unhost" (bus:ok? (bus:unhost 8904)))
)AURA";

// #5440: a global budget trip persisted with a low limit, then a host
// started with a much higher limit must clear it at startup.
const char* kBudgetSeed = R"AURA(
(require "std/bus" all:)
(define (show label value) (display label) (display "=") (display value) (display "|") (newline))
(define h (bus:host 8906))
(define s (car (cdr (bus:attach "" 8906 0 255 255))))
(define t (bus:post-task s 0 1))
(show "budget-seed-task" (list-ref t 1))
(define c (bus:claim s 60000 1))
(show "budget-seed-claim" (bus:ok? c))
(define ch (bus:budget-charge s 1 11))
(show "budget-seed-charge" (bus:message ch))
(define r (bus:budget-report s 0))
(show "budget-seed-flags" (list-ref r 8))
(bus:detach s)
(show "budget-seed-unhost" (bus:ok? (bus:unhost 8906)))
)AURA";

const char* kBudgetRecover = R"AURA(
(require "std/bus" all:)
(define (show label value) (display label) (display "=") (display value) (display "|") (newline))
(define h (bus:host 8906))
(define s (car (cdr (bus:attach "" 8906 0 255 255))))
(define t (bus:post-task s 0 2))
(define c (bus:claim s 60000 (list-ref t 1)))
(show "budget-recover-claim" (if (bus:ok? c) (list-ref c 1) (bus:message c)))
(define r (bus:budget-report s 0))
(show "budget-recover-flags" (list-ref r 8))
(bus:detach s)
(show "budget-recover-unhost" (bus:ok? (bus:unhost 8906)))
)AURA";

// The normal restart after recovery: task 2 (posted while the ledger was
// lost) and a task posted now must both be claimable, not gate-state-lost.
const char* kGateRestart = R"AURA(
(require "std/bus" all:)
(define (show label value) (display label) (display "=") (display value) (display "|") (newline))
(define h (bus:host 8904))
(display "restart-host=") (display (bus:ok? h)) (newline)
(define s (car (cdr (bus:attach "" 8904 0 255 255))))
(define c2 (bus:claim s 5000 2))
(show "new-task-claim" (if (bus:ok? c2) (list-ref c2 1) (bus:message c2)))
(define t3 (bus:post-task s 0 3))
(define c3 (bus:claim s 5000 (list-ref t3 1)))
(show "post-restart-claim" (if (bus:ok? c3) (list-ref c3 1) (bus:message c3)))
(bus:detach s)
(show "restart-unhost" (bus:ok? (bus:unhost 8904)))
)AURA";

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-tasks-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8903);

    check(write_file(d + "/host.aura", kHostScript), "write host");
    check(write_file(d + "/a.aura", kWorkerA), "write worker A");
    check(write_file(d + "/b.aura", kWorkerB), "write worker B");
    check(write_file(d + "/c.aura", kWorkerC), "write worker C");
    check(write_file(d + "/gate-seed.aura", kGateSeed), "write gate seed");
    check(write_file(d + "/gate-reissue.aura", kGateReissue), "write gate reissue");
    check(write_file(d + "/gate-restart.aura", kGateRestart), "write gate restart");
    check(write_file(d + "/budget-seed.aura", kBudgetSeed), "write budget seed");
    check(write_file(d + "/budget-recover.aura", kBudgetRecover), "write budget recover");

    int host_out = -1, host_in = -1;
    const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
    check(host > 0, "fork host");
    check(wait_socket(sock), "host socket appeared");
    check(has(read_until(host_out, "host-ready", 10000), "host-ready"), "host started");
    // #5407: subscribe and query refuse an unknown channel identically.
    const std::string unknown =
        read_until(host_out, "q0=", 10000) + read_until(host_out, "\n", 10000);
    check(has(unknown, "sub0=permission:unknown channel"),
          "subscribe unknown channel is permission/unknown channel");
    check(has(unknown, "q0=permission:unknown channel"),
          "query unknown channel is permission/unknown channel");

    int a_out = -1, a_in = -1;
    const pid_t a = run_script(d + "/a.aura", &a_out, &a_in);
    check(a > 0, "fork worker A");
    const std::string a_claim = read_until(a_out, "claim=", 10000) + read_until(a_out, "\n", 10000);
    check(has(a_claim, "claim=(ok 1 1"), "worker A claimed task 1");

    int b_out = -1, b_in = -1;
    const pid_t b = run_script(d + "/b.aura", &b_out, &b_in);
    check(b > 0, "fork worker B");
    const char* idline = "id=1\n";
    check(::write(b_in, idline, std::strlen(idline)) == static_cast<ssize_t>(std::strlen(idline)),
          "send task id to B");
    const std::string b_double = read_until(b_out, "\n", 10000);
    check(has(b_double, "double=validation-failed:task-claimed"),
          "double claim refused with task-claimed");

    // Kill the holder: its lease is not released, so the board keeps
    // the task claimed until the deadline.
    ::kill(a, SIGKILL);
    int a_status = 0;
    waitpid(a, &a_status, 0);
    ::close(a_in);
    ::close(a_out);
    usleep(900000);
    const char* go = "go\n";
    check(::write(b_in, go, std::strlen(go)) == static_cast<ssize_t>(std::strlen(go)),
          "send go to B");
    const std::string b_done = read_until(b_out, "B-DONE", 15000);
    check(has(b_done, "reclaim=(ok 1 1"), "worker B re-claimed after lease expiry");
    check(has(b_done, "done=(ok 1 2"), "worker B completed task 1");
    int b_status = 0;
    waitpid(b, &b_status, 0);
    ::close(b_in);
    ::close(b_out);

    // Post a second task, read the tasks channel, then restart the
    // host and prove the open task survived the checkpoint.
    check(::write(host_in, "post\n", 5) == 5, "send post");
    const std::string posted =
        read_until(host_out, "posted=", 10000) + read_until(host_out, "\n", 10000);
    check(has(posted, "posted=(ok 2 0"), "posted task 2 open");
    check(::write(host_in, "status2\n", 8) == 8, "send status2");
    const std::string status_before =
        read_until(host_out, "status2=", 10000) + read_until(host_out, "\n", 10000);
    check(has(status_before, "status2=(ok 2 0"), "task 2 is open before restart");
    check(::write(host_in, "events\n", 7) == 7, "send events");
    const std::string events =
        read_until(host_out, "events=", 10000) + read_until(host_out, "\n", 10000);
    check(has(events, "events=(ok"), "tasks channel 6 is readable");

    check(::write(host_in, "unhost\n", 7) == 7, "send unhost");
    check(has(read_until(host_out, "unhosted", 10000), "unhosted"), "host unhosted");
    check(::write(host_in, "host\n", 5) == 5, "send host");
    check(has(read_until(host_out, "hosted", 10000), "hosted"), "host rehosted");
    check(::write(host_in, "status2\n", 8) == 8, "send status2 after restart");
    const std::string status_after =
        read_until(host_out, "status2=", 10000) + read_until(host_out, "\n", 10000);
    check(has(status_after, "status2=(ok 2 0"), "task 2 survived the host restart");

    int c_out = -1, c_in = -1;
    const pid_t c = run_script(d + "/c.aura", &c_out, &c_in);
    check(c > 0, "fork worker C");
    const std::string c_out_text = read_until(c_out, "C-DONE", 15000);
    check(has(c_out_text, "status2=(ok 2 0"), "worker C sees the restored open task");
    check(has(c_out_text, "claim2=(ok 2 1"), "worker C claims the restored task");
    check(has(c_out_text, "done2=(ok 2 2"), "worker C completes the restored task");
    int c_status = 0;
    waitpid(c, &c_status, 0);
    ::close(c_in);
    ::close(c_out);

    // #5427: a gated task's contract, approval and budget survive the
    // host restart. The restored task still refuses a zero-evidence
    // completion (the gate is fail-closed, not fail-open).
    check(::write(host_in, "gated\n", 6) == 6, "send gated");
    const std::string gated =
        read_until(host_out, "gated-charge=", 10000) + read_until(host_out, "\n", 10000);
    check(has(gated, "gated-post=(ok 3"), "the gated task is posted");
    check(has(gated, "gated-contract=(ok"), "the contract is created");
    check(has(gated, "gated-approve=(ok"), "the contract is approved");
    check(has(gated, "gated-claim=(ok 3"), "the gated task is claimed");
    check(has(gated, "gated-charge=(ok"), "the task budget is charged");
    check(::write(host_in, "unhost\n", 7) == 7, "send unhost with gates");
    check(has(read_until(host_out, "unhosted", 10000), "unhosted"), "host unhosted with gates");
    check(::write(host_in, "host\n", 5) == 5, "send host with gates");
    check(has(read_until(host_out, "hosted", 10000), "hosted"), "host rehosted with gates");
    // The unhost may stop the coordinator before its Disconnect frame is
    // served; the explicit 900 ms lease then expires shortly after.
    usleep(1200000);
    check(::write(host_in, "gates\n", 6) == 6, "send gates");
    const std::string gates =
        read_until(host_out, "gates-complete=", 10000) + read_until(host_out, "\n", 10000);
    check(has(gates, "gates-contract=(ok"), "the contract survived the restart");
    check(has(gates, "gates-budget=(ok") && has(gates, "5000"),
          "the spent budget survived the restart");
    check(has(gates, "gates-claim=(ok 3"), "the restored gated task is claimable");
    check(has(gates, "gates-complete=(error") && has(gates, "contract has no evidence"),
          "the restored contract refuses a zero-evidence completion");

    // #5428: the string table is a capacity wall too. Filling it makes
    // the plan report zero source slots and flag low before a candidate
    // loop would die on an unexplained append failure.
    check(::write(host_in, "sources\n", 8) == 8, "send sources");
    const std::string sources = read_until(host_out, "source-stop=", 20000) +
                                read_until(host_out, "capacity-src=", 20000) +
                                read_until(host_out, "\n", 20000);
    check(has(sources, "string table slots exhausted"), "the source limit names the string table");
    check(has(sources, "0/1024 left"), "the source limit reports the remaining slots");
    check(has(sources, "capacity-src=(ok 0 1"), "the plan flags low and counts zero rounds");

    check(::write(host_in, "quit\n", 5) == 5, "send quit");
    const std::string host_out_text = read_all(host_out);
    int h_status = 0;
    waitpid(host, &h_status, 0);
    check(has(host_out_text, "host-quit"), "host quit cleanly");
    struct stat st{};
    check(::stat(sock.c_str(), &st) != 0, "socket unlinked");

    // ── #5440: lost gates recover, and the loss does not spread ──
    const auto run_once = [&](const char* name) {
        int out = -1, in = -1;
        const pid_t pid = run_script(d + "/" + name, &out, &in);
        if (pid <= 0)
            return std::string();
        ::close(in);
        std::string text = read_all(out);
        int status = 0;
        waitpid(pid, &status, 0);
        return text;
    };
    {
        const std::string seed = run_once("gate-seed.aura");
        check(has(seed, "seed-task=1"), "the seed board has task 1");
        const std::string gate_sock = default_socket_path(8904);
        check(::unlink((gate_sock + ".tasks.gates").c_str()) == 0,
              "the gate ledger was removed before the restart");
    }
    {
        const std::string text = run_once("gate-reissue.aura");
        if (!has(text, "lost-claim=gate-state-lost"))
            std::printf("GATE-REISSUE-OUT:[%s]\n", text.c_str());
        check(has(text, "lost-claim=gate-state-lost"),
              "a restored task fails closed while its gates are lost");
        check(has(text, "reissue-contract=ok"), "the contract can be re-issued");
        check(has(text, "reissue-approve=ok"), "the re-issued contract can be approved");
        check(has(text, "recovered-claim=1"), "the recovered task is claimable again");
        check(has(text, "recovered-evidence=ok"), "the recovered task records evidence");
        check(has(text, "recovered-complete=ok"), "the recovered task completes");
        check(has(text, "new-task=2"), "task 2 is posted in the recovered host");
    }
    {
        const std::string text = run_once("gate-restart.aura");
        if (!has(text, "new-task-claim=2"))
            std::printf("GATE-RESTART-OUT:[%s]\n", text.c_str());
        check(has(text, "new-task-claim=2"),
              "task 2 survived the next restart without gate-state-lost");
        check(has(text, "post-restart-claim=3"),
              "a task posted after recovery is claimable after a restart");
    }

    // ── #5440: raising the global budget clears a persisted trip ──
    {
        check(setenv("AURA_BUS_GLOBAL_TOKEN_LIMIT", "10", 1) == 0, "set low global limit");
        const std::string seed = run_once("budget-seed.aura");
        if (!has(seed, "budget-seed-charge=global token budget exceeded"))
            std::printf("BUDGET-SEED-OUT:[%s]\n", seed.c_str());
        check(has(seed, "budget-seed-charge=global token budget exceeded"),
              "the low global limit trips on a charge");
        check(has(seed, "budget-seed-flags=3"), "the global trip is reported");
        check(setenv("AURA_BUS_GLOBAL_TOKEN_LIMIT", "100000", 1) == 0, "raise the global limit");
        const std::string text = run_once("budget-recover.aura");
        if (!has(text, "budget-recover-claim=2"))
            std::printf("BUDGET-RECOVER-OUT:[%s]\n", text.c_str());
        check(has(text, "budget-recover-claim=2"),
              "raising the global limit clears the persisted trip");
        check(has(text, "budget-recover-flags=0"), "the recovered global ledger is clean");
        unsetenv("AURA_BUS_GLOBAL_TOKEN_LIMIT");
    }

    if (g_failed == 0)
        std::printf("test_bus_tasks: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
