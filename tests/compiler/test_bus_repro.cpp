// Issue #5397: the reproduction package.
//
// A KEEP's admission decision must be re-runnable offline: the package
// carries the evidence five-tuple, the held-out windows, the policy
// snapshot and the verdict, so the same decision can be recomputed
// without the coordinator and must land on the same answer.
//
// The test drives a real host: the judge binds the held-out evidence of
// a candidate, the candidate is KEPT, and the package the host exports
// is re-run by `aura bus-repro` — the same binary, the same gate math.

#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bus/gate.hh"
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
        // The host enforces the statistical admission, so a KEEP needs
        // the held-out evidence.
        setenv("AURA_BUS_GATE", "1", 1);
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

// Run `aura bus-repro FILE` as a child and collect its output. The
// verifier is a separate process on purpose: the package must decide
// the same thing with no coordinator and no live bus.
int run_bus_repro(const std::string& file, std::string* output) {
    int out[2];
    if (::pipe(out) != 0)
        return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(out[0]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        ::close(out[1]);
        const std::string exe = aura_binary();
        execl(exe.c_str(), "aura", "bus-repro", file.c_str(), static_cast<char*>(nullptr));
        _exit(24);
    }
    ::close(out[1]);
    char buf[512];
    ssize_t n;
    while ((n = ::read(out[0], buf, sizeof(buf))) > 0)
        output->append(buf, static_cast<std::size_t>(n));
    ::close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

// The host owns (define (score x) (+ x 1)), hosts the bus, binds the
// held-out evidence of a candidate, KEEPs it, and prints the
// reproduction package the coordinator exported for it.
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host 8945))
(define ms (bus:attach "" 8945 3 127 255))
(define msid (car (cdr ms)))
(bus:project-nodes msid)
(display "ready") (newline)
(define (open-candidate body)
  (define tr (bus:trial msid 3))
  (define wl (car (cdr tr)))
  (define src (bus:publish-source msid body))
  (bus:publish-patch msid 5 bus:source-field (car (cdr src)) wl 0)
  (define d (car (cdr (bus:candidate-digest msid wl))))
  (bus:bind-evidence msid wl 0 d 7 42 1)
  wl)
; The round needs a negative control the statistics reject.
(define cwl (open-candidate "(lambda (x) (- x 10))"))
(define cg (bus:bind-gate msid cwl 700 7 42 1 3 0 1 1 (list -900000 -800000 -700000)))
(display "bindc=") (display (if (bus:ok? cg) "ok" (bus:code cg))) (display "|") (newline)
(define wl (open-candidate "(lambda (x) (+ x 10))"))
; The judge binds the held-out windows of the candidate.
(define g (bus:bind-gate msid wl 700 7 42 1 3 0 0 1 (list 500000 600000 700000)))
(display "bind=") (display (if (bus:ok? g) "ok" (bus:code g))) (display "|") (newline)
(define k (bus:keep msid wl 0 0))
(display "keep=") (display (if (bus:ok? k) "ok" (bus:code k))) (display "|") (newline)
(define gv (bus:gate-evidence msid wl))
(display "verdict=") (display (car (cdr gv))) (display "|") (newline)
; The reproduction package: the last element is the JSON line.
(define rp (bus:repro msid wl))
(display "pkg=") (display (list-ref rp 5)) (display "|") (newline)
(display "DONE") (newline)
)AURA";

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-repro-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8945);

    check(write_file(d + "/host.aura", kHostScript), "write host");
    int host_out = -1, host_in = -1;
    const pid_t host = run_script(d + "/host.aura", &host_out, &host_in);
    check(host > 0, "fork host");
    check(wait_socket(sock), "host socket appeared");
    const std::string ready = read_until(host_out, "ready", 10000);
    check(has(ready, "host-ok") || !ready.empty(), "host started");

    const std::string bindc = read_until(host_out, "|", 10000);
    check(has(bindc, "bindc=ok"), "the negative control binds");
    const std::string bind = read_until(host_out, "|", 10000);
    check(has(bind, "bind=ok"), "the held-out evidence binds");
    const std::string keep = read_until(host_out, "|", 10000);
    check(has(keep, "keep=ok"), "the candidate is kept");
    const std::string verdict = read_until(host_out, "|", 10000);
    check(has(verdict, "verdict=1"), "the coordinator admits the candidate");
    const std::string pkg = read_until(host_out, "|", 10000);
    check(has(pkg, "pkg={"), "the host exports a reproduction package");

    // The package is one JSON line: archive it verbatim.
    const std::size_t at = pkg.find("pkg=");
    check(at != std::string::npos, "the package line is there");
    std::string json;
    if (at != std::string::npos) {
        const std::size_t end = pkg.rfind('|');
        json = pkg.substr(at + 4, end == std::string::npos ? std::string::npos : end - at - 4);
    }
    check(!json.empty(), "the package is not empty");
    // The package carries what the decision was made from.
    check(json.find("\"windows\":[500000,600000,700000]") != std::string::npos,
          "the package carries the held-out windows");
    check(json.find("\"seed\":42") != std::string::npos, "the package carries the seed");
    check(json.find("\"fingerprint\":") != std::string::npos,
          "the package carries the fingerprint");

    const std::string pkg_file = d + "/packages.jsonl";
    check(write_file(pkg_file, json.c_str()), "write the package file");

    ::close(host_in);
    ::kill(host, SIGKILL);
    int hstatus = 0;
    waitpid(host, &hstatus, 0);
    ::close(host_out);

    // The offline re-run: the same binary, the same gate math, no bus.
    {
        std::string output;
        const int rc = run_bus_repro(pkg_file, &output);
        check(rc == 0, "the package reproduces offline");
        check(has(output, "1 package(s)"), "the verifier counted the package");
        check(has(output, "0 mismatch"), "the verdict reproduces");
    }

    // A tampered package does not reproduce: the fingerprint and the
    // statistic both move.
    {
        std::string tampered = json;
        const std::size_t w = tampered.find("\"windows\":[500000,600000,700000]");
        check(w != std::string::npos, "the windows are in the package");
        if (w != std::string::npos)
            tampered.replace(w, std::strlen("\"windows\":[500000,600000,700000]"),
                             "\"windows\":[500000,600000,900000]");
        const std::string bad_file = d + "/tampered.jsonl";
        check(write_file(bad_file, tampered.c_str()), "write the tampered package");
        std::string output;
        const int rc = run_bus_repro(bad_file, &output);
        check(rc == 1, "the tampered package does not reproduce");
        check(has(output, "MISMATCH"), "the mismatch is reported");
    }

    if (g_failed == 0)
        std::printf("test_bus_repro: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
