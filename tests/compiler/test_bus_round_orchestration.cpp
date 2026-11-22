// Issue #5393: the admission round state machine and its fail-closed
// semantics.
//
// The host drives one round at a time: idle → signal → trial → judge
// → canary → sealed. Every transition is audited, every failure fails
// closed: a judge that dies, a proposer that dies or a round that
// runs out of budget rolls the pending candidate back, and a restarted
// host resumes from its checkpoint instead of applying a sealed
// candidate twice.

#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/image_schema.hh"
#include "bus/round.hh"
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

// ── The machine ───────────────────────────────────────────────
void test_machine() {
    RoundBudget budget{};
    budget.wall_ms = 1000;
    budget.cooldown_ms = 500;
    RoundMachine machine;
    machine.configure(budget);

    std::string why;
    // A round opens, takes a candidate, is adjudicated and seals.
    check(machine.open(1000, false, &why), "a round opens");
    check(machine.phase() == RoundPhase::Signal, "the round is in the signal phase");
    check(machine.id() == 1, "the first round is id 1");
    check(machine.note_trial(7, 42, &why), "the candidate joins the round");
    check(machine.phase() == RoundPhase::Trial, "the round is in the trial phase");
    check(machine.worldline() == 7, "the round holds the candidate");
    check(machine.note_judge(43, &why), "the judge is recorded");
    check(machine.note_keep(&why), "the candidate goes to the judge");
    check(machine.phase() == RoundPhase::Judge, "the round is in the judge phase");
    check(machine.note_seal(false, 1200, &why), "the round seals");
    check(machine.phase() == RoundPhase::Sealed, "the round is sealed");
    check(machine.sealed_count() == 1, "one round sealed");

    // The cooldown keeps the next round out.
    check(!machine.open(1300, false, &why), "the cooldown refuses a round");
    check(why == "round-cooldown", "the refusal names the cooldown");
    check(machine.open(1800, false, &why), "the round opens after the cooldown");

    // One round at a time.
    check(!machine.open(1900, false, &why), "a second round is refused");
    check(why == "round-active", "the refusal names the active round");

    // An unsealed observation owns the workspace.
    machine.roll("test", 2000, &why);
    check(machine.phase() == RoundPhase::Idle, "a rolled round is idle again");
    check(!machine.open(2100, true, &why), "an unsealed observation refuses a round");
    check(why == "canary-active", "the refusal names the observation");

    // The wall budget closes a round that runs out of time.
    check(machine.open(2600, false, &why), "a round opens");
    check(!machine.expired(3500), "the round is inside its budget");
    check(machine.expired(3700), "the round ran out of wall clock");

    // The round cap.
    RoundBudget capped{};
    capped.max_rounds = 2;
    RoundMachine limited;
    limited.configure(capped);
    check(limited.open(0, false, &why), "the first round opens");
    limited.roll("test", 1, &why);
    check(limited.open(2, false, &why), "the second round opens");
    limited.roll("test", 3, &why);
    check(!limited.open(4, false, &why), "the round cap refuses a round");
    check(why == "round-budget-exhausted", "the refusal names the budget");

    // The checkpoint remembers the sealed candidates.
    const std::string path = "/tmp/aura-round-checkpoint-" + std::to_string(::getpid()) + ".txt";
    ::unlink(path.c_str());
    RoundCheckpoint checkpoint;
    check(checkpoint.open(path, &why), "the checkpoint opens");
    check(!checkpoint.sealed(0xabc), "nothing is sealed yet");
    check(checkpoint.record_seal(0xabc, 3, 100, &why), "the seal is recorded");
    check(checkpoint.record_round(7, RoundPhase::Sealed, 3, 101, &why), "the round is recorded");
    RoundCheckpoint reloaded;
    check(reloaded.open(path, &why), "the checkpoint reloads");
    check(reloaded.sealed(0xabc), "the sealed candidate is remembered");
    check(reloaded.round_id() == 7, "the round id is remembered");
    check(reloaded.round_phase() == RoundPhase::Sealed, "the round phase is remembered");
    ::unlink(path.c_str());
}

// ── The std/bus surface, driven through real aura processes ───
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

bool write_file(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    std::fputs(text.c_str(), f);
    std::fclose(f);
    return true;
}

