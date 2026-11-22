// Issue #5381: canary two-phase commit and automatic degradation revert.
// A KEEP with an observation window enters Canary; metrics below the
// floor twice auto-revert (workspace restored, canary-reverted audit);
// metrics at/above the floor for the full window seal to Kept; a second
// KEEP while a canary is active is refused canary-active.

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

pid_t run_script(const std::string& script, int* out_fd, int* in_fd, const char* window,
                 const char* floor, const char* key, bool degrade = false) {
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
        setenv("AURA_BUS_CANARY_WINDOW", window, 1);
        setenv("AURA_BUS_CANARY_FLOOR", floor, 1);
        setenv("AURA_BUS_CANARY_KEY", key, 1);
        // #5394: the degradation detector watches the same metric.
        if (degrade) {
            setenv("AURA_BUS_DEGRADE_KEYS", key, 1);
            setenv("AURA_BUS_DEGRADE_UP", key, 1);
            setenv("AURA_BUS_DEGRADE_MIN_SAMPLES", "4", 1);
            setenv("AURA_BUS_DEGRADE_LIMIT", "200000", 1);
        }
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

std::string read_until(int fd, const char* marker, int timeout_ms) {
    std::string s;
    char buf[512];
    int waited = 0;
    while (waited < timeout_ms) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 50);
        waited += 50;
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
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

std::string read_all(int fd) {
    std::string s;
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

// Host: owns choose-fn, hosts the bus with canary params from env,
// self-attaches as maintainer. Commands:
//   "keep"        — open trial, publish pure patch + evidence, grant,
//                   activate, KEEP (enters canary). Reports keep result.
//   "metric <v>"  — publish metric key 9 value v (drives the window).
//   "choose"      — report (choose-fn 5).
//   "audit"       — report the worldline audit kinds.
//   "keep2"       — second trial + KEEP while canary active (must refuse).
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (choose-fn x) (+ x 1))")
(eval-current)
(define h (bus:host 8940))
(define ms (bus:attach "" 8940 3 127 255))
(define msid (car (cdr ms)))
(bus:project-nodes msid)
(display "ready") (newline)
(define (do-keep)
  (define tr (bus:trial msid 3))
  (define wl (car (cdr tr)))
  (define src (bus:publish-source msid "(lambda (x) (+ x 10))"))
  (bus:publish-patch msid 5 bus:source-field (car (cdr src)) wl 0)
  (define d (car (cdr (bus:candidate-digest msid wl))))
  (bus:bind-evidence msid wl 0 d 7 42 1)
  (define g (bus:grant msid 24 255 0 0 wl))
  (bus:grant-activate msid (car (cdr g)))
  (define k (bus:keep msid wl 0 0))
  (display "keep=") (display (if (bus:ok? k) "ok" (bus:code k)))
  (display " wl=") (display wl) (display "|") (newline)
  (bus:detach msid)
  (set! ms (bus:attach "" 8940 3 127 255))
  (set! msid (car (cdr ms)))
  wl)
(define last-wl 0)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "keep")
       (set! last-wl (do-keep))
       (loop))
      ((string=? l "keep2")
       (define tr (bus:trial msid 3))
       (define wl (car (cdr tr)))
       (define src (bus:publish-source msid "(lambda (x) (+ x 100))"))
       (bus:publish-patch msid 5 bus:source-field (car (cdr src)) wl 0)
       (define d (car (cdr (bus:candidate-digest msid wl))))
       (bus:bind-evidence msid wl 1 d 7 42 1)
       (define g (bus:grant msid 24 255 0 0 wl))
       (bus:grant-activate msid (car (cdr g)))
       (define k (bus:keep msid wl 0 0))
       (display "keep2=") (display (if (bus:ok? k) "ok" (bus:code k))) (display "|") (newline)
       (bus:detach msid)
       (set! ms (bus:attach "" 8940 3 127 255))
       (set! msid (car (cdr ms)))
       (loop))
      ((and (>= (string-length l) 7) (string=? (substring l 0 7) "metric "))
       (define v (string->number (substring l 7 (string-length l))))
       (bus:publish msid 9 v)
       (display "metric-ok|") (newline)
       (loop))
      ((string=? l "choose")
       (display "choose=") (display (choose-fn 5)) (display "|") (newline)
       (loop))
      ((string=? l "audit")
       (define a (bus:audit msid last-wl))
       (display "audit=") (display a) (display "|") (newline)
       (loop))
      (else (loop)))))
(bus:detach msid)
(bus:unhost 8940)
(display "DONE") (newline)
)AURA";

