// Issue #5394: the host's degradation detector and the automatic
// round it triggers.
//
// The host watches a few key metrics on the metrics channel and runs an
// EWMA plus a one-sided CUSUM change test on them. A sustained
// degradation opens a round on its own; a stationary load never does;
// the configuration is versioned and audited; and the canary
// observation window reuses the same detector, so a candidate that
// degrades the metric is rolled back.

#include <cstdio>
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

#include "bus/degrade.hh"
#include "bus/image_schema.hh"
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

// A detector that watches one metric whose fall is a degradation.
DegradeConfig make_config() {
    DegradeConfig config{};
    DegradeKey key{};
    key.key = 9;
    key.higher_is_worse = true;
    config.keys.push_back(key);
    config.min_samples = 4;
    config.ewma_alpha_ppm = 250000;
    config.drift_ppm = 20000;
    config.limit_ppm = 200000;
    config.calm_samples = 2;
    return config;
}

// ── The detector ──────────────────────────────────────────────
void test_detector() {
    DegradeDetector detector;
    detector.configure(make_config());

    // A stationary load never fires: the deviation stays at zero.
    bool fired = false;
    for (int i = 0; i < 20; ++i)
        fired = detector.observe(9, 100, nullptr) || fired;
    check(!fired, "a stationary load never fires");
    check(detector.alerts() == 0, "no alert on a stationary load");
    check(detector.states().size() == 1, "the detector watches one metric");
    check(detector.states()[0].samples == 20, "every sample counts");

    // A phase shift fires: the level jumps.
    for (int i = 0; i < 3 && !fired; ++i)
        fired = detector.observe(9, 1000, nullptr);
    check(fired, "a phase shift fires");
    check(detector.alerts() == 1, "one alert");
    check(detector.last_change() > 0, "the change point is recorded");

    // The hysteresis: the metric must return to its baseline before a
    // new alert.
    fired = false;
    for (int i = 0; i < 2; ++i)
        fired = detector.observe(9, 1000, nullptr) || fired;
    check(!fired, "the hysteresis holds the second alert back");
    // The metric returns to its baseline: the change score decays back
    // to zero, and only then may the key alert again.
    for (int i = 0; i < 150; ++i)
        (void)detector.observe(9, 100, nullptr);
    fired = false;
    for (int i = 0; i < 3 && !fired; ++i)
        fired = detector.observe(9, 1000, nullptr);
    check(fired, "the detector fires again after the metric calmed down");
    check(detector.alerts() == 2, "two alerts");

    // A metric that is not watched is ignored.
    fired = detector.observe(42, 1000, nullptr);
    check(!fired, "an unwatched metric never fires");

    // The configuration is versioned and can be rolled back.
    DegradeDetector versioned;
    versioned.configure(make_config());
    const std::uint64_t first = versioned.version();
    DegradeConfig next = make_config();
    next.min_samples = 9;
    versioned.install(next);
    check(versioned.version() == first + 1, "the install bumps the version");
    check(versioned.config().min_samples == 9, "the new configuration is in force");
    check(versioned.rollback(), "the rollback restores the previous version");
    check(versioned.version() == first, "the rollback restores the version");
    check(versioned.config().min_samples == 4, "the rollback restores the settings");
    check(!versioned.rollback(), "there is no configuration to roll back to");
}

// ── The std/bus surface, driven through real aura processes ────
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

pid_t run_script(const std::string& script, int* out_fd, int* in_fd,
                 const char* degrade_keys = nullptr) {
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
        if (degrade_keys != nullptr) {
            setenv("AURA_BUS_DEGRADE_KEYS", degrade_keys, 1);
            setenv("AURA_BUS_DEGRADE_UP", degrade_keys, 1);
            setenv("AURA_BUS_DEGRADE_MIN_SAMPLES", "4", 1);
            setenv("AURA_BUS_DEGRADE_LIMIT", "200000", 1);
            setenv("AURA_BUS_DEGRADE_CALM", "2", 1);
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

// The host: it owns the bus and the detector.
const char* kHostScript = R"AURA(
(require "std/bus" all:)
(set-code "(define (score x) (+ x 1))")
(eval-current)
(define h (bus:host 8962))
(display "ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      (else (loop)))))
(bus:unhost 8962)
(display "HOST-DONE") (newline)
)AURA";

// The driver: a peer that reads the round and the detector state.
const char* kDriverScript = R"AURA(
(require "std/bus" all:)
(require "std/string" all:)
(define s (bus:attach "" 8962 0 127 255))
(define sid (car (cdr s)))
(bus:project-nodes sid)
(display "driver-ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string=? l "state")
       (define st (bus:round-state sid))
       (display "phase=") (display (list-ref st 1))
       (display " id=") (display (list-ref st 2))
       (display " opened=") (display (list-ref st 4))
       (display "|") (newline) (loop))
      ((string=? l "degrade")
       (define d (bus:degradation-state sid))
       (display "version=") (display (list-ref d 1))
       (display " samples=") (display (list-ref d 2))
       (display " alerts=") (display (list-ref d 3))
       (display " last=") (display (list-ref d 4))
       (display "|") (newline) (loop))
      ((string=? l "audit")
       (define a (bus:audit sid 0))
       (display "audit=") (display a) (display "|") (newline) (loop))
      ((string=? l "install")
       (define c (bus:degrade-install sid 6 250000 30000 300000 3 (list 9) (list)))
       (display "install=")
       (display (if (bus:ok? c) (list-ref c 1) (bus:code c)))
       (display "|") (newline) (loop))
      ((string=? l "rollback")
       (define c (bus:degrade-rollback sid))
       (display "rollback=")
       (display (if (bus:ok? c) (list-ref c 1) (bus:code c)))
       (display "|") (newline) (loop))
      (else (loop)))))
