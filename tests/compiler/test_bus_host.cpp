// Issue #5128: the workspace owner hosts the bus from Aura. Two real
// aura processes over std/bus only — the host script calls bus:host and
// serves; the peer script attaches, publishes, queries, trials, keeps
// under an installed constraint, rolls back and reads the audit. No C++
// coordinator harness: that is the point (#5121's process model).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
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

// Run an aura script with stdin from a pipe the caller keeps open.
// Returns the pid; stdout+stderr go to a pipe the caller reads.
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

bool has(const std::string& out, const char* line) {
    return out.find(line) != std::string::npos;
}

const char* kHostScript = R"AURA(
(require "std/bus" all:)
(define h (bus:host 777))
(display (if (bus:ok? h) "host-ok" "host-FAIL")) (newline)
(display (if (equal? h (bus:host 777)) "idem-ok" "idem-FAIL")) (newline)
(display (if (= 1 (car (cdr (bus:host? 777)))) "active-ok" "active-FAIL")) (newline)
(let loop ()
  (let ((l (read-line)))
    (if (eof-object? l) #f (loop))))
(display (if (bus:ok? (bus:unhost 777)) "unhost-ok" "unhost-FAIL")) (newline)
(display (if (= 0 (car (cdr (bus:host? 777)))) "inactive-ok" "inactive-FAIL")) (newline)
(display "HOST-DONE") (newline)
)AURA";

const char* kPeerScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 777 1 127 255))
(display (if (bus:ok? s) "attach-ok" "attach-FAIL")) (newline)
(define sid (car (cdr s)))
(display (if (bus:ok? (bus:subscribe sid 1)) "sub-ok" "sub-FAIL")) (newline)
(display (if (bus:ok? (bus:publish sid 0 42)) "pub-ok" "pub-FAIL")) (newline)
(define q (bus:query sid 1 0 0))
(display (if (bus:ok? q) "query-ok" "query-FAIL")) (newline)
(define tr (bus:trial sid 1))
(display (if (bus:ok? tr) "trial-ok" "trial-FAIL")) (newline)
(define wl (car (cdr tr)))
(display (if (bus:ok? (bus:keep sid wl 0 5)) "keep-ok" "keep-FAIL")) (newline)
(define tr2 (bus:trial sid 1))
(define wl2 (car (cdr tr2)))
(define ic (bus:install-constraint sid "allow-small"
             "(lambda (ctx) (<= (ctx 'new-value) 100))"))
(display (if (bus:ok? ic) "constraint-ok" "constraint-FAIL")) (newline)
(define bad (bus:keep sid wl2 0 200))
(display (if (bus:error? bad) "gate-rejected" "gate-FAIL")) (newline)
(display (if (bus:ok? (bus:keep sid wl2 0 5)) "keep2-ok" "keep2-FAIL")) (newline)
(define tr3 (bus:trial sid 1))
(display (if (bus:ok? (bus:rollback sid (car (cdr tr3)))) "rollback-ok" "rollback-FAIL")) (newline)
(define a (bus:audit sid 0))
(display (if (and (bus:ok? a) (> (car (cdr a)) 0)) "audit-ok" "audit-FAIL")) (newline)
(display (if (bus:ok? (bus:detach sid)) "detach-ok" "detach-FAIL")) (newline)
(display "PEER-DONE") (newline)
)AURA";

// Same-process host + attach: the owning process can also be a peer.
const char* kSelfScript = R"AURA(
(require "std/bus" all:)
(define h (bus:host 779))
(display (if (bus:ok? h) "self-host-ok" "self-host-FAIL")) (newline)
(define s (bus:attach "" 779 1 127 255))
(display (if (bus:ok? s) "self-attach-ok" "self-attach-FAIL")) (newline)
(define sid (car (cdr s)))
(display (if (bus:ok? (bus:publish sid 0 7)) "self-pub-ok" "self-pub-FAIL")) (newline)
(display (if (bus:ok? (bus:detach sid)) "self-detach-ok" "self-detach-FAIL")) (newline)
(display (if (bus:ok? (bus:unhost 779)) "self-unhost-ok" "self-unhost-FAIL")) (newline)
(display "SELF-DONE") (newline)
)AURA";