pid_t run_script(const std::string& script, int* out_fd, int* in_fd, const char* wall_ms = nullptr,
                 const char* canary_window = nullptr) {
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
        setenv("AURA_BUS_EVIDENCE", "1", 1);
        if (wall_ms != nullptr)
            setenv("AURA_BUS_ROUND_WALL_MS", wall_ms, 1);
        if (canary_window != nullptr)
            setenv("AURA_BUS_CANARY_WINDOW", canary_window, 1);
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

// The bytes past the marker: one pipe read can carry more than one
// line, and the next command's reply must not be lost with it.
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
    std::string out = g_pending[fd];
    g_pending.erase(fd);
    char buf[512];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(fd);
    return out;
}

bool has(const std::string& out, const char* line) {
    return out.find(line) != std::string::npos;
}

bool wait_socket(const std::string& sock) {
    for (int i = 0; i < 400; ++i) {
        struct stat st{};
        if (::stat(sock.c_str(), &st) == 0)
            return true;
        usleep(50000);
    }
    return false;
}

void send_cmd(int fd, const std::string& cmd) {
    const ssize_t n = ::write(fd, cmd.c_str(), cmd.size());
    check(n == static_cast<ssize_t>(cmd.size()), "send command");
    // The peer reads one line at a time: give it the line before the
    // next command goes out.
    usleep(120000);
}

// The host: it owns the bus and answers the round commands.
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host 8960))
(display "ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      (else (loop)))))
(bus:unhost 8960)
(display "HOST-DONE") (newline)
)AURA";

// The driver: a peer that opens the rounds, delegates the
// adjudication and reads the round state. It is a separate process on
// purpose: the host's own session would share its socket with the
// coordinator.
const char* kDriverScript = R"AURA(
(require "std/bus" all:)
(require "std/string" all:)
(define s (bus:attach "" 8960 0 127 255))
(define sid (car (cdr s)))
; The workspace nodes: a candidate patch names one of them.
(bus:project-nodes sid)
(display "driver-ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "round")
       (define r (bus:round-open sid))
       (display "round=")
       (display (if (bus:ok? r) "ok" (bus:message r)))
       (display "|") (newline) (loop))
      ((string=? l "state")
       (define st (bus:round-state sid))
       (display "phase=") (display (list-ref st 1))
       (display " id=") (display (list-ref st 2))
       (display " opened=") (display (list-ref st 4))
       (display " sealed=") (display (list-ref st 5))
       (display " reason=") (display (list-ref st 6))
       (display "|") (newline) (loop))
      ((string=? l "budget")
       (define b (bus:round-budget sid))
       (display "wall=") (display (list-ref b 1))
       (display " max=") (display (list-ref b 3))
       (display "|") (newline) (loop))
      ((string-prefix? l "grant ")
       (define parts (string-split l " "))
       (define wl (string->number (list-ref parts 1)))
       (define g (bus:grant sid 24 255 0 0 wl))
       (display "token=") (display (car (cdr g))) (display "|") (newline) (loop))
      ((string=? l "audit")
       (define a (bus:audit sid 0))
       (display "audit=") (display a) (display "|") (newline) (loop))
      ((string-prefix? l "metric ")
       (define parts (string-split l " "))
       (define v (string->number (list-ref parts 1)))
       (bus:publish sid 9 v)
       (display "metric-ok|") (newline) (loop))
      (else (loop)))))
(bus:detach sid)
(display "DRIVER-DONE") (newline)
)AURA";

// The proposer: it opens the round's candidate. The command carries the
// candidate's number, so every case proposes a distinct candidate (a
// sealed candidate is never applied twice).
const char* kProposerScript = R"AURA(
(require "std/bus" all:)
(require "std/string" all:)
(define s (bus:attach "" 8960 1 7 255))
(define sid (car (cdr s)))
(display "prop-ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string-prefix? l "trial ")
       (define parts (string-split l " "))
       (define body (string-append "(lambda (x) (+ x " (list-ref parts 1) "))"))
       (define tr (bus:trial sid 3))
       (define wl (car (cdr tr)))
       (define src (bus:publish-source sid body))
       (define pp (bus:publish-patch sid 5 bus:source-field (car (cdr src)) wl 0))
       (define d (car (cdr (bus:candidate-digest sid wl))))
       (bus:bind-evidence sid wl 0 d 7 42 1)
       (display "wl=") (display wl) (display "|") (newline)
       (loop))
      (else (loop)))))
