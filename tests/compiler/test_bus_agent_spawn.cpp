// Issue #5422: the supervisor's execution layer. The host's plan comes
// from real slack (fixtures pin it); --spawn-once starts the workers the
// plan allows, waits for them, and the audit records both the plan and
// the spawn. A low-slack fixture spawns nothing and audits the refusal.

#include "bus/transport.hh"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>
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

pid_t run_agent(const std::vector<std::string>& args, int* out_fd, int* in_fd) {
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
        std::vector<std::string> full{"aura", "agent"};
        full.insert(full.end(), args.begin(), args.end());
        std::vector<char*> argv;
        argv.reserve(full.size() + 1);
        for (auto& a : full)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        execv(exe.c_str(), argv.data());
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

bool wait_exit_ms(pid_t pid, int* status, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t r = ::waitpid(pid, status, WNOHANG);
        if (r == pid || (r < 0 && errno == ECHILD))
            return true;
        ::usleep(20000);
    }
    return false;
}

std::vector<pid_t> children_of(pid_t pid) {
    std::vector<pid_t> out;
    std::ifstream in("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children",
                     std::ios::binary);
    int child = 0;
    while (in >> child)
        out.push_back(static_cast<pid_t>(child));
    return out;
}

int count_workers(std::uint64_t workspace) {
    DIR* proc = ::opendir("/proc");
    if (proc == nullptr)
        return -1;
    const std::string ws = std::to_string(workspace);
    int n = 0;
    while (const dirent* e = ::readdir(proc)) {
        bool numeric = e->d_name[0] != '\0';
        for (const char* p = e->d_name; *p; ++p)
            numeric = numeric && *p >= '0' && *p <= '9';
        if (!numeric)
            continue;
        std::ifstream in(std::string("/proc/") + e->d_name + "/cmdline", std::ios::binary);
        const std::string cmd((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        if (cmd.find("worker") != std::string::npos && cmd.find(ws) != std::string::npos)
            ++n;
    }
    ::closedir(proc);
    return n;
}

const char* kHostTemplate = R"AURA(
(require "std/bus" all:)
(define h (bus:host {{port}}))
(display (if (bus:ok? h) "host-ready" "host-FAIL")) (newline)
(let loop ()
  (let ((l (read-line)))
    (if (eof-object? l) #f (loop))))
)AURA";

const char* kVerifyTemplate = R"AURA(
(require "std/bus" all:)
(define s (bus:attach "" {{port}} 0 255 255))
(define sid (car (cdr s)))
(define (state id) (list-ref (bus:task-status sid id) 2))
(display "s1=") (display (state 1)) (display "|") (newline)
(display "s2=") (display (state 2)) (display "|") (newline)
(display "s3=") (display (state 3)) (display "|") (newline)
(define a (bus:audit-filter sid 0 0 1))
(define rows (list-ref a 3))
(define plan-count 0)
(define spawn-count 0)
(define plan-reason -1)
(define spawn-result 0)
(define spawn-alive 0)
(for-each
  (lambda (r)
    (if (= (list-ref r 0) 48)
      (begin
        (set! plan-count (+ plan-count 1))
        (set! plan-reason (list-ref r 3))
        (display "plan-version=") (display (list-ref r 4)) (display "|") (newline)))
    (if (= (list-ref r 0) 49)
      (begin
        (set! spawn-count (+ spawn-count 1))
        (set! spawn-result (list-ref r 3))
        (set! spawn-alive (list-ref r 4)))))
  rows)
(display "plan-count=") (display plan-count) (display "|") (newline)
(display "plan-reason=") (display plan-reason) (display "|") (newline)
(display "spawn-count=") (display spawn-count) (display "|") (newline)
(display "spawn-result=") (display spawn-result) (display "|") (newline)
(display "spawn-alive=") (display spawn-alive) (display "|") (newline)
(display "VERIFY-DONE") (newline)
(bus:detach sid)
)AURA";

std::string make_host_script(const std::string& dir, std::uint64_t port) {
    std::string text(kHostTemplate);
    const std::string from = "{{port}}";
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
        text.replace(at, from.size(), std::to_string(port));
    }
    const std::string path = dir + "/host-" + std::to_string(port) + ".aura";
    write_file(path, text.c_str());
    return path;
}

std::string make_verify_script(const std::string& dir, std::uint64_t port) {
    std::string text(kVerifyTemplate);
    const std::string from = "{{port}}";
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
        text.replace(at, from.size(), std::to_string(port));
    }
    const std::string path = dir + "/verify-" + std::to_string(port) + ".aura";
    write_file(path, text.c_str());
    return path;
}

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-agent-spawn-XXXXXX";
    char* dir = mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    check(setenv("XDG_RUNTIME_DIR", dir, 1) == 0, "setenv XDG_RUNTIME_DIR");
    const std::string d(dir);

    // ── enough slack: desired 2, spawn 2, drain 3 tasks ──────────
    {
        constexpr std::uint64_t kPort = 8955;
        setenv("AURA_SUPERVISOR_MEM_FREE_MB", "512", 1);
        setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "20", 1);
        setenv("AURA_SUPERVISOR_CPUS", "4", 1);
        const std::string host_script = make_host_script(d, kPort);
        const std::string sock = default_socket_path(kPort);
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(host_script, &host_out, &host_in);
        check(host > 0, "fork host");
        check(wait_socket(sock), "host socket appeared");
        check(has(read_until(host_out, "host-ready", 10000), "host-ready"), "host started");

        // The host has its copy of the fixtures; the child processes do
        // not need them.
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");

        // Planner posts three plain tasks (mock, no prompt).
        {
            int out = -1, in = -1;
            const pid_t planner =
                run_agent({"--role", "planner", "--workspace", std::to_string(kPort), "--mock",
                           "--tasks", "3", "--requirement", "spawn fixture"},
                          &out, &in);
            ::close(in);
            const std::string text = read_all(out);
            check(has(text, "posted=3"), "planner posted three tasks");
            int status = 0;
            waitpid(planner, &status, 0);
        }

        // One spawn cycle: two workers drain the three tasks.
        {
            int out = -1, in = -1;
            const pid_t supervisor =
                run_agent({"--role", "supervisor", "--workspace", std::to_string(kPort), "--mock",
                           "--spawn-once", "--interval-ms", "50"},
                          &out, &in);
            ::close(in);
            const std::string text = read_all(out);
            check(has(text, "supervisor-desired=2"), "plan admits two workers");
            check(has(text, "spawned=2"), "supervisor spawned two workers");
            check(has(text, "supervisor-done spawned=2 alive=0"), "workers reaped cleanly");
            // The workers write concurrently into the same pipe, so the
            // board below (s1..s3 done) is the completion evidence, not a
            // line count.
            int status = 0;
            waitpid(supervisor, &status, 0);
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "supervisor exited 0");
        }

        // Verify the board and the audit: plan + spawn events.
        {
            const std::string verify_script = make_verify_script(d, kPort);
            int out = -1, in = -1;
            const pid_t verify = run_script(verify_script, &out, &in);
            ::close(in);
            const std::string text = read_all(out);
            check(has(text, "s1=2"), "task 1 done");
            check(has(text, "s2=2"), "task 2 done");
            check(has(text, "s3=2"), "task 3 done");
            check(has(text, "plan-version=2"), "the audited plan is two workers");
            check(has(text, "spawn-count=1"), "one spawn event");
            check(has(text, "spawn-result=2"), "the spawn event records two started");
            check(has(text, "spawn-alive=2"), "the spawn event records two live");
            int status = 0;
            waitpid(verify, &status, 0);
        }

        ::close(host_in);
        int status = 0;
        waitpid(host, &status, 0);
    }

    // #5458: --spawn-once against no host is a failed one-shot, not a
    // reconnect loop. It must exit non-zero within the first backoff.
    {
        constexpr std::uint64_t kPort = 8997;
        ::unlink(default_socket_path(kPort).c_str());
        int out = -1, in = -1;
        const pid_t supervisor = run_agent({"--role", "supervisor", "--workspace",
                                            std::to_string(kPort), "--mock", "--spawn-once"},
                                           &out, &in);
        ::close(in);
        check(supervisor > 0, "fork spawn-once without host");
        int status = 0;
        const bool exited = wait_exit_ms(supervisor, &status, 3000);
        check(exited, "spawn-once exits within a backoff period without a host");
        if (!exited) {
            ::kill(supervisor, SIGKILL);
            (void)::waitpid(supervisor, &status, 0);
        }
        check(WIFEXITED(status) && WEXITSTATUS(status) != 0,
              "spawn-once without a host exits non-zero");
        const std::string text = read_all(out);
        check(has(text, "supervisor-reconnect-wait="), "spawn-once reported the failed connect");
        ::close(out);
    }

    // #5458: a stopped host must not make the supervisor immune to SIGTERM
    // while it waits for a plan reply.
    {
        constexpr std::uint64_t kPort = 8960;
        setenv("AURA_SUPERVISOR_MEM_FREE_MB", "512", 1);
        setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "20", 1);
        setenv("AURA_SUPERVISOR_CPUS", "4", 1);
        const std::string host_script = make_host_script(d, kPort);
        const std::string sock = default_socket_path(kPort);
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(host_script, &host_out, &host_in);
        check(host > 0, "fork stopped-host test host");
        check(wait_socket(sock), "stopped-host socket appeared");
        check(has(read_until(host_out, "host-ready", 10000), "host-ready"), "stop host started");
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");

        int out = -1, in = -1;
        const pid_t supervisor =
            run_agent({"--role", "supervisor", "--workspace", std::to_string(kPort), "--mock",
                       "--spawn", "--interval-ms", "100"},
                      &out, &in);
        ::close(in);
        (void)read_until(out, "supervisor-desired=", 5000);
        ::kill(host, SIGSTOP);
        ::usleep(200000);
        ::kill(supervisor, SIGTERM);
        int status = 0;
        const bool exited = wait_exit_ms(supervisor, &status, 3000);
        check(exited, "SIGTERM exits while the host is stopped");
        if (!exited)
            ::kill(supervisor, SIGKILL);
        ::kill(host, SIGCONT);
        ::kill(host, SIGTERM);
        ::close(host_in);
        ::waitpid(host, &status, 0);
        ::close(out);
        ::close(host_out);
    }

    // #5458: workers get their own process group, so a terminal Ctrl-C
    // reaches the supervisor and the normal drain path, not the workers.
    {
        constexpr std::uint64_t kPort = 8961;
        setenv("AURA_SUPERVISOR_MEM_FREE_MB", "512", 1);
        setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "20", 1);
        setenv("AURA_SUPERVISOR_CPUS", "4", 1);
        const std::string host_script = make_host_script(d, kPort);
        const std::string sock = default_socket_path(kPort);
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(host_script, &host_out, &host_in);
        check(host > 0, "fork pgrp host");
        check(wait_socket(sock), "pgrp host socket appeared");
        check(has(read_until(host_out, "host-ready", 10000), "host-ready"), "pgrp host started");
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");

        {
            int pout = -1, pin = -1;
            const pid_t planner =
                run_agent({"--role", "planner", "--workspace", std::to_string(kPort), "--mock",
                           "--tasks", "64", "--requirement", "pgrp work"},
                          &pout, &pin);
            ::close(pin);
            const std::string text = read_all(pout);
            check(has(text, "posted=64"), "pgrp planner posted 64 tasks");
            int pstatus = 0;
            waitpid(planner, &pstatus, 0);
            ::close(pout);
        }

        int out = -1, in = -1;
        const pid_t supervisor =
            run_agent({"--role", "supervisor", "--workspace", std::to_string(kPort), "--mock",
                       "--spawn-once", "--interval-ms", "50"},
                      &out, &in);
        ::close(in);
        (void)read_until(out, "spawned=2", 15000);
        const std::vector<pid_t> workers = children_of(supervisor);
        check(!workers.empty(), "supervisor has live workers to inspect");
        for (const pid_t worker : workers)
            check(::getpgid(worker) == worker, "worker is in its own process group");
        // The process-group fact is set at fork; stop the supervisor now
        // instead of waiting for all 64 mock tasks. PDEATHSIG plus the
        // explicit kill clean the workers up.
        ::kill(supervisor, SIGKILL);
        for (const pid_t worker : workers)
            ::kill(worker, SIGKILL);
        int status = 0;
        ::waitpid(supervisor, &status, 0);
        ::close(out);
        ::close(host_in);
        ::waitpid(host, &status, 0);
        ::close(host_out);
    }

    // ── low slack: desired 0, nothing spawned, plan audited ──────
    {
        constexpr std::uint64_t kPort = 8956;
        setenv("AURA_SUPERVISOR_MEM_FREE_MB", "100", 1);
        setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "5", 1);
        setenv("AURA_SUPERVISOR_CPUS", "4", 1);
        const std::string host_script = make_host_script(d, kPort);
        const std::string sock = default_socket_path(kPort);
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(host_script, &host_out, &host_in);
        check(host > 0, "fork low-slack host");
        check(wait_socket(sock), "low-slack socket appeared");
        check(has(read_until(host_out, "host-ready", 10000), "host-ready"), "low host started");
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");

        {
            int out = -1, in = -1;
            const pid_t supervisor = run_agent({"--role", "supervisor", "--workspace",
                                                std::to_string(kPort), "--mock", "--spawn-once"},
                                               &out, &in);
            ::close(in);
            const std::string text = read_all(out);
            check(has(text, "supervisor-desired=0"), "low slack admits no worker");
            check(has(text, "spawned=0"), "low slack spawns nothing");
            int status = 0;
            waitpid(supervisor, &status, 0);
        }
        {
            const std::string verify_script = make_verify_script(d, kPort);
            int out = -1, in = -1;
            const pid_t verify = run_script(verify_script, &out, &in);
            ::close(in);
            const std::string text = read_all(out);
            check(has(text, "plan-reason=3"), "the refused plan is audited");
            check(has(text, "spawn-count=0"), "no spawn event on a refused plan");
            int status = 0;
            waitpid(verify, &status, 0);
        }

        ::close(host_in);
        int status = 0;
        waitpid(host, &status, 0);
    }


    // ── continuous mode: SIGTERM stops spawning, no orphans ───────
    {
        constexpr std::uint64_t kPort = 8957;
        setenv("AURA_SUPERVISOR_MEM_FREE_MB", "512", 1);
        setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "20", 1);
        setenv("AURA_SUPERVISOR_CPUS", "4", 1);
        const std::string host_script = make_host_script(d, kPort);
        const std::string sock = default_socket_path(kPort);
        int host_out = -1, host_in = -1;
        const pid_t host = run_script(host_script, &host_out, &host_in);
        check(host > 0, "fork signal host");
        check(wait_socket(sock), "signal host socket appeared");
        check(has(read_until(host_out, "host-ready", 10000), "host-ready"), "signal host started");
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");

        int out = -1, in = -1;
        const pid_t supervisor =
            run_agent({"--role", "supervisor", "--workspace", std::to_string(kPort), "--mock",
                       "--spawn", "--interval-ms", "100"},
                      &out, &in);
        ::close(in);
        // With no open tasks, capacity alone must not start an idle loop.
        const std::string idle =
            read_until(out, "supervisor-desired=", 5000) + read_until(out, "\n", 1000);
        check(has(idle, "open-tasks=0"), "idle supervisor sees no work");
        check(has(idle, "spawned=0"), "idle supervisor spawns nothing");

        // Work arrives after the supervisor is running: it must react.
        {
            int pout = -1, pin = -1;
            const pid_t planner =
                run_agent({"--role", "planner", "--workspace", std::to_string(kPort), "--mock",
                           "--tasks", "3", "--requirement", "late work"},
                          &pout, &pin);
            ::close(pin);
            const std::string text = read_all(pout);
            check(has(text, "posted=3"), "late planner posted three tasks");
            int pstatus = 0;
            waitpid(planner, &pstatus, 0);
        }
        const std::string spawned = read_until(out, "spawned=2", 15000);
        check(has(spawned, "spawned=2"), "supervisor starts workers for late work");

        // Wait for the board to drain.
        bool drained = false;
        for (int i = 0; i < 50 && !drained; ++i) {
            const std::string verify_script = make_verify_script(d, kPort);
            int vout = -1, vin = -1;
            const pid_t verify = run_script(verify_script, &vout, &vin);
            ::close(vin);
            const std::string text = read_all(vout);
            int vstatus = 0;
            waitpid(verify, &vstatus, 0);
            drained = has(text, "s1=2") && has(text, "s2=2") && has(text, "s3=2");
            if (!drained)
                usleep(200000);
        }
        check(drained, "late tasks completed");
        check(count_workers(kPort) == 0, "idle workers are reaped");

        ::kill(supervisor, SIGTERM);
        int status = 0;
        waitpid(supervisor, &status, 0);
        ::close(out);
        usleep(500000);
        check(count_workers(kPort) == 0, "no worker outlives a stopped supervisor");

        ::close(host_in);
        waitpid(host, &status, 0);
    }


    // ── crash takeover: the host dies, the supervisor reconnects ──
    {
        constexpr std::uint64_t kPort = 8958;
        setenv("AURA_SUPERVISOR_MEM_FREE_MB", "512", 1);
        setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "20", 1);
        setenv("AURA_SUPERVISOR_CPUS", "4", 1);
        const std::string host_script = make_host_script(d, kPort);
        const std::string sock = default_socket_path(kPort);
        int host_a_out = -1, host_a_in = -1;
        const pid_t host_a = run_script(host_script, &host_a_out, &host_a_in);
        check(host_a > 0, "fork first host");
        check(wait_socket(sock), "first host socket appeared");
        check(has(read_until(host_a_out, "host-ready", 10000), "host-ready"), "first host started");
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");

        int out = -1, in = -1;
        const pid_t supervisor =
            run_agent({"--role", "supervisor", "--workspace", std::to_string(kPort), "--mock",
                       "--spawn", "--interval-ms", "100", "--drain-ms", "1000"},
                      &out, &in);
        ::close(in);
        (void)read_until(out, "supervisor-desired=", 5000);

        // A hard kill leaves the socket behind; the take-over must work.
        ::kill(host_a, SIGKILL);
        int status = 0;
        waitpid(host_a, &status, 0);
        ::close(host_a_out);
        ::close(host_a_in);
        usleep(200000);

        setenv("AURA_SUPERVISOR_MEM_FREE_MB", "512", 1);
        setenv("AURA_SUPERVISOR_CPU_FREE_PCT", "20", 1);
        setenv("AURA_SUPERVISOR_CPUS", "4", 1);
        int host_b_out = -1, host_b_in = -1;
        const pid_t host_b = run_script(host_script, &host_b_out, &host_b_in);
        check(host_b > 0, "fork second host");
        // No wait_socket: the stale node may make it appear before bind.
        check(has(read_until(host_b_out, "host-ready", 10000), "host-ready"),
              "second host took over the stale socket");
        unsetenv("AURA_SUPERVISOR_MEM_FREE_MB");
        unsetenv("AURA_SUPERVISOR_CPU_FREE_PCT");
        unsetenv("AURA_SUPERVISOR_CPUS");

        {
            int pout = -1, pin = -1;
            const pid_t planner =
                run_agent({"--role", "planner", "--workspace", std::to_string(kPort), "--mock",
                           "--tasks", "2", "--requirement", "after crash"},
                          &pout, &pin);
            ::close(pin);
            const std::string text = read_all(pout);
            check(has(text, "posted=2"), "planner posted after the crash");
            int pstatus = 0;
            waitpid(planner, &pstatus, 0);
        }

        bool drained = false;
        for (int i = 0; i < 50 && !drained; ++i) {
            const std::string verify_script = make_verify_script(d, kPort);
            int vout = -1, vin = -1;
            const pid_t verify = run_script(verify_script, &vout, &vin);
            ::close(vin);
            const std::string text = read_all(vout);
            int vstatus = 0;
            waitpid(verify, &vstatus, 0);
            drained = has(text, "s1=2") && has(text, "s2=2");
            if (!drained)
                usleep(200000);
        }
        check(drained, "tasks completed after the host restart");

        ::kill(supervisor, SIGTERM);
        waitpid(supervisor, &status, 0);
        const std::string sup_text = read_all(out);
        check(has(sup_text, "host query failed") || has(sup_text, "supervisor-reconnect-wait="),
              "supervisor retried the dead host");
        check(count_workers(kPort) == 0, "restart leaves no worker behind");

        ::close(host_b_in);
        waitpid(host_b, &status, 0);
    }
    if (g_failed == 0)
        std::printf("test_bus_agent_spawn: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
