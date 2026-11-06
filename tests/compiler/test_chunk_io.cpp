// @category: unit
// @reason: Issue #4936 — std::array file chunks and the self-exe readlink.
//
//   AC1: append_fd matches a file larger than one chunk, including 0 and 0xFF
//   AC2: an empty file stays empty
//   AC3: copy_fd reproduces those bytes
//   AC4: resolve_self_executable reads /proc/self/exe into a std::array

#include "compiler/chunk_io.hh"
#include "compiler/runtime_paths.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <string_view>

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
