// @category: unit
// @reason: Issue #2656 — CLI denseness fiber:spawn returns positive id
//          (not -1); spawn+join payload works under thread fallback.
//
//   AC1: fiber:spawn returns positive int (never -1 / #f on success)
//   AC2: fiber:join returns payload 1
//   AC3: fiber:spawn-backend is thread (2) under CompilerService (CLI)
//   AC4: two concurrent spawns both join correctly
//   AC5: source cites #2656 + docs/stdlib/fiber-spawn.md

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
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

// ── AC1: positive id ──
static void ac1_positive_id() {
    std::println("\n--- #2656 AC1: fiber:spawn positive id ---");
    CompilerService cs;
    auto fid = cs.eval("(fiber:spawn (lambda () 1))");
    CHECK(fid && is_int(*fid), "AC1: spawn returns int");
    if (fid && is_int(*fid)) {
        CHECK(as_int(*fid) > 0, "AC1: id > 0 (not -1)");
        CHECK(as_int(*fid) != -1, "AC1: id is not -1");
    }
}

// ── AC2: join payload ──
static void ac2_join_payload() {
    std::println("\n--- #2656 AC2: spawn+join payload 1 ---");
    CompilerService cs;
    auto r = cs.eval("(fiber:join (fiber:spawn (lambda () 1)))");
    CHECK(r && is_int(*r) && as_int(*r) == 1, "AC2: join returns 1");
    auto r42 = cs.eval("(let ((f (fiber:spawn (lambda () 42)))) (fiber:join f))");
    CHECK(r42 && is_int(*r42) && as_int(*r42) == 42, "AC2: join returns 42");
}

// ── AC3: backend ──
static void ac3_backend_thread() {
    std::println("\n--- #2656 AC3: CLI backend is thread (2) ---");
    CompilerService cs;
    // No serve-async scheduler → thread fallback.
    auto b = cs.eval("(fiber:spawn-backend)");
    CHECK(b && is_int(*b) && as_int(*b) == 2, "AC3: spawn-backend = 2 (thread)");
}

// ── AC4: concurrent ──
static void ac4_two_workers() {
    std::println("\n--- #2656 AC4: two concurrent spawns ---");
    CompilerService cs;
    auto r = cs.eval(R"(
(let ((a (fiber:spawn (lambda () 10)))
      (b (fiber:spawn (lambda () 20))))
  (+ (fiber:join a) (fiber:join b))))");
    CHECK(r && is_int(*r) && as_int(*r) == 30, "AC4: 10+20=30");
}

// ── AC5: source + docs ──
static void ac5_source() {
    std::println("\n--- #2656 AC5: source-cite + docs ---");
    const auto msg = read_file("src/compiler/evaluator_primitives_messaging.cpp");
    CHECK(msg.find("#2656") != std::string::npos, "AC5: messaging cites #2656");
    CHECK(msg.find("0x4000") != std::string::npos || msg.find("positive") != std::string::npos,
          "AC5: positive thread-fallback ids");
    CHECK(msg.find("fiber:spawn-backend") != std::string::npos, "AC5: spawn-backend prim");
    const auto doc = read_file("docs/stdlib/fiber-spawn.md");
    CHECK(doc.find("#2656") != std::string::npos, "AC5: denseness contract doc");
    CHECK(doc.find("thread") != std::string::npos, "AC5: doc mentions thread backend");
    const auto cmake = read_file("CMakeLists.txt");
    CHECK(cmake.find("test_fiber_spawn_cli") != std::string::npos, "AC5: cmake");
    const auto build = read_file("build.py");
    CHECK(build.find("check_fiber_spawn_cli_2656") != std::string::npos, "AC5: coverage");
}

} // namespace

int run_test_fiber_spawn_cli() {
    std::println("=== Issue #2656: CLI denseness fiber:spawn ===");
    ac1_positive_id();
    ac2_join_payload();
    ac3_backend_thread();
    ac4_two_workers();
    ac5_source();
    std::println("\n=== #2656: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_fiber_spawn_cli();
}
#endif
