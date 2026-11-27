// @category: unit
// @reason: IR compile-time unbound globals stayed ConstI64 0. A program that
//          binds the name with a runtime require inside a function (the live
//          game loads lib/coins in load-coins!) then died with "cannot call:
//          coin-blank". The IR now lowers an unresolved global to top-ref and
//          lowers a require inside a function body to the import primitive.
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_float;
using aura::compiler::types::is_float;
using aura::test::g_failed;
using aura::test::g_passed;

static void ac1_runtime_require_binds_late_global() {
    // The function body requires std/math at run time; sqrt is unbound when
    // the form is compiled.
    CompilerService cs;
    auto r = cs.eval_ir(R"(
        (define (f) (begin (require "std/math") (sqrt 9.0)))
        (f))");
    CHECK(r && is_float(*r) && as_float(*r) == 3.0,
          "AC1: a runtime require binds a compile-time unbound global");
}

static void ac2_plain_global_still_resolves() {
    CompilerService cs;
    auto r = cs.eval_ir(R"((define x 41) (+ x 1))");
    CHECK(r && aura::compiler::types::is_int(*r) && aura::compiler::types::as_int(*r) == 42,
          "AC2: a bound global still lowers normally");
}

static std::string aura_binary_5484() {
    if (const char* e = std::getenv("AURA_BIN"); e != nullptr && ::access(e, X_OK) == 0)
        return e;
    for (const char* c : {"./build/aura", "../build/aura", "../../build/aura"}) {
        if (::access(c, X_OK) == 0)
            return c;
    }
    return {};
}

static bool write_text_5484(const std::filesystem::path& path, const std::string& body) {
    std::ofstream out(path);
    out << body;
    return static_cast<bool>(out);
}

struct CliResult5484 {
    int rc = -1;
    std::string out;
};

static CliResult5484 run_cli_5484(const std::string& cmd, const std::filesystem::path& out_path) {
    CliResult5484 r;
    const int raw = std::system(cmd.c_str());
    r.rc = (raw != -1 && WIFEXITED(raw)) ? WEXITSTATUS(raw) : raw;
    std::ifstream in(out_path);
    r.out.assign(std::istreambuf_iterator<char>(in), {});
    return r;
}

static void ac3_warm_disk_cache_keeps_cross_call() {
    // A module's cached bundle is the define plus the callees it inlined.
    // The warm (disk-cache) require used to load only the entry function, so
    // f's call to g resolved to __top__ on the second run. Cold was green.
    const auto bin = aura_binary_5484();
    if (bin.empty()) {
        std::println("SKIP AC3: no aura binary (set AURA_BIN)");
        return;
    }
    const auto dir = std::filesystem::temp_directory_path() /
                     ("aura-5484-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "lib", ec);
    CHECK(!ec, "AC3: temp workspace");
    CHECK(write_text_5484(dir / "lib" / "m.aura", "(define (g x) (+ x 1))\n"
                                                  "(define (f x) (g x))\n"
                                                  "(export f g)\n"),
          "AC3: write module");
    CHECK(write_text_5484(dir / "m.aura", "(require \"lib/m\")\n"
                                          "(display (f 1))\n"
                                          "(newline)\n"),
          "AC3: write main");
    const auto cache = dir / "cache";
    const auto out_path = dir / "out.txt";
    std::filesystem::create_directories(cache, ec);

    const std::string cmd = "cd '" + dir.string() + "' && XDG_CACHE_HOME='" + cache.string() +
                            "' AURA_SANDBOX=off '" + bin + "' --ir m.aura >'" + out_path.string() +
                            "' 2>&1";
    const auto cold = run_cli_5484(cmd, out_path);
    CHECK(cold.rc == 0 && cold.out.find('2') != std::string::npos,
          "AC3: cold --ir require resolves the sibling call");
    const auto warm = run_cli_5484(cmd, out_path);
    CHECK(warm.rc == 0 && warm.out.find('2') != std::string::npos,
          "AC3: warm --ir require keeps the whole bundle");
    if (!(warm.rc == 0))
        std::println("AC3 warm stderr/stdout: {}", warm.out);
    std::filesystem::remove_all(dir, ec);
}

static void ac4_ir_map_callback_reads_captures() {
    // The primitive map/for-each bridge binds free vars into a tree-walker
    // Env. IR cell captures are ids in the interpreter's cell_heap_; binding
    // the raw id made the callback read void. The bridge must resolve the
    // cell value first.
    CompilerService cs;
    auto global = cs.eval_ir(R"(
        (define n 5)
        (car (map (lambda (x) (+ x n)) (list 1 2))))");
    CHECK(global && aura::compiler::types::is_int(*global) &&
              aura::compiler::types::as_int(*global) == 6,
          "AC4: map callback reads a top-level define");

    auto local = cs.eval_ir(R"(
        (let ((k 10))
          (car (map (lambda (x) (+ x k)) (list 1 2)))))");
    CHECK(local && aura::compiler::types::is_int(*local) &&
              aura::compiler::types::as_int(*local) == 11,
          "AC4: map callback reads a let capture");
}

} // namespace

int run_test_ir_runtime_require() {
    // The require path needs the dev capability set in the test process.
    ::setenv("AURA_SANDBOX", "off", 1);
    std::println("=== IR runtime require + late-bound global ===");
    ac1_runtime_require_binds_late_global();
    ac2_plain_global_still_resolves();
    ac3_warm_disk_cache_keeps_cross_call();
    ac4_ir_map_callback_reads_captures();
    std::println("\n=== IR runtime require: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_ir_runtime_require();
}
#endif
