// Issue #5397: the target extension — one worldline rewrites several
// pure policy functions (choose-fn + admit-fn + ttl-fn) and the apply is
// all-or-nothing.
//
// The peer learns the host's define names from the host itself
// (bus:defines): the node ids live in the host's workspace AST, so
// guessing them from another process is not sound. The peer then
// publishes one source-carrier patch per function into ONE worldline.
// The host's KEEP applies them together or not at all: a broken body
// anywhere in the set leaves every function byte-identical, and a
// target outside the host's mutable-target allowlist is refused the
// same way.

#include <cstdio>
#include <map>
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

pid_t run_script(const std::string& script, int* out_fd, int* in_fd, const char* mutable_targets) {
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
        if (mutable_targets != nullptr)
            setenv("AURA_BUS_MUTABLE_TARGETS", mutable_targets, 1);
        else
            ::unsetenv("AURA_BUS_MUTABLE_TARGETS");
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
    for (int i = 0; i < 200; ++i) {
        struct stat st{};
        if (::stat(sock.c_str(), &st) == 0)
            return true;
        usleep(50000);
    }
    return false;
}

// The host owns three pure policy functions. It hosts the bus, self-
// attaches as maintainer, installs the purity constraint, and on "keep
// <wl>" it derives a scoped keep grant, activates it and KEEPs the
// worldline the proposer opened. On "values" it reports the three
// function bodies.
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (choose-fn x) (+ x 1)) (define (admit-fn y) (+ y 2)) (define (ttl-fn z) (+ z 3))")
(eval-current)
(define h (bus:host 8915))
(display (if (bus:ok? h) "host-ok" "host-FAIL")) (newline)
(define ms (bus:attach "" 8915 3 127 255))
(define msid (car (cdr ms)))
(bus:install-constraint msid "pure-define-only" "(lambda (ctx) #t)")
; The nodes the patch may name live in the projected node section.
(bus:project-nodes msid)
(display "ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "values")
       (display "v=") (display (choose-fn 1)) (display ",") (display (admit-fn 1))
       (display ",") (display (ttl-fn 1)) (display "|") (newline)
       (loop))
      ((and (>= (string-length l) 5) (string=? (substring l 0 5) "keep "))
       (define wl (string->number (substring l 5 (string-length l))))
       (define g (bus:grant msid 24 255 0 0 wl))
       (define tok (car (cdr g)))
       (bus:grant-activate msid tok)
       (define k (bus:keep msid wl 0 0))
       (display "keep=") (display (if (bus:ok? k) "ok" (bus:code k))) (display "|") (newline)
       (loop))
      (else (loop)))))
(bus:detach msid)
(bus:unhost 8915)
(display "HOST-DONE") (newline)
)AURA";

// The proposer asks the host for its define names, publishes one
// source-carrier patch per function into ONE worldline, and binds the
// evidence. The bodies are substituted by the test.
const char* kProposerTemplate = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 8915 1 7 255))
(define sid (car (cdr s)))
; The host's define names and their node ids.
(define dn (bus:defines sid))
(define defs (list-ref dn 3))
(define (node-of nm)
  (let loop ((rows defs))
    (if (null? rows) -1
        (if (string=? (car (car rows)) nm)
            (car (cdr (car rows)))
            (loop (cdr rows))))))
(define tr (bus:trial sid 3))
(define wl (car (cdr tr)))
(define (patch nm body)
  (define src (bus:publish-source sid body))
  (bus:publish-patch sid (node-of nm) bus:source-field (car (cdr src)) wl 0))
(patch "choose-fn" "BODY1")
(patch "admit-fn" "BODY2")
(patch "ttl-fn" "BODY3")
(define d (car (cdr (bus:candidate-digest sid wl))))
(bus:bind-evidence sid wl 0 d 7 42 1)
(display "wl=") (display wl) (newline)
(display "prop-ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (if (eof-object? l) #f (loop))))
(bus:detach sid)
(display "PROP-DONE") (newline)
)AURA";

std::string proposer_with_bodies(const char* b1, const char* b2, const char* b3) {
    std::string s = kProposerTemplate;
    const auto replace = [&s](const char* marker, const char* body) {
        const std::size_t at = s.find(marker);
        if (at == std::string::npos)
            return;
        std::string escaped;
        for (const char* p = body; *p; ++p) {
            if (*p == '"')
                escaped += "\\\"";
            else
                escaped += *p;
        }
        s.replace(at, std::strlen(marker), escaped);
    };
    replace("BODY1", b1);
    replace("BODY2", b2);
    replace("BODY3", b3);
    return s;
}