(bus:detach sid)
(display "DRIVER-DONE") (newline)
)AURA";

// The publisher: it feeds the metrics the detector watches.
const char* kPublisherScript = R"AURA(
(require "std/bus" all:)
(require "std/string" all:)
(define s (bus:attach "" 8962 1 7 255))
(define sid (car (cdr s)))
(display "pub-ready") (newline)
(let loop ()
  (let ((l (read-line)))
    (cond
      ((eof-object? l) #f)
      ((string-prefix? l "metric ")
       (define parts (string-split l " "))
       (define key (string->number (list-ref parts 1)))
       (define value (string->number (list-ref parts 2)))
       (define r (bus:publish sid key value))
       (display "published=") (display (if (bus:ok? r) "ok" (bus:message r)))
       (display "|") (newline) (loop))
      (else (loop)))))
(bus:detach sid)
(display "PUB-DONE") (newline)
)AURA";

} // namespace

int main() {
    test_detector();

    char tmpl[] = "/tmp/aura-bus-degrade-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);
    const std::string sock = default_socket_path(8962);
    check(write_file(d + "/host.aura", kHostScript), "write the host script");
    check(write_file(d + "/driver.aura", kDriverScript), "write the driver script");
    check(write_file(d + "/publisher.aura", kPublisherScript), "write the publisher script");

    // ── 1. A phase shift opens a round on its own ───────────────
    {
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in, "9");
        check(host > 0, "fork the host");
        check(wait_socket(sock), "the host socket appeared");
        check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
        int drv_out = -1, drv_in = -1;
        const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
        check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
              "the driver is ready");
        int pub_out = -1, pub_in = -1;
        const pid_t publisher = run_script(d + "/publisher.aura", &pub_out, &pub_in);
        check(has(read_until(pub_out, "pub-ready", 20000), "pub-ready"), "the publisher is ready");

        // A stationary load: no round opens.
        for (int i = 0; i < 6; ++i) {
            send_cmd(pub_in, "metric 9 100\n");
            (void)read_until(pub_out, "|", 10000);
        }
        send_cmd(drv_in, "state\n");
        const std::string quiet = read_until(drv_out, "|", 10000);
        check(has(quiet, "opened=0"), "a stationary load opens no round");
        send_cmd(drv_in, "degrade\n");
        const std::string detector = read_until(drv_out, "|", 10000);
        check(has(detector, "alerts=0"), "the detector stays quiet on a stationary load");

        // The phase shift: the host opens the round itself.
        for (int i = 0; i < 3; ++i) {
            send_cmd(pub_in, "metric 9 1000\n");
            (void)read_until(pub_out, "|", 10000);
        }
        send_cmd(drv_in, "state\n");
        const std::string opened = read_until(drv_out, "|", 10000);
        check(has(opened, "opened=1"), "the phase shift opened a round");
        check(has(opened, "phase=1"), "the round is in the signal phase");
        send_cmd(drv_in, "degrade\n");
        const std::string fired = read_until(drv_out, "|", 10000);
        check(has(fired, "alerts=1"), "the detector fired once");
        send_cmd(drv_in, "audit\n");
        const std::string audit = read_until(drv_out, "|", 10000);
        // DegradeAlert = 27, RoundOpen = 20.
        check(has(audit, "27"), "the audit carries the degradation alert");
        check(has(audit, "20"), "the audit carries the round opening");

        ::close(pub_in);
        ::close(drv_in);
        ::close(host_in);
        read_all(host_out);
        read_all(drv_out);
        read_all(pub_out);
        int status = 0;
        waitpid(host, &status, 0);
        kill(driver, SIGKILL);
        kill(publisher, SIGKILL);
        waitpid(driver, &status, 0);
        waitpid(publisher, &status, 0);
    }

    // ── 2. The configuration is versioned, audited and reversible ─
    {
        ::unlink(sock.c_str());
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(d + "/host.aura", &host_out, &host_in, "9");
        check(wait_socket(sock), "the host socket appeared");
        check(has(read_until(host_out, "ready", 20000), "ready"), "the host is ready");
        int drv_out = -1, drv_in = -1;
        const pid_t driver = run_script(d + "/driver.aura", &drv_out, &drv_in);
        check(has(read_until(drv_out, "driver-ready", 20000), "driver-ready"),
              "the driver is ready");
        send_cmd(drv_in, "install\n");
        const std::string installed = read_until(drv_out, "|", 10000);
        check(has(installed, "install=2"), "the install bumps the version");
        send_cmd(drv_in, "rollback\n");
        const std::string rolled = read_until(drv_out, "|", 10000);
        check(has(rolled, "rollback=1"), "the rollback restores the version");
        send_cmd(drv_in, "audit\n");
        const std::string audit = read_until(drv_out, "|", 10000);
        // DegradeConfig = 28.
        check(has(audit, "28"), "the audit carries the configuration change");

        ::close(drv_in);
        ::close(host_in);
        read_all(host_out);
        read_all(drv_out);
        int status = 0;
        waitpid(host, &status, 0);
        kill(driver, SIGKILL);
        waitpid(driver, &status, 0);
    }

    if (g_failed == 0)
        std::printf("test_bus_degradation_trigger: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
