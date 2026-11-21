// @category: unit
// @reason: Issue #5374 — the require path (cache_module) gets the same
//          content-keyed disk IR cache compile_module already uses. A warm
//          require must behave exactly like a cold one; a source change
//          must invalidate; the cache file is keyed by content + build SHA.
//
//   AC1: cold require writes a cache file under XDG_CACHE_HOME/aura/modules
//   AC2: warm require (fresh service, same content) reads it — same behavior
//   AC3: changed source invalidates — new behavior, new cache file
//   AC4: string literals survive the warm path (ir_cache_strings_ remap)
//   AC5: distinct literals in separate defines survive the warm path
//        (the written module merges per-define string pools)

#include "core/runtime_config.hh"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_error;
using aura::compiler::types::is_string;
using aura::test::g_failed;
using aura::test::g_passed;

static bool write_mod(const std::string& dir, const std::string& name, const std::string& body) {
    std::filesystem::create_directories(dir);
    std::ofstream out(dir + "/" + name);
    if (!out)
        return false;
    out << body;
    return true;
}

template <typename F> static std::string with_stdout_capture(F&& fn) {
    int pipefd[2];
    if (::pipe(pipefd) != 0)
        return {};
    int saved = ::dup(STDOUT_FILENO);
    if (saved < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return {};
    }
    ::fflush(stdout);
    if (::dup2(pipefd[1], STDOUT_FILENO) < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        ::close(saved);
        return {};
    }
    ::close(pipefd[1]);
    fn();
    ::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);
    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    return out;
}

static std::size_t count_cache_files(const std::string& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".abfc")
            ++n;
    return n;
}

// One require + call in a fresh service; returns the captured stdout.
static std::string load_and_call(const std::string& mod_dir, const std::string& call) {
    ::aura::config::test_setenv("AURA_PATH", mod_dir.c_str(), 1);
    CompilerService cs;
    std::string out = with_stdout_capture([&] {
        auto req = cs.eval("(require \"diskmod\" all:)");
        if (!req || (is_error(*req) && !is_string(*req)))
            return;
        (void)cs.eval(call);
    });
    ::aura::config::test_unsetenv("AURA_PATH");
    return out;
}

static const char* kBodyV1 = R"((export dm-helper dm-add)
(define (dm-helper msg)
  (display "[disk] ")
  (display msg)
  (newline)
  #t)
(define (dm-add a b) (+ a b))
)";

static const char* kBodyV2 = R"((export dm-helper dm-add)
(define (dm-helper msg)
  (display "[disk-v2] ")
  (display msg)
  (newline)
  #t)
(define (dm-add a b) (+ a b 100))
)";

// Two defines with DISTINCT literals: each bundle's ConstString operands
// index its own lowering pool, so the disk write must merge the pools and
// remap. dm-total also calls dm-add — a cross-define call on the warm path.
static const char* kBodyMulti = R"((export dm-a dm-b dm-total)
(define (dm-a) (display "alpha-1") (newline) 11)
(define (dm-b) (display "beta-2") (newline) 22)
(define (dm-add a b) (+ a b))
(define (dm-total) (dm-add (dm-a) (dm-b)))
)";

static void ac1_cold_writes_cache(const std::string& cache_dir, const std::string& mod_dir) {
    std::println("\n--- #5374 AC1: cold require writes the disk cache ---");
    CHECK(write_mod(mod_dir, "diskmod.aura", kBodyV1), "write diskmod v1");
    const std::string out = load_and_call(mod_dir, "(dm-helper \"cold\")");
    CHECK(out.find("[disk] cold") != std::string::npos, "AC1: cold call correct");
    CHECK(count_cache_files(cache_dir) >= 1, "AC1: cache file written");
}

static void ac2_warm_same_behavior(const std::string& cache_dir, const std::string& mod_dir) {
    std::println("\n--- #5374 AC2: warm require reads the cache, same behavior ---");
    const std::size_t before = count_cache_files(cache_dir);
    const std::string out =
        load_and_call(mod_dir, "(begin (dm-helper \"warm\") (display (dm-add 2 3)) (newline))");
    CHECK(out.find("[disk] warm") != std::string::npos, "AC2: warm string literal correct");
    CHECK(out.find("5") != std::string::npos, "AC2: warm arithmetic correct");
    CHECK(count_cache_files(cache_dir) == before, "AC2: hit does not rewrite the cache");
}