const char* kNoHostScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 778 1 127 255))
(display (if (bus:error? s) (bus:code s) "no-error")) (newline)
(display "NOHOST-DONE") (newline)
)AURA";

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-bus-host-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);

    // 1. No host: attach fails with a transport error, not a hang.
    {
        const std::string script = d + "/nohost.aura";
        check(write_file(script, kNoHostScript), "write nohost script");
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(script, &out_fd, &in_fd);
        check(pid > 0, "fork nohost");
        ::close(in_fd);
        const std::string out = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(out, "transport-failed"), "attach without host reports transport-failed");
        check(has(out, "NOHOST-DONE"), "nohost script finished");
    }

    // 2. Same-process host + peer.
    {
        const std::string script = d + "/self.aura";
        check(write_file(script, kSelfScript), "write self script");
        int out_fd = -1, in_fd = -1;
        const pid_t pid = run_script(script, &out_fd, &in_fd);
        check(pid > 0, "fork self");
        ::close(in_fd);
        const std::string out = read_all(out_fd);
        int status = 0;
        waitpid(pid, &status, 0);
        check(has(out, "self-host-ok"), "self host");
        check(has(out, "self-attach-ok"), "self attach");
        check(has(out, "self-pub-ok"), "self publish");
        check(has(out, "self-detach-ok"), "self detach");
        check(has(out, "self-unhost-ok"), "self unhost");
        check(has(out, "SELF-DONE"), "self script finished");
    }

    // 3. Two aura processes: host serves, peer exercises the surface.
    {
        const std::string host_script = d + "/host.aura";
        const std::string peer_script = d + "/peer.aura";
        check(write_file(host_script, kHostScript), "write host script");
        check(write_file(peer_script, kPeerScript), "write peer script");
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(host_script, &host_out, &host_in);
        check(host > 0, "fork host");
        // Wait for the socket the host creates in listen_at.
        const std::string sock = default_socket_path(777);
        bool ready = false;
        for (int i = 0; i < 200 && !ready; ++i) {
            struct stat st{};
            ready = ::stat(sock.c_str(), &st) == 0;
            if (!ready)
                usleep(50000);
        }
        check(ready, "host socket appeared");
        int peer_out = -1, peer_in = -1;
        const pid_t peer = run_script(peer_script, &peer_out, &peer_in);
        check(peer > 0, "fork peer");
        ::close(peer_in);
        const std::string pout = read_all(peer_out);
        int pstatus = 0;
        waitpid(peer, &pstatus, 0);
        check(has(pout, "attach-ok"), "peer attach (role perf)");
        check(has(pout, "sub-ok"), "peer subscribe");
        check(has(pout, "pub-ok"), "peer publish");
        check(has(pout, "query-ok"), "peer query");
        check(has(pout, "trial-ok"), "peer trial");
        check(has(pout, "keep-ok"), "peer keep under empty constraint set");
        check(has(pout, "constraint-ok"), "peer installs allow-small");
        check(has(pout, "gate-rejected"), "keep 200 rejected by the constraint");
        check(has(pout, "keep2-ok"), "keep 5 passes after rejection (worldline stayed pending)");
        check(has(pout, "rollback-ok"), "peer rollback");
        check(has(pout, "audit-ok"), "peer reads the shared audit");
        check(has(pout, "detach-ok"), "peer detach");
        check(has(pout, "PEER-DONE"), "peer script finished");
        // EOF on the host's stdin ends its serve loop; it unhosts.
        ::close(host_in);
        const std::string hout = read_all(host_out);
        int hstatus = 0;
        waitpid(host, &hstatus, 0);
        check(has(hout, "host-ok"), "host started");
        check(has(hout, "idem-ok"), "host is idempotent per workspace");
        check(has(hout, "active-ok"), "host? reports active");
        check(has(hout, "unhost-ok"), "host unhosted");
        check(has(hout, "inactive-ok"), "host? reports inactive after unhost");
        check(has(hout, "HOST-DONE"), "host script finished");
        struct stat st{};
        check(::stat(sock.c_str(), &st) != 0, "socket unlinked after unhost");
    }

    if (g_failed == 0)
        std::printf("test_bus_host: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
