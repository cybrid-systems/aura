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

// Issue #5128 completion line: the image and audit outlive peer sessions.
// A perf peer publishes, trials, keeps and detaches; a harness peer then
// attaches to the SAME host and must read the committed records, and a
// constraint the perf peer installed must still gate the next KEEP.
const char* kPerfPeerScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 780 1 95 255))
(display (if (bus:ok? s) "perf-attach-ok" "perf-attach-FAIL")) (newline)
(define sid (car (cdr s)))
(display (if (bus:ok? (bus:publish sid 1 100)) "perf-pub-ok" "perf-pub-FAIL")) (newline)
(define tr (bus:trial sid 1))
(define wl (car (cdr tr)))
(display (if (bus:ok? (bus:keep sid wl 0 50)) "perf-keep-ok" "perf-keep-FAIL")) (newline)
(define ic (bus:install-constraint sid "allow-small"
             "(lambda (ctx) (<= (ctx 'new-value) 100))"))
(display (if (bus:ok? ic) "perf-constraint-ok" "perf-constraint-FAIL")) (newline)
(define tr2 (bus:trial sid 1))
(bus:project-nodes sid)
(define badpatch (bus:publish-patch sid 999999 0 50 (car (cdr tr2)) 0))
(display (if (bus:error? badpatch) "perf-badpatch-rejected" "perf-badpatch-FAIL")) (newline)
(bus:publish-patch sid 0 0 50 (car (cdr tr2)) 0)
(display (if (bus:ok? (bus:detach sid)) "perf-detach-ok" "perf-detach-FAIL")) (newline)
(display "PERF-DONE") (newline)
)AURA";

const char* kHarnessPeerScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 780 2 1 255))
(display (if (bus:ok? s) "harness-attach-ok" "harness-attach-FAIL")) (newline)
(define sid (car (cdr s)))
(define q (bus:query sid 1 0 0))
(display (if (and (bus:ok? q) (= 1 (car (cdr (cdr q))))
                  (equal? (list-ref q 4) '((1 100))))
             "harness-sees-metric" "harness-metric-FAIL")) (newline)
(define qp (bus:query sid 3 0 0))
(display (if (and (bus:ok? qp) (= 1 (car (cdr (cdr qp)))))
             "harness-sees-patch" "harness-patch-FAIL")) (newline)
(define c (bus:constraints sid))
(display (if (and (bus:ok? c) (= 1 (car (cdr (cdr c)))))
             "harness-sees-constraint" "harness-constraint-FAIL")) (newline)
(define a (bus:audit sid 0))
(display (if (and (bus:ok? a) (> (car (cdr a)) 5))
             "harness-sees-audit" "harness-audit-FAIL")) (newline)
(display (if (bus:ok? (bus:detach sid)) "harness-detach-ok" "harness-detach-FAIL")) (newline)
(display "HARNESS-DONE") (newline)
)AURA";

const char* kMaintainerPeerScript = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" 780 3 31 255))
(display (if (bus:ok? s) "maint-attach-ok" "maint-attach-FAIL")) (newline)
(define sid (car (cdr s)))
(define tr (bus:trial sid 1))
(define wl (car (cdr tr)))
(define bad (bus:keep sid wl 0 200))
(display (if (bus:error? bad) "maint-gate-rejected" "maint-gate-FAIL")) (newline)
(display (if (bus:ok? (bus:keep sid wl 0 5)) "maint-keep-ok" "maint-keep-FAIL")) (newline)
(define a (bus:audit sid wl))
(display (if (and (bus:ok? a) (> (car (cdr a)) 0))
             "maint-sees-wl-audit" "maint-audit-FAIL")) (newline)
(display (if (bus:ok? (bus:detach sid)) "maint-detach-ok" "maint-detach-FAIL")) (newline)
(display "MAINT-DONE") (newline)
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

    // 4. Three sequential peers on one host (#5128): perf publishes and
    // installs a constraint, harness reads the SAME image and audit,
    // maintainer's KEEP is gated by perf's constraint. Committed evidence
    // outlives the session that wrote it; pending trials do not.
    {
        const std::string host_script = d + "/host2.aura";
        std::string hs = kHostScript;
        const std::string from = "777", to = "780";
        for (std::size_t at = hs.find(from); at != std::string::npos;
             at = hs.find(from, at + to.size()))
            hs.replace(at, from.size(), to);
        check(write_file(host_script, hs.c_str()), "write host2 script");
        check(write_file((d + "/perf.aura").c_str(), kPerfPeerScript), "write perf script");
        check(write_file((d + "/harness.aura").c_str(), kHarnessPeerScript),
              "write harness script");
        check(write_file((d + "/maint.aura").c_str(), kMaintainerPeerScript), "write maint script");
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(host_script, &host_out, &host_in);
        check(host > 0, "fork host2");
        const std::string sock = default_socket_path(780);
        bool ready = false;
        for (int i = 0; i < 200 && !ready; ++i) {
            struct stat st{};
            ready = ::stat(sock.c_str(), &st) == 0;
            if (!ready)
                usleep(50000);
        }
        check(ready, "host2 socket appeared");
        const auto run_peer = [&](const char* name) {
            int out_fd = -1, in_fd = -1;
            const pid_t pid = run_script(d + "/" + name, &out_fd, &in_fd);
            check(pid > 0, "fork peer");
            ::close(in_fd);
            const std::string out = read_all(out_fd);
            int status = 0;
            waitpid(pid, &status, 0);
            return out;
        };
        const std::string perf = run_peer("perf.aura");
        check(has(perf, "perf-attach-ok"), "perf attaches");
        check(has(perf, "perf-pub-ok"), "perf publishes a metric");
        check(has(perf, "perf-keep-ok"), "perf keeps a trial");
        check(has(perf, "perf-constraint-ok"), "perf installs allow-small");
        check(has(perf, "perf-badpatch-rejected"), "out-of-range patch node rejected at publish");
        check(has(perf, "perf-detach-ok"), "perf detaches");
        check(has(perf, "PERF-DONE"), "perf finished");
        const std::string harness = run_peer("harness.aura");
        check(has(harness, "harness-attach-ok"), "harness attaches after perf");
        check(has(harness, "harness-sees-metric"), "harness reads perf's committed metric");
        check(has(harness, "harness-sees-patch"), "harness reads perf's committed patch");
        check(has(harness, "harness-sees-constraint"), "harness reads the installed constraint");
        check(has(harness, "harness-sees-audit"), "harness reads the shared audit");
        check(has(harness, "harness-detach-ok"), "harness detaches");
        check(has(harness, "HARNESS-DONE"), "harness finished");
        const std::string maint = run_peer("maint.aura");
        check(has(maint, "maint-attach-ok"), "maintainer attaches third");
        check(has(maint, "maint-gate-rejected"), "perf's constraint gates maintainer KEEP 200");
        check(has(maint, "maint-keep-ok"), "maintainer KEEP 5 passes");
        check(has(maint, "maint-sees-wl-audit"), "maintainer reads its worldline's audit");
        check(has(maint, "maint-detach-ok"), "maintainer detaches");
        check(has(maint, "MAINT-DONE"), "maintainer finished");
        ::close(host_in);
        const std::string hout = read_all(host_out);
        int hstatus = 0;
        waitpid(host, &hstatus, 0);
        check(has(hout, "HOST-DONE"), "host2 finished");
        struct stat st{};
        check(::stat(sock.c_str(), &st) != 0, "socket unlinked after host2 unhost");
    }

    if (g_failed == 0)
        std::printf("test_bus_host: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