static void ac3_source_change_invalidates(const std::string& cache_dir,
                                          const std::string& mod_dir) {
    std::println("\n--- #5374 AC3: changed source invalidates the cache ---");
    CHECK(write_mod(mod_dir, "diskmod.aura", kBodyV2), "write diskmod v2");
    const std::string out =
        load_and_call(mod_dir, "(begin (dm-helper \"changed\") (display (dm-add 2 3)) (newline))");
    CHECK(out.find("[disk-v2] changed") != std::string::npos, "AC3: new body runs");
    CHECK(out.find("105") != std::string::npos, "AC3: new arithmetic runs");
    CHECK(out.find("[disk] changed") == std::string::npos, "AC3: no stale body");
    CHECK(count_cache_files(cache_dir) >= 2, "AC3: new content, new cache file");
    // back to v1: the original cache entry must still serve the old behavior
    CHECK(write_mod(mod_dir, "diskmod.aura", kBodyV1), "restore diskmod v1");
    const std::string back = load_and_call(mod_dir, "(begin (display (dm-add 2 3)) (newline))");
    CHECK(back.find("5") != std::string::npos, "AC3: v1 cache still valid after v2");
}

static void ac4_warm_literals(const std::string& mod_dir) {
    std::println("\n--- #5374 AC4: warm multi-display literals stay ungarbled ---");
    const std::string out = load_and_call(mod_dir, "(dm-helper \"once\")");
    CHECK(out.find("[disk] once") != std::string::npos, "AC4: prefix + arg");
    CHECK(out.find("onceonce") == std::string::npos, "AC4: no duplicated arg");
}

static void ac5_multi_define_pools(const std::string& mod_dir) {
    std::println("\n--- #5374 AC5: distinct literals across defines stay correct ---");
    CHECK(write_mod(mod_dir, "diskmod.aura", kBodyMulti), "write multi-define module");
    const std::string cold = load_and_call(mod_dir, "(begin (display (dm-total)) (newline))");
    CHECK(cold.find("alpha-1") != std::string::npos, "AC5: cold first literal");
    CHECK(cold.find("beta-2") != std::string::npos, "AC5: cold second literal");
    const std::string warm = load_and_call(mod_dir, "(begin (display (dm-total)) (newline))");
    CHECK(warm.find("alpha-1") != std::string::npos, "AC5: warm first literal ungarbled");
    CHECK(warm.find("beta-2") != std::string::npos, "AC5: warm second literal ungarbled");
    CHECK(warm.find("33") != std::string::npos, "AC5: warm cross-define call correct");
    // Restore the single-literal module so later members see the original shape.
    CHECK(write_mod(mod_dir, "diskmod.aura", kBodyV1), "restore diskmod v1");
}

} // namespace

int run_test_module_disk_cache() {
    std::println("=== Issue #5374: require-path content-keyed disk IR cache ===");
    char cache_tmpl[] = "/tmp/aura_test_modcache5374_XXXXXX";
    char mod_tmpl[] = "/tmp/aura_test_modsrc5374_XXXXXX";
    const char* cache_dir = ::mkdtemp(cache_tmpl);
    const char* mod_dir = ::mkdtemp(mod_tmpl);
    if (!cache_dir || !mod_dir) {
        std::println(stderr, "mkdtemp failed");
        return 1;
    }
    ::aura::config::test_setenv("XDG_CACHE_HOME", cache_dir, 1);
    const std::string modules_dir = std::string(cache_dir) + "/aura/modules";
    ac1_cold_writes_cache(modules_dir, mod_dir);
    ac2_warm_same_behavior(modules_dir, mod_dir);
    ac3_source_change_invalidates(modules_dir, mod_dir);
    ac4_warm_literals(mod_dir);
    ac5_multi_define_pools(mod_dir);
    ::aura::config::test_unsetenv("XDG_CACHE_HOME");
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    std::filesystem::remove_all(mod_dir, ec);
    std::println("\n=== #5374: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_module_disk_cache();
}
#endif