struct CaseResult {
    std::string keep_line;
    std::string values_line;
};

// Run one multi-target case: host + proposer, the host KEEPs the
// proposer's worldline, and the three function values are reported.
CaseResult run_case(const std::string& d, const char* b1, const char* b2, const char* b3,
                    const std::string& sock, const char* mutable_targets) {
    CaseResult r;
    check(write_file(d + "/host.aura", kHostScript), "write host");
    check(write_file(d + "/prop.aura", proposer_with_bodies(b1, b2, b3).c_str()), "write proposer");
    int host_out = -1, host_in = -1;
    const pid_t host = run_script(d + "/host.aura", &host_out, &host_in, mutable_targets);
    check(host > 0, "fork host");
    check(wait_socket(sock), "host socket appeared");
    const std::string hready = read_until(host_out, "ready", 10000);
    check(has(hready, "host-ok"), "host started");

    int prop_out = -1, prop_in = -1;
    const pid_t prop = run_script(d + "/prop.aura", &prop_out, &prop_in, nullptr);
    check(prop > 0, "fork proposer");
    const std::string pready = read_until(prop_out, "prop-ready", 10000);
    check(has(pready, "prop-ready"), "proposer ready");
    const auto wlpos = pready.find("wl=");
    const auto wlend = pready.find('\n', wlpos);
    const std::string wl = pready.substr(wlpos + 3, wlend - wlpos - 3);

    const std::string cmd = "keep " + wl + "\n";
    check(::write(host_in, cmd.c_str(), cmd.size()) == static_cast<ssize_t>(cmd.size()),
          "send keep command");
    r.keep_line = read_until(host_out, "|", 10000);
    check(::write(host_in, "values\n", 7) == 7, "send values");
    r.values_line = read_until(host_out, "|", 10000);

    ::close(prop_in);
    ::kill(prop, SIGKILL);
    int pstatus = 0;
    waitpid(prop, &pstatus, 0);
    ::close(prop_out);
    ::close(host_in);
    const std::string hout = read_all(host_out);
    int hstatus = 0;
    waitpid(host, &hstatus, 0);
    check(has(hout, "HOST-DONE"), "host finished");
    return r;
}

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-multi-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8915);

    // 1. Three pure bodies in one worldline: all three are rewritten.
    {
        const CaseResult r = run_case(d, "(lambda (x) (+ x 10))", "(lambda (y) (+ y 20))",
                                      "(lambda (z) (+ z 30))", sock, nullptr);
        check(has(r.keep_line, "keep=ok"), "the multi-target KEEP succeeds");
        check(has(r.values_line, "v=11,21,31"), "all three functions are rewritten");
    }

    // 2. One broken body in the set: nothing is applied.
    {
        const CaseResult r = run_case(d, "(lambda (x) (+ x 10))", "(lambda (y) (+ y 20)",
                                      "(lambda (z) (+ z 30)", sock, nullptr);
        check(has(r.keep_line, "keep=validation-failed"), "a broken body refuses the whole set");
        check(has(r.values_line, "v=2,3,4"), "no function is half-applied");
    }

    // 3. A target outside the mutable allowlist: refused, nothing applied.
    {
        const CaseResult r = run_case(d, "(lambda (x) (+ x 10))", "(lambda (y) (+ y 20))",
                                      "(lambda (z) (+ z 30))", sock, "choose-fn,admit-fn");
        check(has(r.keep_line, "keep=validation-failed"), "a non-mutable target is refused");
        check(has(r.values_line, "v=2,3,4"), "the refused set changes nothing");
    }

    // 4. The allowlist that names every target lets the set through.
    {
        const CaseResult r = run_case(d, "(lambda (x) (+ x 10))", "(lambda (y) (+ y 20))",
                                      "(lambda (z) (+ z 30))", sock, "choose-fn,admit-fn,ttl-fn");
        check(has(r.keep_line, "keep=ok"), "the allowlisted set is applied");
        check(has(r.values_line, "v=11,21,31"), "the allowlisted set rewrites all three");
    }

    if (g_failed == 0)
        std::printf("test_bus_multi_target: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
