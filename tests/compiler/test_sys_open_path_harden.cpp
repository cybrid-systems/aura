// @category: unit
// @reason: Issue #2487 — sys-open O_NOFOLLOW + path_is_denied (TOCTOU
//          symlink + /proc/self/mem disclosure closed).
//
//   AC1: (sys-open "/proc/self/mem") → -1 (path_is_denied)
//   AC2: open via symlink → -1 (O_NOFOLLOW, no follow)
//   AC3: (sys-open "/dev/null") still succeeds (≥ 0)
//   AC4: caller flags ignored (source fixed O_RDONLY|O_NOFOLLOW|O_CLOEXEC)
//   AC5: shared path_is_denied helper + source cites #2487
//   AC6: gate wiring (CMake + this test)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unistd.h>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::kSysOpenPathHardenIssue;
using aura::compiler::security::path_is_denied;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

// ── AC1: sensitive path denied ──
static void ac1_proc_self_mem() {
    std::println("\n--- #2487 AC1: /proc/self/mem denied ---");
    CHECK(path_is_denied("/proc/self/mem"), "AC1: helper denies /proc/self/mem");
    CHECK(path_is_denied("/dev/mem"), "AC1: helper denies /dev/mem");
    CHECK(path_is_denied("/proc/kcore"), "AC1: helper denies /proc/kcore");
    CHECK(path_is_denied("/proc/1234/mem"), "AC1: helper denies /proc/*/mem");
    CHECK(!path_is_denied("/dev/null"), "AC1: /dev/null not denied");
    CHECK(!path_is_denied("/tmp/x"), "AC1: normal path not denied");

    CompilerService cs;
    auto r = cs.eval(R"((sys-open "/proc/self/mem"))");
    CHECK(r && is_int(*r) && as_int(*r) == -1, "AC1: sys-open /proc/self/mem → -1");
    auto r2 = cs.eval(R"((sys-open "/dev/mem"))");
    CHECK(r2 && is_int(*r2) && as_int(*r2) == -1, "AC1: sys-open /dev/mem → -1");
    auto r3 = cs.eval(R"((sys-open "/proc/kcore"))");
    CHECK(r3 && is_int(*r3) && as_int(*r3) == -1, "AC1: sys-open /proc/kcore → -1");
}

// ── AC2: symlink not followed ──
static void ac2_symlink_nofollow() {
    std::println("\n--- #2487 AC2: symlink open fails (O_NOFOLLOW) ---");
    char dir_tmpl[] = "/tmp/aura_sys_open_2487_XXXXXX";
    char* dir = ::mkdtemp(dir_tmpl);
    CHECK(dir != nullptr, "AC2: mkdtemp");
    if (!dir)
        return;
    const std::string target = std::string(dir) + "/target.txt";
    const std::string link = std::string(dir) + "/link.txt";
    {
        std::ofstream out(target);
        out << "secret";
    }
    ::unlink(link.c_str());
    CHECK(::symlink(target.c_str(), link.c_str()) == 0, "AC2: symlink created");

    // Direct open of target is fine.
    CompilerService cs;
    auto ok = cs.eval(std::format(R"((sys-open "{}"))", target));
    CHECK(ok && is_int(*ok) && as_int(*ok) >= 0, "AC2: open real file ok");
    if (ok && is_int(*ok) && as_int(*ok) >= 0)
        ::close(static_cast<int>(as_int(*ok)));

    // Symlink must not be followed → -1 (ELOOP).
    auto bad = cs.eval(std::format(R"((sys-open "{}"))", link));
    CHECK(bad && is_int(*bad) && as_int(*bad) == -1, "AC2: open symlink → -1");

    // Sanity: raw open with O_NOFOLLOW also fails.
    int fd = ::open(link.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(fd < 0 && errno == ELOOP, "AC2: kernel O_NOFOLLOW → ELOOP");
    if (fd >= 0)
        ::close(fd);

    ::unlink(link.c_str());
    ::unlink(target.c_str());
    ::rmdir(dir);
}

// ── AC3: /dev/null still works ──
static void ac3_dev_null() {
    std::println("\n--- #2487 AC3: /dev/null still opens ---");
    CompilerService cs;
    auto r = cs.eval(R"((sys-open "/dev/null" 0))");
    CHECK(r && is_int(*r) && as_int(*r) >= 0, "AC3: sys-open /dev/null ≥ 0");
    if (r && is_int(*r) && as_int(*r) >= 0)
        ::close(static_cast<int>(as_int(*r)));
}

// ── AC4: flags ignored (source contract) ──
static void ac4_flags_ignored_source() {
    std::println("\n--- #2487 AC4: fixed flags in source ---");
    auto src = read_file("src/compiler/evaluator_primitives_io.cpp");
    CHECK(!src.empty(), "AC4: read io.cpp");
    auto pos = src.find("add(\"sys-open\"");
    CHECK(pos != std::string::npos, "AC4: sys-open found");
    auto body = src.substr(pos, 900);
    CHECK(body.find("O_NOFOLLOW") != std::string::npos, "AC4: O_NOFOLLOW");
    CHECK(body.find("O_CLOEXEC") != std::string::npos, "AC4: O_CLOEXEC");
    CHECK(body.find("O_RDONLY") != std::string::npos, "AC4: O_RDONLY");
    CHECK(body.find("path_is_denied") != std::string::npos, "AC4: path_is_denied");
    CHECK(body.find("Issue #2487") != std::string::npos || body.find("#2487") != std::string::npos,
          "AC4: cites #2487");
    // Must not pass caller-controlled flags into ::open (no static_cast flags).
    CHECK(body.find("static_cast<int>(as_int(a[1]))") == std::string::npos,
          "AC4: no caller flags cast");
    CHECK(body.find("0644") == std::string::npos, "AC4: no world-readable 0644 mode");
}

// ── AC5: shared helper stamp ──
static void ac5_shared_helper() {
    std::println("\n--- #2487 AC5: shared path_is_denied + stamp ---");
    CHECK(kSysOpenPathHardenIssue == 2487, "AC5: issue stamp");
    auto hdr = read_file("src/compiler/security_capabilities.h");
    CHECK(!hdr.empty(), "AC5: read security_capabilities.h");
    CHECK(hdr.find("path_is_denied") != std::string::npos, "AC5: helper in header");
    CHECK(hdr.find("kSysOpenPathHardenIssue") != std::string::npos, "AC5: stamp constant");

    auto file_src = read_file("src/compiler/evaluator_primitives_file.cpp");
    CHECK(file_src.find("security::path_is_denied") != std::string::npos ||
              file_src.find("aura::compiler::security::path_is_denied") != std::string::npos,
          "AC5: file.cpp uses shared helper");
}

// ── AC6: gate ──
static void ac6_gate() {
    std::println("\n--- #2487 AC6: CMake wiring ---");
    auto cm = read_file("CMakeLists.txt");
    CHECK(cm.find("test_sys_open_path_harden") != std::string::npos, "AC6: CMake target");
}

} // namespace

int run_test_sys_open_path_harden() {
    std::println("=== Issue #2487: sys-open path harden ===");
    ac1_proc_self_mem();
    ac2_symlink_nofollow();
    ac3_dev_null();
    ac4_flags_ignored_source();
    ac5_shared_helper();
    ac6_gate();
    std::println("\n=== #2487 summary: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_sys_open_path_harden();
}
#endif
