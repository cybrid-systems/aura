// Issue #5380: bus constraint executable minimal safe subset, aligned
// with the #5253 protection set and purity checks. A protected define
// cannot be patched; an impure candidate body (I/O / set! / bus:) is
// refused by pure-define-only; a legal pure patch passes; a session
// without constraint-write cannot install or rollback constraints.

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
        setenv("AURA_BUS_EVIDENCE", "1", 1);
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

// Host: protects "gate" via std/contract, installs pure-define-only,
// hosts the bus, self-attaches as maintainer. On "patch <node> <body>"
// it opens a trial, publishes a source-carrier patch + evidence, grants
// itself scoped keep, activates, and KEEPs. Reports keep result + the
// live gate / choose-fn values.
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(require "std/contract" all:)
(set-code "(define (gate x) (+ x 1)) (define (choose-fn x) (+ x 1))")
(eval-current)
(contract:activate! (contract:protect (contract:set) "gate"))
(define h (bus:host 8930))
(define ms (bus:attach "" 8930 3 127 255))
(define msid (car (cdr ms)))
(bus:install-constraint msid "pure-define-only" "(lambda (ctx) #t)")
(bus:project-nodes msid)
(display "ready") (newline)
(define (do-patch node body)
  ; One long-lived session opens the trial, publishes patch + evidence,
  ; derives a scoped keep grant, activates it, and KEEPs. The session
  ; owns the trial so no cross-session delegation is needed; the grant
  ; exercise is covered by test_bus_multi_peer. After the KEEP the
  ; session caps are narrowed, so re-attach to restore full caps.
  (define tr (bus:trial msid 3))
  (define wl (car (cdr tr)))
  (define src (bus:publish-source msid body))
  (bus:publish-patch msid node bus:source-field (car (cdr src)) wl 0)
  (define d (car (cdr (bus:candidate-digest msid wl))))
  (bus:bind-evidence msid wl 0 d 7 42 1)
  (define g (bus:grant msid 24 255 0 0 wl))
  (bus:grant-activate msid (car (cdr g)))
  (define k (bus:keep msid wl 0 0))
  (display "keep=") (display (if (bus:ok? k) "ok" (bus:code k)))
  (display " reason=") (display (bus:message k)) (display " patch-done|") (newline)
  (bus:detach msid)
  (set! ms (bus:attach "" 8930 3 127 255))
  (set! msid (car (cdr ms))))
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "gate")
       (display "gate=") (display (gate 5)) (display "|") (newline)
       (loop))
      ((string=? l "choose")
       (display "choose=") (display (choose-fn 5)) (display "|") (newline)
       (loop))
      ((and (>= (string-length l) 6) (string=? (substring l 0 6) "patch "))
       ; "patch <node> <body>"
       (define sp (let find ((i 6))
                    (cond ((>= i (string-length l)) #f)
                          ((char=? (string-ref l i) #\space) i)
                          (else (find (+ i 1))))))
       (define node (string->number (substring l 6 sp)))
       (define body (substring l (+ sp 1) (string-length l)))
       (do-patch node body)
       (loop))
      (else (loop)))))
(bus:detach msid)
(bus:unhost 8930)
(display "DONE") (newline)
)AURA";

// Low-privilege peer: role harness, caps read|publish (3), no
// constraint-write. Attempts install / rollback; both must be refused.
const char* kLowPeerScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8930 2 3 255))
(define sid (car (cdr s)))
(define ic (bus:install-constraint sid "pure-define-only" "(lambda (ctx) #t)"))
(display "install=") (display (bus:code ic)) (display "|") (newline)
(define rc (bus:rollback-constraints sid))
(display "rollback=") (display (bus:code rc)) (display "|") (newline)
(bus:detach sid)
(display "LOW-DONE") (newline)
)AURA";

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-con-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8930);

    check(write_file(d + "/host.aura", kHostScript), "write host");
    check(write_file(d + "/low.aura", kLowPeerScript), "write low peer");
    int host_out = -1, host_in = -1;
    const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
    check(host > 0, "fork host");
    check(wait_socket(sock), "host socket appeared");
    const std::string rdy = read_until(host_out, "ready", 10000);
    check(has(rdy, "ready"), "host ready");

    // 1. Patch the protected "gate" (node 5): refused, gate unchanged.
    {
        const std::string cmd = "patch 5 (lambda (x) (+ x 999))\n";
        check(::write(host_in, cmd.c_str(), cmd.size()) == static_cast<ssize_t>(cmd.size()),
              "send protected patch");
    }
    const std::string k1 = read_until(host_out, "patch-done|", 15000);
    check(has(k1, "keep=validation-failed") || has(k1, "keep=constraint-rejected"),
          "protected define patch refused");
    check(has(k1, "protects") || has(k1, "constraint"), "refusal names the protection set");
    check(::write(host_in, "gate\n", 5) == 5, "send gate query");
    check(has(read_until(host_out, "|", 10000), "gate=6"), "gate unchanged after protected patch");

    // 2. Impure choose-fn body (display): pure-define-only refuses.
    {
        const std::string cmd = "patch 11 (lambda (x) (display x) (+ x 10))\n";
        check(::write(host_in, cmd.c_str(), cmd.size()) == static_cast<ssize_t>(cmd.size()),
              "send impure patch");
    }
    const std::string k2 = read_until(host_out, "patch-done|", 15000);
    check(has(k2, "keep=constraint-rejected"), "impure body KEEP refused");
    check(has(k2, "pure-define-only"), "refusal names pure-define-only");
    check(::write(host_in, "choose\n", 7) == 7, "send choose query");
    check(has(read_until(host_out, "|", 10000), "choose=6"),
          "choose-fn unchanged after impure patch");

    // 3. Legal pure choose-fn patch: passes and applies.
    {
        const std::string cmd = "patch 11 (lambda (x) (+ x 10))\n";
        check(::write(host_in, cmd.c_str(), cmd.size()) == static_cast<ssize_t>(cmd.size()),
              "send pure patch");
    }
    const std::string k3 = read_until(host_out, "patch-done|", 15000);
    check(has(k3, "keep=ok"), "legal pure patch KEEP succeeds");
    check(::write(host_in, "choose\n", 7) == 7, "send choose query 2");
    check(has(read_until(host_out, "|", 10000), "choose=15"), "choose-fn reflects the pure patch");

    // 4. Low-privilege peer cannot install or rollback constraints.
    int low_out = -1, low_in = -1;
    const pid_t low = run_script(d + "/low.aura", &low_out, &low_in);
    check(low > 0, "fork low peer");
    const std::string lout = read_until(low_out, "LOW-DONE", 10000);
    check(has(lout, "install=permission"), "low peer install refused with permission");
    check(has(lout, "rollback=permission"), "low peer rollback refused with permission");
    int lstatus = 0;
    waitpid(low, &lstatus, 0);
    ::close(low_in);
    ::close(low_out);

    ::close(host_in);
    const std::string hout = read_all(host_out);
    int hstatus = 0;
    waitpid(host, &hstatus, 0);
    check(has(hout, "DONE"), "host finished");

    if (g_failed == 0)
        std::printf("test_bus_constraints_subset: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
