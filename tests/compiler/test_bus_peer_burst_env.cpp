// Issue #5469: AURA_BUS_PEER_BURST is a pinned bucket.
//
// The host used to treat the historical default 20000 as "unset" and
// derive a multi-million-frame bucket from the image, so an operator who
// pinned the documented 20000 got no limit. The env var being present
// must now win even when its value is 20000.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <poll.h>
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

// Read until the child closes stdout. Stopping at the marker would
// close the pipe while the child is still writing and turn its exit
// into SIGPIPE.
std::string read_to_eof(int fd, int timeout_ms) {
    std::string s;
    char buf[512];
    int waited = 0;
    while (waited < timeout_ms) {
        struct pollfd pfd{fd, POLLIN, 0};
        const int pr = ::poll(&pfd, 1, 50);
        waited += 50;
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP)) != 0) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            s.append(buf, static_cast<std::size_t>(n));
        }
    }
    return s;
}

const char* kFloodScript = R"AURA(
(require "std/bus" all:)
(define h (bus:host 9197))
(define a (bus:attach "" 9197 0 127 255))
(define s (car (cdr a)))
(define stopped -1)
(let loop ((i 0))
  (if (and (< i 40000) (< stopped 0))
      (if (bus:ok? (bus:subscribe s 1))
          (loop (+ i 1))
          (set! stopped i))
      #f))
(display "stopped-at=") (display stopped) (newline)
(bus:detach s)
(bus:unhost 9197)
(display "DONE") (newline)
)AURA";

// Run the flood with a pinned burst and return the first refused frame.
long run_flood(const std::string& script, const std::string& burst, const std::string& runtime_dir,
               bool* ok) {
    *ok = false;
    ::mkdir(runtime_dir.c_str(), 0700);
    int out[2];
    if (::pipe(out) != 0)
        return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(out[0]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        ::close(out[1]);
        setenv("AURA_SANDBOX", "off", 1);
        setenv("XDG_RUNTIME_DIR", runtime_dir.c_str(), 1);
        setenv("AURA_BUS_PEER_BURST", burst.c_str(), 1);
        setenv("AURA_BUS_PEER_FRAMES", "1", 1);
        const std::string exe = aura_binary();
        execl(exe.c_str(), "aura", script.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(out[1]);
    if (pid < 0) {
        ::close(out[0]);
        return -1;
    }
    const std::string text = read_to_eof(out[0], 60000);
    ::close(out[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    *ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && text.find("DONE") != std::string::npos;
    const auto at = text.find("stopped-at=");
    if (at == std::string::npos)
        return -1;
    return std::strtol(text.c_str() + at + std::strlen("stopped-at="), nullptr, 10);
}

} // namespace

int main() {
    char tmpl[] = "/tmp/aura-burst-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    check(dir != nullptr, "temp dir");
    if (!dir)
        return 1;
    const std::string script = std::string(dir) + "/flood.aura";
    FILE* f = std::fopen(script.c_str(), "w");
    check(f != nullptr, "write script");
    if (f != nullptr) {
        std::fputs(kFloodScript, f);
        std::fclose(f);
    }

    // #5469: the documented 20000 must pin the bucket just like 19999.
    for (const char* burst : {"19999", "20000"}) {
        bool ok = false;
        const long stopped = run_flood(script, burst, std::string(dir) + "/rt-" + burst, &ok);
        std::printf("burst=%s stopped-at=%ld ok=%d\n", burst, stopped, ok ? 1 : 0);
        check(ok, burst);
        check(stopped >= 19000 && stopped <= 26000, "the pinned bucket trips near 20k frames");
    }
    return g_failed == 0 ? 0 : 1;
}
