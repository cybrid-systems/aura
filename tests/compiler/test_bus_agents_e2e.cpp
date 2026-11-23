// Issue #5406: the multi-agent end-to-end lane, mock LLM only.
//
// One host, one planner, four mock workers, then four fault paths:
//   * a worker killed mid-task: the lease expires and another worker
//     claims the task (no double hold);
//   * the host restarts: the open task survives the checkpoint;
//   * a task budget overrun: the fuse trips and the task fails closed;
//   * an approval timeout: the task is denied and cannot be claimed.
//
// No network and no key: every worker runs `aura agent --mock`. The
// audit replay must still reconstruct the timeline.

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

pid_t spawn_aura(const std::string& first, const std::string& second, int* out_fd, int* in_fd) {
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
        if (second.empty())
            execl(exe.c_str(), "aura", first.c_str(), static_cast<char*>(nullptr));
        else
            execl(exe.c_str(), "aura", first.c_str(), second.c_str(), static_cast<char*>(nullptr));
        _exit(24);
    }
    ::close(out[1]);
    ::close(in[0]);
    *out_fd = out[0];
    *in_fd = in[1];
    return pid;
}

pid_t run_script(const std::string& script, int* out_fd, int* in_fd) {
    return spawn_aura(script, "", out_fd, in_fd);
}

// `aura agent --role worker --workspace N --mock`
pid_t run_worker(std::uint64_t workspace, int* out_fd, int* in_fd) {
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
        const std::string ws = std::to_string(workspace);
        execl(exe.c_str(), "aura", "agent", "--role", "worker", "--workspace", ws.c_str(), "--mock",
              static_cast<char*>(nullptr));
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

int count_occurrences(const std::string& text, const char* needle) {
    int n = 0;
    const std::size_t len = std::strlen(needle);
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + len))
        ++n;
    return n;
}

const char* kHostScript = R"AURA(
(require "std/bus" all:)
(define h (bus:host 8920))
(display (if (bus:ok? h) "host-ready" "host-FAIL")) (newline)
(define msid 0)
(define (attach-host)
  (define s (bus:attach "" 8920 0 255 255))
  (set! msid (car (cdr s))))
(attach-host)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "post")
       (define p (bus:post-task msid 0 77))
       (display "posted=") (write (list-ref p 1)) (display "|") (newline)
       (loop))
      ((string=? l "unhost")
       (bus:detach msid)
       (bus:unhost 8920)
       (display "unhosted") (newline)
       (loop))
      ((string=? l "host")
       (define h2 (bus:host 8920))
       (attach-host)
       (display "hosted") (newline)
       (loop))
      ((string=? l "quit")
       (bus:detach msid)
       (bus:unhost 8920)
       (display "host-quit") (newline))
      (else (loop)))))
)AURA";

const char* kApproveScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8920 0 255 255))
(define sid (car (cdr s)))
(define (ok? r) (string=? (car r) "ok"))
(display "approve1=") (display (ok? (bus:approve sid 1 1 0))) (display "|") (newline)
(display "approve2=") (display (ok? (bus:approve sid 2 1 0))) (display "|") (newline)
(display "approve3=") (display (ok? (bus:approve sid 3 1 0))) (display "|") (newline)
(display "approve4=") (display (ok? (bus:approve sid 4 1 0))) (display "|") (newline)
(display "APPROVED") (newline)
(bus:detach sid)
)AURA";

const char* kVerifyScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8920 0 255 255))
(define sid (car (cdr s)))
(define (state id) (list-ref (bus:task-status sid id) 2))
(display "s1=") (display (state 1)) (display "|") (newline)
(display "s2=") (display (state 2)) (display "|") (newline)
(display "s3=") (display (state 3)) (display "|") (newline)
(display "s4=") (display (state 4)) (display "|") (newline)
(define r (bus:audit-replay sid))
(display "findings=") (display (list-ref r 4)) (display "|") (newline)
(display "VERIFY-DONE") (newline)
(bus:detach sid)
)AURA";

const char* kHangScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8920 5 255 255))
(define sid (car (cdr s)))
(define c (bus:claim sid 900))
(display "hang-claimed=") (display (if (bus:ok? c) (list-ref c 1) (bus:message c))) (display "|") (newline)
(read-line)
)AURA";

