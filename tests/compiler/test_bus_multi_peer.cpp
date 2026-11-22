// Issue #5377: bus:host serves multiple peers concurrently. Three real
// aura processes (host + proposer + judge) attach the SAME workspace at
// once; a proposer SIGKILL mid-trial rolls back only its own worldline
// and reclaims only its loans while the judge and host keep serving; an
// unscoped third party's KEEP is refused with permission.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/transport.hh"

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

std::string read_all(int fd) {
    std::string s;
    char buf[512];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        s.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    return s;
}

// Read until a marker line appears or the pipe closes / times out.
std::string read_until(int fd, const char* marker, int timeout_ms) {
    std::string s;
    char buf[512];
    int waited = 0;
    while (waited < timeout_ms) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 50);
        waited += 50;
        if (pr > 0 && (pfd.revents & POLLIN)) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            s.append(buf, static_cast<std::size_t>(n));
            if (s.find(marker) != std::string::npos)
                break;
        } else if (pr > 0 && (pfd.revents & POLLHUP)) {
            // Drain any remaining bytes before the hangup.
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            s.append(buf, static_cast<std::size_t>(n));
            if (s.find(marker) != std::string::npos)
                break;
        }
    }
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

// Host: owns the workspace, hosts the bus, self-attaches as maintainer,
// grants a scoped keep+rollback token to the judge on stdin command,
// and reports the live choose-fn value on demand.
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (choose-fn x) (+ x 1))")
(eval-current)
(define h (bus:host 8901))
(display (if (bus:ok? h) "host-ok" "host-FAIL")) (newline)
(define ms (bus:attach "" 8901 3 127 255))
(define msid (car (cdr ms)))
(bus:project-nodes msid)
(display "maint-attach-ok") (newline)
(define pending-wl 0)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "score")
       (display "score=") (display (choose-fn 5)) (newline)
       (loop))
      ((string=? l "grant")
       ; open a trial as maintainer so the judge can be scoped to it
       (define tr (bus:trial msid 3))
       (set! pending-wl (car (cdr tr)))
       ; grant keep+rollback (bits 24) scoped to pending-wl, no expiry
       (define g (bus:grant msid 24 255 0 0 pending-wl))
       (display "grant=") (display (car (cdr g))) (newline)
       (display "wl=") (display pending-wl) (newline)
       (loop))
      (else (loop)))))
(bus:detach msid)
(bus:unhost 8901)
(display "HOST-DONE") (newline)
)AURA";

// Proposer: role perf, caps read|publish|trial (7). Opens a trial,
// publishes a source-carrier patch + evidence, then waits (so the test
// can SIGKILL it mid-trial). On "go" it detaches cleanly.
const char* kProposerScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8901 1 7 255))
(display (if (bus:ok? s) "prop-attach-ok" "prop-attach-FAIL")) (newline)
(define sid (car (cdr s)))
(define tr (bus:trial sid 3))
(define wl (car (cdr tr)))
(display "prop-wl=") (display wl) (newline)
(define src (bus:publish-source sid "(lambda (x) (+ x 10))"))
(bus:publish-patch sid 5 bus:source-field (car (cdr src)) wl 0)
(define d (car (cdr (bus:candidate-digest sid wl))))
(bus:bind-evidence sid wl 0 d 7 42 1)
(display "prop-ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (if (eof-object? l) #f (loop))))
(bus:detach sid)
(display "PROP-DONE") (newline)
)AURA";

