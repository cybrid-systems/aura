// @category: unit
// @reason: Issue #4936 — std::array file chunks and the self-exe readlink.
//
//   AC1: append_fd matches a file larger than one chunk, including 0 and 0xFF
//   AC2: an empty file stays empty
//   AC3: copy_fd reproduces those bytes
//   AC4: resolve_self_executable reads /proc/self/exe into a std::array

#include "compiler/chunk_io.hh"
#include "compiler/generic_error_words.hh"
#include "compiler/runtime_paths.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace {

int g_failed = 0;

void expect(bool ok, const char* label) {
    if (!ok) {
        std::println(std::cerr, "FAIL {}", label);
        ++g_failed;
    } else {
        std::println("PASS {}", label);
    }
}

std::string payload() {
    std::string s;
    s.reserve(5000);
    for (int i = 0; i < 5000; ++i)
        s.push_back(static_cast<char>(i == 0 ? 0 : (i == 4095 ? '\xff' : (i % 251))));
    return s;
}

} // namespace

int main() {
    const auto body = payload();
    char in_name[] = "/tmp/aura-chunk-in-XXXXXX";
    char out_name[] = "/tmp/aura-chunk-out-XXXXXX";
    const int in_fd = ::mkstemp(in_name);
    const int out_fd = ::mkstemp(out_name);
    expect(in_fd >= 0 && out_fd >= 0, "temp files");
    expect(::write(in_fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()),
           "write payload");
    ::lseek(in_fd, 0, SEEK_SET);
    std::string got;
    expect(aura::compiler::io::append_fd(in_fd, got) && got == body,
           "append_fd matches 5000 bytes across a 4096 chunk");

    char empty_name[] = "/tmp/aura-chunk-empty-XXXXXX";
    const int empty_fd = ::mkstemp(empty_name);
    std::string empty;
    expect(empty_fd >= 0 && aura::compiler::io::append_fd(empty_fd, empty) && empty.empty(),
           "empty file");

    ::lseek(in_fd, 0, SEEK_SET);
    expect(aura::compiler::io::copy_fd(in_fd, out_fd) == 0, "copy_fd");
    ::lseek(out_fd, 0, SEEK_SET);
    std::string copied;
    expect(aura::compiler::io::append_fd(out_fd, copied) && copied == body, "copy_fd bytes match");

    const auto self = aura::compiler::paths::resolve_self_executable();
    expect(!self.empty() && self.front() == '/', "self executable is an absolute readlink");
    // Issue #5087: the release bundle's /proc/self/exe is rt/ld.so. The
    // launcher exports AURA_SELF=bin/aura, and that is the re-exec path.
    {
        const char* prev = std::getenv("AURA_SELF");
        const std::string saved = prev ? prev : std::string();
        ::setenv("AURA_SELF", "/tmp/aura-self-missing", 1);
        const auto ignored = aura::compiler::paths::resolve_self_executable();
        expect(ignored != "/tmp/aura-self-missing",
               "5087: a non-executable AURA_SELF does not replace /proc/self/exe");
        ::setenv("AURA_SELF", self.c_str(), 1);
        const auto via = aura::compiler::paths::resolve_self_executable();
        expect(via == self, "5087: an executable AURA_SELF is the re-exec path");
        if (saved.empty())
            ::unsetenv("AURA_SELF");
        else
            ::setenv("AURA_SELF", saved.c_str(), 1);
    }

    std::string kept = "KEEP";
    expect(!aura::compiler::io::append_fd(-1, kept) && kept == "KEEP",
           "EBADF keeps the prefix already in out");
    ::lseek(in_fd, 0, SEEK_SET);
    expect(aura::compiler::io::append_fd(in_fd, kept) && kept == "KEEP" + body,
           "a later successful read appends after that prefix");
    expect(!aura::compiler::io::append_fd(-1, kept) && kept == "KEEP" + body,
           "a failed read after a success still keeps the bytes already appended");

    expect(aura::compiler::io::copy_fd(-1, out_fd) == -1, "copy_fd read error");
    ::lseek(in_fd, 0, SEEK_SET);
    const int full = ::open("/dev/full", O_WRONLY);
    expect(full >= 0 && aura::compiler::io::copy_fd(in_fd, full) == -1, "copy_fd write error");
    if (full >= 0)
        ::close(full);

    {
        int pipefd[2] = {-1, -1};
        expect(::pipe(pipefd) == 0, "short-write pipe");
        std::string drained;
        std::thread reader([&] { (void)aura::compiler::io::append_fd(pipefd[0], drained); });
        ::lseek(in_fd, 0, SEEK_SET);
        // 5000 bytes is above PIPE_BUF, so the kernel may return a short write.
        // The assertion is the bytes, not a count of short writes.
        expect(aura::compiler::io::copy_fd(in_fd, pipefd[1]) == 0, "copy_fd through a pipe");
        ::close(pipefd[1]);
        reader.join();
        ::close(pipefd[0]);
        expect(drained == body,
               "pipe copy matches, including a short write if the kernel split it");
    }

    {
        struct sigaction action{};
        struct sigaction previous{};
        action.sa_handler = [](int) {};
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0; // no SA_RESTART
        sigaction(SIGALRM, &action, &previous);
        int pipefd[2] = {-1, -1};
        expect(::pipe(pipefd) == 0, "EINTR pipe");
        std::string eintr_out = "P:";
        std::thread writer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            const char msg[] = "EINTR";
            (void)::write(pipefd[1], msg, sizeof(msg) - 1);
            ::close(pipefd[1]);
        });
        ::alarm(1);
        const bool eintr_ok = aura::compiler::io::append_fd(pipefd[0], eintr_out);
        writer.join();
        ::alarm(0);
        sigaction(SIGALRM, &previous, nullptr);
        ::close(pipefd[0]);
        // alarm(1) is later than the writer, so this pass does not prove the
        // EINTR branch ran. A blocking read that is signalled without
        // SA_RESTART is covered below with a 10ms timer.
        expect(eintr_ok && eintr_out == "P:EINTR", "blocking read still returns the payload");
    }

    {
        struct sigaction action{};
        struct sigaction previous{};
        static volatile sig_atomic_t ticks = 0;
        action.sa_handler = [](int) { ticks = 1; };
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        sigaction(SIGALRM, &action, &previous);
        itimerval timer{};
        timer.it_value.tv_usec = 10000;
        timer.it_interval.tv_usec = 10000;
        setitimer(ITIMER_REAL, &timer, nullptr);
        int pipefd[2] = {-1, -1};
        expect(::pipe(pipefd) == 0, "EINTR timer pipe");
        std::string eintr_out = "Q:";
        std::thread writer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const char msg[] = "RETRY";
            (void)::write(pipefd[1], msg, sizeof(msg) - 1);
            ::close(pipefd[1]);
        });
        const bool ok = aura::compiler::io::append_fd(pipefd[0], eintr_out);
        writer.join();
        itimerval stop{};
        setitimer(ITIMER_REAL, &stop, nullptr);
        sigaction(SIGALRM, &previous, nullptr);
        ::close(pipefd[0]);
        expect(ticks > 0, "SIGALRM fired during the blocking read");
        expect(ok && eintr_out == "Q:RETRY", "EINTR retry keeps the prefix and the payload");
    }

    expect(aura::compiler::is_generic_error_word("error") &&
               aura::compiler::is_generic_error_word("type") &&
               aura::compiler::is_generic_error_word("parse") &&
               aura::compiler::is_generic_error_word("syntax") &&
               aura::compiler::is_generic_error_word("kind"),
           "generic error words");
    expect(!aura::compiler::is_generic_error_word("error!") &&
               !aura::compiler::is_generic_error_word("Error") &&
               !aura::compiler::is_generic_error_word(""),
           "generic error words are exact");

    ::close(in_fd);
    ::close(out_fd);
    ::close(empty_fd);
    ::unlink(in_name);
    ::unlink(out_name);
    ::unlink(empty_name);

    if (g_failed != 0)
        std::println(std::cerr, "{} failed", g_failed);
    return g_failed == 0 ? 0 : 1;
}