(bus:detach sid)
(display "PROP-DONE") (newline)
)AURA";

// The judge: it activates the delegated grant and keeps the candidate.
const char* kJudgeScript = R"AURA(
(require "std/bus" all:)
(require "std/string" all:)
(define s (bus:attach "" 8960 2 3 255))
(define sid (car (cdr s)))
(display "judge-ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string-prefix? l "activate ")
       (define parts (string-split l " "))
       (define token (string->number (list-ref parts 1)))
       (define g (bus:grant-activate sid token))
       (display "activated=")
       (display (if (bus:ok? g) "ok" (bus:code g)))
       (display "|") (newline) (loop))
      ((string-prefix? l "keep ")
       (define parts (string-split l " "))
       (define token (string->number (list-ref parts 1)))
       (define wl (string->number (list-ref parts 2)))
       (bus:grant-activate sid token)
       (define k (bus:keep sid wl 0 0))
       (display "keep=")
       (display (if (bus:ok? k) "ok" (bus:message k)))
       (display "|") (newline) (loop))
      (else (loop)))))
(bus:detach sid)
(display "JUDGE-DONE") (newline)
)AURA";

} // namespace

int main() {
    test_machine();

    char tmpl[] = "/tmp/aura-bus-round-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8960);
    check(write_file(d + "/host.aura", kHostScript), "write the host script");
    check(write_file(d + "/driver.aura", kDriverScript), "write the driver script");
    check(write_file(d + "/proposer.aura", kProposerScript), "write the proposer script");
    check(write_file(d + "/judge.aura", kJudgeScript), "write the judge script");

    // The worldline id a proposer opened, and the grant token the
    // driver delegated it.
    auto worldline_of = [](const std::string& line) {
        const std::size_t at = line.find("wl=");
        if (at == std::string::npos)
            return std::string();
        return line.substr(at + 3, line.find('|', at) - at - 3);
    };
    auto token_of = [](const std::string& line) {
        const std::size_t at = line.find("token=");
        if (at == std::string::npos)
            return std::string();
        return line.substr(at + 6, line.find('|', at) - at - 6);
    };

    // ── 1. A normal round walks idle → … → sealed ──────────────
    {
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
        check(host > 0, "fork the host");
        check(wait_socket(sock), "the host socket appeared");
        check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
        int drv_out = -1, drv_in = -1;
        const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
        check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
              "the driver is ready");
        int prop_out = -1, prop_in = -1;
        const pid_t proposer = run_script(d + "/proposer.aura", &prop_out, &prop_in);
        check(has(read_until(prop_out, "prop-ready", 20000), "prop-ready"),
              "the proposer is ready");
        int judge_out = -1, judge_in = -1;
        const pid_t judge = run_script(d + "/judge.aura", &judge_out, &judge_in);
        check(has(read_until(judge_out, "judge-ready", 20000), "judge-ready"),
              "the judge is ready");

        // The driver opens the round, the proposer opens the
        // candidate, the driver delegates it and the judge keeps it.
        send_cmd(drv_in, "round\n");
        check(has(read_until(drv_out, "|", 10000), "round=ok"), "the driver opens a round");
        send_cmd(prop_in, "trial 1\n");
        const std::string worldline = worldline_of(read_until(prop_out, "|", 10000));
        check(!worldline.empty(), "the proposer opened a candidate");
        send_cmd(drv_in, "grant " + worldline + "\n");
        const std::string token = token_of(read_until(drv_out, "|", 10000));
        check(!token.empty(), "the driver delegated the candidate");
        send_cmd(judge_in, "keep " + token + " " + worldline + "\n");
        check(has(read_until(judge_out, "|", 10000), "keep=ok"), "the judge keeps the candidate");
        // The round is sealed.
        send_cmd(drv_in, "state\n");
        const std::string state = read_until(drv_out, "|", 10000);
        check(has(state, "phase=5"), "the round is sealed");
        check(has(state, "sealed=1"), "one round sealed");
        send_cmd(drv_in, "audit\n");
        const std::string audit = read_until(drv_out, "|", 10000);
        // RoundOpen = 20, RoundTrial = 21, RoundJudge = 22, RoundSeal = 23.
        check(has(audit, "20"), "the audit carries the round opening");
        check(has(audit, "21"), "the audit carries the round candidate");
        check(has(audit, "23"), "the audit carries the round seal");

        ::close(prop_in);
        ::close(judge_in);
        ::close(drv_in);
        ::close(host_in);
        read_all(host_out);
        read_all(drv_out);
        read_all(prop_out);
        read_all(judge_out);
        int status = 0;
        waitpid(host, &status, 0);
        kill(proposer, SIGKILL);
        kill(judge, SIGKILL);
        waitpid(proposer, &status, 0);
        waitpid(judge, &status, 0);
    }

    // ── 2. A judge that dies rolls the candidate back ──────────
    {
        ::unlink(sock.c_str());
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
        check(wait_socket(sock), "the host socket appeared");
        check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
        int drv_out = -1, drv_in = -1;
        const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
        check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
              "the driver is ready");
        int prop_out = -1, prop_in = -1;
        const pid_t proposer = run_script(d + "/proposer.aura", &prop_out, &prop_in);
        check(has(read_until(prop_out, "prop-ready", 20000), "prop-ready"),
              "the proposer is ready");
        int judge_out = -1, judge_in = -1;
        const pid_t judge = run_script(d + "/judge.aura", &judge_out, &judge_in);
        check(has(read_until(judge_out, "judge-ready", 20000), "judge-ready"),
              "the judge is ready");

        send_cmd(drv_in, "round\n");
        check(has(read_until(drv_out, "|", 10000), "round=ok"), "the driver opens a round");
        send_cmd(prop_in, "trial 2\n");
        const std::string worldline = worldline_of(read_until(prop_out, "|", 10000));
        send_cmd(drv_in, "grant " + worldline + "\n");
        const std::string token = token_of(read_until(drv_out, "|", 10000));
        // The judge takes the delegation and then dies.
        send_cmd(judge_in, "activate " + token + "\n");
        check(has(read_until(judge_out, "|", 10000), "activated=ok"), "the judge took the grant");
        kill(judge, SIGKILL);
        int status = 0;
        waitpid(judge, &status, 0);
        // The host survives and the round fails closed.
        send_cmd(drv_in, "state\n");
        const std::string state = read_until(drv_out, "|", 10000);
        check(has(state, "phase=0"), "the round is idle again");
        check(has(state, "judge gone"), "the round rolled back for the judge");
        send_cmd(drv_in, "audit\n");
        const std::string audit = read_until(drv_out, "|", 10000);
        check(has(audit, "24"), "the audit carries the round rollback");
        // The next round starts normally.
        send_cmd(drv_in, "round\n");
        check(has(read_until(drv_out, "|", 10000), "round=ok"), "the next round opens");
        // The proposer is still connected.
        send_cmd(prop_in, "trial 3\n");
        check(!worldline_of(read_until(prop_out, "|", 10000)).empty(),
              "the proposer is still connected");

        ::close(prop_in);
        ::close(drv_in);
        ::close(host_in);
        read_all(host_out);
        read_all(drv_out);
        read_all(prop_out);
        int status2 = 0;
        waitpid(host, &status2, 0);
        kill(proposer, SIGKILL);
        waitpid(proposer, &status2, 0);
    }

    // ── 3. A proposer that dies returns the round to idle ───────
    {
        ::unlink(sock.c_str());
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
        check(wait_socket(sock), "the host socket appeared");
        check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
        int drv_out = -1, drv_in = -1;
        const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
        check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
              "the driver is ready");
        int prop_out = -1, prop_in = -1;
        const pid_t proposer = run_script(d + "/proposer.aura", &prop_out, &prop_in);
        check(has(read_until(prop_out, "prop-ready", 20000), "prop-ready"),
              "the proposer is ready");
        send_cmd(drv_in, "round\n");
        check(has(read_until(drv_out, "|", 10000), "round=ok"), "the driver opens a round");
        send_cmd(prop_in, "trial 4\n");
        check(!worldline_of(read_until(prop_out, "|", 10000)).empty(),
              "the proposer opened a candidate");
        kill(proposer, SIGKILL);
        int status = 0;
        waitpid(proposer, &status, 0);
        send_cmd(drv_in, "state\n");
        const std::string state = read_until(drv_out, "|", 10000);
        check(has(state, "phase=0"), "the round is idle again");
        check(has(state, "proposer gone"), "the round rolled back for the proposer");
        send_cmd(drv_in, "audit\n");
        const std::string audit = read_until(drv_out, "|", 10000);
        check(has(audit, "24"), "the audit carries the round rollback");

        ::close(drv_in);
        ::close(host_in);
        read_all(host_out);
        read_all(drv_out);
        int status2 = 0;
        waitpid(host, &status2, 0);
    }

    // ── 4. The wall budget closes a round that runs out of time ─
    {
        ::unlink(sock.c_str());
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in, "300");
        check(wait_socket(sock), "the host socket appeared");
        check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
        int drv_out = -1, drv_in = -1;
        const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
        check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
              "the driver is ready");
        int prop_out = -1, prop_in = -1;
        const pid_t proposer = run_script(d + "/proposer.aura", &prop_out, &prop_in);
        check(has(read_until(prop_out, "prop-ready", 20000), "prop-ready"),
              "the proposer is ready");
        send_cmd(drv_in, "budget\n");
        check(has(read_until(drv_out, "|", 10000), "wall=300"), "the budget is the wall clock");
        send_cmd(drv_in, "round\n");
        check(has(read_until(drv_out, "|", 10000), "round=ok"), "the driver opens a round");
        send_cmd(prop_in, "trial 5\n");
        check(!worldline_of(read_until(prop_out, "|", 10000)).empty(),
              "the proposer opened a candidate");
        // Nobody keeps: the budget closes the round.
        usleep(900000);
        send_cmd(drv_in, "state\n");
        const std::string state = read_until(drv_out, "|", 10000);
        check(has(state, "phase=0"), "the round is closed");
        check(has(state, "budget"), "the round rolled back for the budget");
        send_cmd(drv_in, "audit\n");
        const std::string audit = read_until(drv_out, "|", 10000);
        check(has(audit, "25"), "the audit carries the budget event");

        ::close(prop_in);
        ::close(drv_in);
        ::close(host_in);
        read_all(host_out);
        read_all(drv_out);
        int status = 0;
        waitpid(host, &status, 0);
        kill(proposer, SIGKILL);
        waitpid(proposer, &status, 0);
    }

    // ── 5. An unsealed observation refuses a new round ─────────
    {
        ::unlink(sock.c_str());
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in, nullptr, "8");
        check(wait_socket(sock), "the host socket appeared");
        check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
        int drv_out = -1, drv_in = -1;
        const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
        check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
              "the driver is ready");
        int prop_out = -1, prop_in = -1;
        const pid_t proposer = run_script(d + "/proposer.aura", &prop_out, &prop_in);
        check(has(read_until(prop_out, "prop-ready", 20000), "prop-ready"),
              "the proposer is ready");
        int judge_out = -1, judge_in = -1;
        const pid_t judge = run_script(d + "/judge.aura", &judge_out, &judge_in);
        check(has(read_until(judge_out, "judge-ready", 20000), "judge-ready"),
              "the judge is ready");
        send_cmd(drv_in, "round\n");
        check(has(read_until(drv_out, "|", 10000), "round=ok"), "the driver opens a round");
        send_cmd(prop_in, "trial 6\n");
        const std::string worldline = worldline_of(read_until(prop_out, "|", 10000));
        send_cmd(drv_in, "grant " + worldline + "\n");
        const std::string token = token_of(read_until(drv_out, "|", 10000));
        send_cmd(judge_in, "keep " + token + " " + worldline + "\n");
        check(has(read_until(judge_out, "|", 10000), "keep=ok"), "the judge keeps the candidate");
        // The candidate is under observation: the round holds it, so a
        // new round is refused.
        send_cmd(drv_in, "state\n");
        const std::string state = read_until(drv_out, "|", 10000);
        check(has(state, "phase=4"), "the round is under observation");
        send_cmd(drv_in, "round\n");
        const std::string refused = read_until(drv_out, "|", 10000);
        check(!has(refused, "round=ok"), "the unsealed observation refuses a new round");

        ::close(prop_in);
        ::close(judge_in);
        ::close(drv_in);
        ::close(host_in);
        read_all(host_out);
        read_all(drv_out);
        read_all(judge_out);
        int status = 0;
        waitpid(host, &status, 0);
        kill(proposer, SIGKILL);
        kill(judge, SIGKILL);
        waitpid(proposer, &status, 0);
        waitpid(judge, &status, 0);
    }

    // ── 6. A restarted host resumes and does not apply twice ───
    {
        ::unlink(sock.c_str());
        {
            int host_out = -1, host_in = -1;
            const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
            check(wait_socket(sock), "the host socket appeared");
            check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
            int drv_out = -1, drv_in = -1;
            const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
            check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
                  "the driver is ready");
            int prop_out = -1, prop_in = -1;
            const pid_t proposer = run_script(d + "/proposer.aura", &prop_out, &prop_in);
            check(has(read_until(prop_out, "prop-ready", 20000), "prop-ready"),
                  "the proposer is ready");
            int judge_out = -1, judge_in = -1;
            const pid_t judge = run_script(d + "/judge.aura", &judge_out, &judge_in);
            check(has(read_until(judge_out, "judge-ready", 20000), "judge-ready"),
                  "the judge is ready");
            send_cmd(drv_in, "round\n");
            check(has(read_until(drv_out, "|", 10000), "round=ok"), "the driver opens a round");
            send_cmd(prop_in, "trial 7\n");
            const std::string worldline = worldline_of(read_until(prop_out, "|", 10000));
            send_cmd(drv_in, "grant " + worldline + "\n");
            const std::string token = token_of(read_until(drv_out, "|", 10000));
            send_cmd(judge_in, "keep " + token + " " + worldline + "\n");
            check(has(read_until(judge_out, "|", 10000), "keep=ok"),
                  "the judge keeps the candidate");
            ::close(prop_in);
            ::close(judge_in);
            ::close(drv_in);
            ::close(host_in);
            read_all(host_out);
            read_all(drv_out);
            read_all(judge_out);
            int status = 0;
            waitpid(host, &status, 0);
            kill(proposer, SIGKILL);
            kill(judge, SIGKILL);
            waitpid(proposer, &status, 0);
            waitpid(judge, &status, 0);
        }
        // The host comes back: the checkpoint says the round ran and
        // the candidate is sealed.
        ::unlink(sock.c_str());
        {
            int host_out = -1, host_in = -1;
            const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
            check(wait_socket(sock), "the host socket appeared");
            check(has(read_until(host_out, "ready", 20000), "ready"), "the host is back");
            int drv_out = -1, drv_in = -1;
            const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
            check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
                  "the driver is ready");
            send_cmd(drv_in, "state\n");
            const std::string state = read_until(drv_out, "|", 10000);
            check(has(state, "opened=0"), "the restarted host has run no round yet");
            check(!has(state, "id=0"), "the host resumes after its checkpoint");
            // The same candidate is not applied twice.
            int prop_out = -1, prop_in = -1;
            const pid_t proposer = run_script(d + "/proposer.aura", &prop_out, &prop_in);
            check(has(read_until(prop_out, "prop-ready", 20000), "prop-ready"),
                  "the proposer is ready");
            int judge_out = -1, judge_in = -1;
            const pid_t judge = run_script(d + "/judge.aura", &judge_out, &judge_in);
            check(has(read_until(judge_out, "judge-ready", 20000), "judge-ready"),
                  "the judge is ready");
            send_cmd(drv_in, "round\n");
            check(has(read_until(drv_out, "|", 10000), "round=ok"), "the driver opens a round");
            send_cmd(prop_in, "trial 7\n");
            const std::string worldline = worldline_of(read_until(prop_out, "|", 10000));
            send_cmd(drv_in, "grant " + worldline + "\n");
            const std::string token = token_of(read_until(drv_out, "|", 10000));
            send_cmd(judge_in, "keep " + token + " " + worldline + "\n");
            const std::string kept = read_until(judge_out, "|", 10000);
            check(has(kept, "already-sealed"), "the sealed candidate is not applied twice");

            ::close(prop_in);
            ::close(judge_in);
            ::close(drv_in);
            ::close(host_in);
            read_all(host_out);
            read_all(drv_out);
            read_all(judge_out);
            int status = 0;
            waitpid(host, &status, 0);
            kill(proposer, SIGKILL);
            kill(judge, SIGKILL);
            waitpid(proposer, &status, 0);
            waitpid(judge, &status, 0);
        }
    }

    if (g_failed == 0)
        std::printf("test_bus_round_orchestration: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
