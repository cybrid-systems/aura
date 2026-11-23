// Issue #5422: the supervisor's execution layer. The host's plan comes
// from real slack (fixtures pin it); --spawn-once starts the workers the
// plan allows, waits for them, and the audit records both the plan and
// the spawn. A low-slack fixture spawns nothing and audits the refusal.

#include "bus/transport.hh"

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
        bool saw_worker = false;
        for (int i = 0; i < 40 && !saw_worker; ++i) {
            saw_worker = count_workers(kPort) >= 1;
            if (!saw_worker)
                usleep(50000);
        }
        check(saw_worker, "continuous supervisor has live workers");
        ::kill(supervisor, SIGTERM);
        int status = 0;
        waitpid(supervisor, &status, 0);
        ::close(out);
        usleep(1000000); // PDEATHSIG + terminate grace
        check(count_workers(kPort) == 0, "no worker outlives a stopped supervisor");

        ::close(host_in);
        waitpid(host, &status, 0);
    }

    if (g_failed == 0)
        std::printf("test_bus_agent_spawn: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