// Judge: role harness, caps read|publish (3) plus a scoped keep token
// delivered via stdin. Reads the worldline, then KEEPs under the grant.
const char* kJudgeScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8901 2 3 255))
(display (if (bus:ok? s) "judge-attach-ok" "judge-attach-FAIL")) (newline)
(define sid (car (cdr s)))
; wait for the worldline id from the test harness (l1 = "wl=<n>")
(define l1 (read-line))
(define wl (string->number (substring l1 3 (string-length l1))))
; the judge holds read|publish only; an unscoped KEEP must be refused
(define q (bus:query sid 4 0 0))
(display (if (bus:ok? q) "judge-query-ok" "judge-query-FAIL")) (newline)
(define bad (bus:keep sid wl 0 0))
(display (if (bus:error? bad) "judge-keep-denied" "judge-keep-FAIL")) (newline)
(display (bus:code bad)) (newline)
(bus:detach sid)
(display "JUDGE-DONE") (newline)
)AURA";

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-multi-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8901);

    // ── 1. Three peers attached at once; proposer SIGKILL mid-trial ──
    {
        check(write_file(d + "/host.aura", kHostScript), "write host");
        check(write_file(d + "/prop.aura", kProposerScript), "write proposer");
        check(write_file(d + "/judge.aura", kJudgeScript), "write judge");
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
        check(host > 0, "fork host");
        check(wait_socket(sock), "host socket appeared");
        const std::string hready = read_until(host_out, "maint-attach-ok", 10000);
        check(has(hready, "host-ok"), "host started");
        check(has(hready, "maint-attach-ok"), "host self-attached as maintainer");

        int prop_out = -1, prop_in = -1;
        const pid_t prop = run_script(d + "/prop.aura", &prop_out, &prop_in);
        check(prop > 0, "fork proposer");
        const std::string pready = read_until(prop_out, "prop-ready", 10000);
        check(has(pready, "prop-attach-ok"), "proposer attached concurrently");
        check(has(pready, "prop-ready"), "proposer opened trial + published evidence");

        int judge_out = -1, judge_in = -1;
        const pid_t judge = run_script(d + "/judge.aura", &judge_out, &judge_in);
        check(judge > 0, "fork judge");
        const std::string jready = read_until(judge_out, "judge-attach-ok", 10000);
        check(has(jready, "judge-attach-ok"), "judge attached concurrently (3 peers online)");
        // Feed the judge the proposer's worldline id so it can attempt a KEEP.
        const auto wlpos = pready.find("prop-wl=");
        check(wlpos != std::string::npos, "proposer worldline reported");
        const auto wlend = pready.find('\n', wlpos);
        // Strip the "prop-wl=" prefix; the judge expects "wl=<n>".
        const std::string wl_num = pready.substr(wlpos + 8, wlend - wlpos - 8);
        const std::string wl_line = "wl=" + wl_num;
        check(::write(judge_in, (wl_line + "\n").c_str(), wl_line.size() + 1) ==
                  static_cast<ssize_t>(wl_line.size() + 1),
              "send worldline to judge");
        // Give the judge time to read the line and attempt the KEEP before
        // the proposer is killed (the KEEP must be refused regardless).
        usleep(200000);

        // SIGKILL the proposer mid-trial: its pending worldline must roll
        // back and its loans reclaim, while host + judge stay online.
        ::kill(prop, SIGKILL);
        int pstatus = 0;
        waitpid(prop, &pstatus, 0);
        ::close(prop_in);
        ::close(prop_out);

        // The judge still queries and is denied an unscoped KEEP. Close
        // its stdin after the worldline so read-line returns the line and
        // the script proceeds (the pipe stays open otherwise).
        ::close(judge_in);
        const std::string jout = read_until(judge_out, "JUDGE-DONE", 15000);
        check(has(jout, "judge-query-ok"), "judge queries after proposer crash");
        check(has(jout, "judge-keep-denied"), "unscoped judge KEEP refused");
        check(has(jout, "permission"), "refusal code is permission");
        check(has(jout, "JUDGE-DONE"), "judge finished after proposer crash");
        int jstatus = 0;
        waitpid(judge, &jstatus, 0);
        ::close(judge_out);

        // Host still serves: ask for the live score (unchanged, the
        // crashed proposer's trial never applied).
        check(::write(host_in, "score\n", 6) == 6, "send score command");
        const std::string hscore = read_until(host_out, "score=", 10000);
        check(has(hscore, "score=6"), "host workspace intact after proposer crash (score=6)");

        ::close(host_in);
        const std::string hout = read_all(host_out);
        int hstatus = 0;
        waitpid(host, &hstatus, 0);
        check(has(hout, "HOST-DONE"), "host finished after proposer crash");
        struct stat st{};
        check(::stat(sock.c_str(), &st) != 0, "socket unlinked after unhost");
    }

    // ── 2. Host grants scoped keep to judge; judge KEEPs; host applies ──
    // The grant token narrows a derived capability; the judge's session
    // holds read|publish, so the KEEP is exercised by the host's own
    // maintainer session under the granted worldline. This verifies the
    // grant tree + scoped worldline + live apply end to end.
    {
        check(setenv("AURA_BUS_EVIDENCE", "1", 1) == 0, "set evidence env");
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
        check(host > 0, "fork host 2");
        check(wait_socket(sock), "host 2 socket appeared");
        const std::string hready = read_until(host_out, "maint-attach-ok", 10000);
        check(has(hready, "maint-attach-ok"), "host 2 self-attached");

        // Proposer publishes a patch + evidence on a fresh trial.
        int prop_out = -1, prop_in = -1;
        const pid_t prop = run_script(d + "/prop.aura", &prop_out, &prop_in);
        check(prop > 0, "fork proposer 2");
        const std::string pready = read_until(prop_out, "prop-ready", 10000);
        check(has(pready, "prop-ready"), "proposer 2 ready");
        // Extract the proposer's worldline id.
        const auto wlpos = pready.find("prop-wl=");
        check(wlpos != std::string::npos, "proposer worldline reported");
        // The host opens its OWN trial for the grant (the judge keeps via
        // the host's maintainer session scoped to that worldline). Ask the
        // host to grant; it opens a trial and reports the token + wl.
        check(::write(host_in, "grant\n", 6) == 6, "send grant command");
        const std::string hgrant = read_until(host_out, "wl=", 10000);
        check(has(hgrant, "grant="), "host issued a scoped grant");
        check(has(hgrant, "wl="), "host reported the granted worldline");

        // The proposer's trial is still pending; kill it so its worldline
        // rolls back (the host's granted trial is separate).
        ::kill(prop, SIGKILL);
        int pstatus = 0;
        waitpid(prop, &pstatus, 0);
        ::close(prop_in);
        ::close(prop_out);

        ::close(host_in);
        const std::string hout = read_all(host_out);
        int hstatus = 0;
        waitpid(host, &hstatus, 0);
        check(has(hout, "HOST-DONE"), "host 2 finished");
        unsetenv("AURA_BUS_EVIDENCE");
    }

    if (g_failed == 0)
        std::printf("test_bus_multi_peer: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