const char* kQuotaScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8920 5 255 255))
(define sid (car (cdr s)))
(define c (bus:claim sid 5000))
(display "quota-claim=") (display (if (bus:ok? c) (list-ref c 1) (bus:message c))) (display "|") (newline)
(define charge (bus:budget-charge sid (list-ref c 1) 2000001))
(display "quota-charge=") (display (car charge)) (display ":") (display (if (bus:ok? charge) "ok" (bus:message charge))) (display "|") (newline)
(define done (bus:complete sid (list-ref c 1) 9))
(display "quota-complete=") (display (car done)) (display ":") (display (if (bus:ok? done) "ok" (bus:message done))) (display "|") (newline)
(bus:detach sid)
)AURA";

const char* kTimeoutRequestScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8920 0 255 255))
(define sid (car (cdr s)))
(define p (bus:post-task sid 0 77 1))
(define id (list-ref p 1))
(display "timeout-task=") (display id) (display "|") (newline)
(display "timeout-request=") (display (car (bus:approve sid id 0 50))) (display "|") (newline)
(bus:detach sid)
)AURA";

const char* kTimeoutClaimScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8920 5 255 255))
(define sid (car (cdr s)))
(define c (bus:claim sid 5000 8))
(display "timeout-claim=") (display (car c)) (display ":") (display (if (bus:ok? c) "ok" (bus:message c))) (display "|") (newline)
(bus:detach sid)
)AURA";

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-agents-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8920);

    check(write_file(d + "/host.aura", kHostScript), "write host");
    check(write_file(d + "/approve.aura", kApproveScript), "write approve");
    check(write_file(d + "/verify.aura", kVerifyScript), "write verify");
    check(write_file(d + "/hang.aura", kHangScript), "write hang");
    check(write_file(d + "/quota.aura", kQuotaScript), "write quota");
    check(write_file(d + "/timeout-request.aura", kTimeoutRequestScript), "write timeout request");
    check(write_file(d + "/timeout-claim.aura", kTimeoutClaimScript), "write timeout claim");

    int host_out = -1, host_in = -1;
    const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
    check(host > 0, "fork host");
    check(wait_socket(sock), "host socket appeared");
    check(has(read_until(host_out, "host-ready", 10000), "host-ready"), "host started");

    // ── main path: planner + four mock workers + contract gate ──
    // The planner posts four approval-gated tasks. The CLI resolves the
    // runner and attaches as role 4.
    {
        int out = -1, in = -1;
        const pid_t plan = [&] {
            int po[2], pi[2];
            if (pipe(po) != 0 || pipe(pi) != 0)
                return static_cast<pid_t>(-1);
            const pid_t pid = fork();
            if (pid == 0) {
                ::close(po[0]);
                ::close(pi[1]);
                dup2(po[1], STDOUT_FILENO);
                dup2(po[1], STDERR_FILENO);
                dup2(pi[0], STDIN_FILENO);
                ::close(po[1]);
                ::close(pi[0]);
                setenv("AURA_SANDBOX", "off", 1);
                const std::string exe = aura_binary();
                execl(exe.c_str(), "aura", "agent", "--role", "planner", "--workspace", "8920",
                      "--mock", "--tasks", "4", "--contract", "--requirement", "port ZRANGEBYSCORE",
                      static_cast<char*>(nullptr));
                _exit(24);
            }
            ::close(po[1]);
            ::close(pi[0]);
            out = po[0];
            in = pi[1];
            return pid;
        }();
        check(plan > 0, "fork planner");
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "posted=4"), "planner posted four tasks");
        check(has(text, "agent-exit=0"), "planner exited cleanly");
        int status = 0;
        waitpid(plan, &status, 0);
    }
    {
        int out = -1, in = -1;
        const pid_t approve = run_script(d + "/approve.aura", &out, &in);
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "approve1=#t"), "task 1 approved");
        check(has(text, "approve2=#t"), "task 2 approved");
        check(has(text, "approve3=#t"), "task 3 approved");
        check(has(text, "approve4=#t"), "task 4 approved");
        if (!has(text, "approve1=#t"))
            std::printf("APPROVE-OUT:[%s]\n", text.c_str());
        int status = 0;
        waitpid(approve, &status, 0);
    }
    {
        constexpr int kWorkers = 4;
        pid_t pids[kWorkers] = {};
        int outs[kWorkers] = {};
        int ins[kWorkers] = {};
        for (int i = 0; i < kWorkers; ++i) {
            pids[i] = run_worker(8920, &outs[i], &ins[i]);
            check(pids[i] > 0, "fork mock worker");
            ::close(ins[i]);
        }
        std::string text;
        for (int i = 0; i < kWorkers; ++i) {
            text += read_all(outs[i]);
            int status = 0;
            waitpid(pids[i], &status, 0);
        }
        check(count_occurrences(text, "completed-task=") == 4,
              "four workers completed four tasks (mock LLM)");
        check(has(text, "idle=task-none-open"), "a drained worker reports no open task");
    }
    {
        int out = -1, in = -1;
        const pid_t verify = run_script(d + "/verify.aura", &out, &in);
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "s1=2"), "task 1 is done");
        check(has(text, "s2=2"), "task 2 is done");
        check(has(text, "s3=2"), "task 3 is done");
        check(has(text, "s4=2"), "task 4 is done");
        check(has(text, "findings=0"), "audit replay has no integrity findings");
        if (!has(text, "findings=0"))
            std::printf("VERIFY-OUT:[%s]\n", text.c_str());
        int status = 0;
        waitpid(verify, &status, 0);
    }

    // ── fault 1: kill the holder; the lease expires; another claims ──
    check(::write(host_in, "post\n", 5) == 5, "post task 5");
    check(
        has(read_until(host_out, "posted=", 10000) + read_until(host_out, "\n", 10000), "posted=5"),
        "task 5 posted");
    {
        int out = -1, in = -1;
        const pid_t hang = run_script(d + "/hang.aura", &out, &in);
        check(hang > 0, "fork hanging worker");
        const std::string hang_text =
            read_until(out, "hang-claimed=", 10000) + read_until(out, "\n", 10000);
        if (!has(hang_text, "hang-claimed=5"))
            std::printf("HANG-OUT:[%s]\n", hang_text.c_str());
        check(has(hang_text, "hang-claimed=5"), "hanging worker claimed task 5");
        ::kill(hang, SIGKILL);
        int status = 0;
        waitpid(hang, &status, 0);
        ::close(in);
        ::close(out);
        usleep(1200000); // lease 900 ms
    }
    {
        int out = -1, in = -1;
        const pid_t worker = run_worker(8920, &out, &in);
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "completed-task=5"), "another worker reclaimed and completed task 5");
        int status = 0;
        waitpid(worker, &status, 0);
    }

    // ── fault 2: restart the host; the open task survives ──
    check(::write(host_in, "post\n", 5) == 5, "post task 6");
    check(
        has(read_until(host_out, "posted=", 10000) + read_until(host_out, "\n", 10000), "posted=6"),
        "task 6 posted");
    check(::write(host_in, "unhost\n", 7) == 7, "unhost");
    check(has(read_until(host_out, "unhosted", 10000), "unhosted"), "host unhosted");
    check(::write(host_in, "host\n", 5) == 5, "rehost");
    check(has(read_until(host_out, "hosted", 10000), "hosted"), "host rehosted");
    {
        int out = -1, in = -1;
        const pid_t worker = run_worker(8920, &out, &in);
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "completed-task=6"), "restored task 6 completed after host restart");
        int status = 0;
        waitpid(worker, &status, 0);
    }

    // ── fault 3: the task budget fuse ──
    check(::write(host_in, "post\n", 5) == 5, "post task 7");
    check(
        has(read_until(host_out, "posted=", 10000) + read_until(host_out, "\n", 10000), "posted=7"),
        "task 7 posted");
    {
        int out = -1, in = -1;
        const pid_t quota = run_script(d + "/quota.aura", &out, &in);
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "quota-claim=7"), "budget worker claimed task 7");
        check(has(text, "quota-charge=error"), "over-limit charge is refused");
        check(has(text, "quota-complete=error"), "a tripped task cannot complete");
        int status = 0;
        waitpid(quota, &status, 0);
    }

    // ── fault 4: approval timeout denies the task ──
    {
        int out = -1, in = -1;
        const pid_t req = run_script(d + "/timeout-request.aura", &out, &in);
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "timeout-task=8"), "timeout task is 8");
        check(has(text, "timeout-request=ok"), "approval request accepted");
        int status = 0;
        waitpid(req, &status, 0);
        usleep(250000); // the 50 ms timeout must have elapsed
    }
    {
        int out = -1, in = -1;
        const pid_t claim = run_script(d + "/timeout-claim.aura", &out, &in);
        ::close(in);
        const std::string text = read_all(out);
        check(has(text, "timeout-claim=error:contract-approval-timeout"),
              "timed-out approval refuses the claim");
        int status = 0;
        waitpid(claim, &status, 0);
    }

    check(::write(host_in, "quit\n", 5) == 5, "quit host");
    const std::string tail = read_all(host_out);
    int status = 0;
    waitpid(host, &status, 0);
    check(has(tail, "host-quit"), "host quit cleanly");
    struct stat st{};
    check(::stat(sock.c_str(), &st) != 0, "socket unlinked");

    if (g_failed == 0)
        std::printf("test_bus_agents_e2e: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