void send_cmd(int fd, const std::string& cmd) {
    check(::write(fd, cmd.c_str(), cmd.size()) == static_cast<ssize_t>(cmd.size()), "send command");
}

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-canary-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8940);
    check(write_file(d + "/host.aura", kHostScript), "write host");

    // 1. Below-floor metrics auto-revert: choose-fn restores to 6.
    {
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/host.aura", &out_fd, &in_fd, "4", "50", "9");
        check(pid > 0, "fork revert host");
        check(wait_socket(sock), "revert socket appeared");
        check(has(read_until(out_fd, "ready", 10000), "ready"), "revert host ready");
        send_cmd(in_fd, "keep\n");
        const std::string k = read_until(out_fd, "|", 10000);
        check(has(k, "keep=ok"), "revert: KEEP enters canary");
        send_cmd(in_fd, "choose\n");
        check(has(read_until(out_fd, "|", 10000), "choose=15"), "revert: canary body live (15)");
        send_cmd(in_fd, "metric 10\n");
        (void)read_until(out_fd, "|", 10000);
        send_cmd(in_fd, "metric 10\n");
        (void)read_until(out_fd, "|", 10000);
        send_cmd(in_fd, "choose\n");
        check(has(read_until(out_fd, "|", 10000), "choose=6"), "revert: workspace restored (6)");
        send_cmd(in_fd, "audit\n");
        const std::string a = read_until(out_fd, "|", 10000);
        // CanaryRevert kind = 16
        check(has(a, "16"), "revert: canary-reverted audit present");
        ::close(in_fd);
        const std::string out = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(out, "DONE"), "revert host finished");
    }

    // 2. At/above-floor metrics seal to Kept: choose-fn stays 15.
    {
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/host.aura", &out_fd, &in_fd, "4", "50", "9");
        check(pid > 0, "fork seal host");
        check(wait_socket(sock), "seal socket appeared");
        check(has(read_until(out_fd, "ready", 10000), "ready"), "seal host ready");
        send_cmd(in_fd, "keep\n");
        check(has(read_until(out_fd, "|", 10000), "keep=ok"), "seal: KEEP enters canary");
        for (int i = 0; i < 4; ++i) {
            send_cmd(in_fd, "metric 100\n");
            (void)read_until(out_fd, "|", 10000);
        }
        send_cmd(in_fd, "choose\n");
        check(has(read_until(out_fd, "|", 10000), "choose=15"),
              "seal: window sealed, body kept (15)");
        send_cmd(in_fd, "audit\n");
        const std::string a = read_until(out_fd, "|", 10000);
        // CanarySeal kind = 15
        check(has(a, "15"), "seal: canary-seal audit present");
        ::close(in_fd);
        const std::string out = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(out, "DONE"), "seal host finished");
    }

    // 3. Second KEEP while canary active is refused canary-active.
    {
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/host.aura", &out_fd, &in_fd, "4", "50", "9");
        check(pid > 0, "fork busy host");
        check(wait_socket(sock), "busy socket appeared");
        check(has(read_until(out_fd, "ready", 10000), "ready"), "busy host ready");
        send_cmd(in_fd, "keep\n");
        check(has(read_until(out_fd, "|", 10000), "keep=ok"), "busy: first KEEP enters canary");
        send_cmd(in_fd, "keep2\n");
        const std::string k2 = read_until(out_fd, "|", 10000);
        check(has(k2, "keep2=canary-active"), "busy: second KEEP refused canary-active");
        ::close(in_fd);
        const std::string out = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(out, "DONE"), "busy host finished");
    }

    // 4. #5394: the degradation detector watches the same metric. A
    //    sustained rise rolls the observation back — the candidate
    //    degraded the metric it was supposed to improve.
    {
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(d + "/host.aura", &out_fd, &in_fd, "8", "0", "9", true);
        check(pid > 0, "fork the detector host");
        check(wait_socket(sock), "detector socket appeared");
        check(has(read_until(out_fd, "ready", 10000), "ready"), "detector host ready");
        send_cmd(in_fd, "keep\n");
        check(has(read_until(out_fd, "|", 10000), "keep=ok"), "detector: KEEP enters canary");
        // The baseline, then a sustained rise: the detector fires.
        send_cmd(in_fd, "metric 40\n");
        (void)read_until(out_fd, "|", 10000);
        for (int i = 0; i < 3; ++i) {
            send_cmd(in_fd, "metric 1000\n");
            (void)read_until(out_fd, "|", 10000);
        }
        send_cmd(in_fd, "choose\n");
        check(has(read_until(out_fd, "|", 10000), "choose=6"),
              "detector: the degraded candidate was rolled back");
        send_cmd(in_fd, "audit\n");
        const std::string a = read_until(out_fd, "|", 10000);
        // DegradeAlert kind = 27, CanaryRevert kind = 16
        check(has(a, "27"), "detector: the degradation alert is audited");
        check(has(a, "16"), "detector: the observation was reverted");
        ::close(in_fd);
        const std::string out = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(out, "DONE"), "detector host finished");
    }

    if (g_failed == 0)
        std::printf("test_bus_canary_rollback: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
