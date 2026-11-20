// Issue #5373: a stale short-circuit (and/or) name-cache row whose
// pool-local SymId was reused by a later form hijacked that form. A
// top-level `display` of an inline-computed list was evaluated as the
// cached and/or, so the display primitive never ran and its output was
// silently dropped (exit 0). The fix validates the binding generation
// (top_gen) on the short-circuit cache read, matching every other
// name-cache guard.
//
// The trigger needs a module load (generation bump) plus the exact
// negative-memory allocation pattern from aura-perf; the fixture under
// tests/suite/issue_5373/ is a verbatim copy of that code. This test runs
// the bare top-level display repro and asserts the line is printed.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

std::string self_dir() {
    char exe[4096] = {0};
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        const std::string self(exe, static_cast<std::size_t>(n));
        const auto slash = self.find_last_of('/');
        if (slash != std::string::npos)
            return self.substr(0, slash);
    }
    return ".";
}

std::string parent_dir(const std::string& d) {
    const auto slash = d.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : d.substr(0, slash);
}

} // namespace

int main() {
    const std::string build = self_dir();
    const std::string aura = build + "/aura";
    const std::string root = parent_dir(build);
    const std::string script = root + "/tests/suite/issue_5373/repro.aura";

    int out[2];
    check(pipe(out) == 0, "pipe");
    const pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        ::close(out[0]);
        dup2(out[1], STDOUT_FILENO);
        dup2(out[1], STDERR_FILENO);
        ::close(out[1]);
        setenv("AURA_SANDBOX", "off", 1);
        setenv("AURA_PATH", root.c_str(), 1);
        execl(aura.c_str(), "aura", script.c_str(), static_cast<char*>(nullptr));
        _exit(24);
    }
    ::close(out[1]);
    std::string output;
    char buf[512];
    ssize_t n;
    while ((n = ::read(out[0], buf, sizeof(buf))) > 0)
        output.append(buf, static_cast<std::size_t>(n));
    ::close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);

    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "repro exits 0");
    // The display line must be present. Under #5373 it was silently dropped.
    check(output.find("(1 0 1 2 2 linear-scan 2 redundant-pass,wrong-scale)") != std::string::npos,
          "display of the inline-computed list is printed (not dropped)");

    if (g_failed == 0)
        std::printf("test_display_cache_5373: all checks passed\n");
    return g_failed == 0 ? 0 : 1;
}
