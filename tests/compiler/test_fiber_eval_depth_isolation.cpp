// @category: unit
// @reason: Issue #2650 / #2649 H11 — eval recursion depth is fiber-local
//          (stackful ucontext fibers share an OS thread; TLS would sum depths).
//
//   AC1: Fiber A and Fiber B have independent eval_c_stack_depth slots
//   AC2: Host (no fiber) uses separate TLS host slot
//   AC3: Source uses aura_eval_c_stack_depth_slot / cites #2650
//   AC4: load_module_file refuses empty / prompt-like / pure-digit paths (#2653)
//   AC5: Shallow recursive eval still works under host path

#include "serve/fiber.h"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::aura_env_lookup_depth_slot;
using aura::serve::aura_eval_c_stack_depth_slot;
using aura::serve::Fiber;
using aura::serve::g_current_fiber;
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

// ── AC1: independent fiber slots ──
static void ac1_fiber_depth_isolated() {
    std::println("\n--- #2650 AC1: Fiber A/B independent depth slots ---");
    Fiber a([] {}, 64 * 1024);
    Fiber b([] {}, 64 * 1024);

    a.eval_c_stack_depth_slot() = 400;
    b.eval_c_stack_depth_slot() = 50;
    CHECK(a.eval_c_stack_depth_slot() == 400, "AC1: A depth 400");
    CHECK(b.eval_c_stack_depth_slot() == 50, "AC1: B depth 50");
    CHECK(a.eval_c_stack_depth_slot() != b.eval_c_stack_depth_slot(),
          "AC1: A and B not the same counter");

    // Slot helper follows g_current_fiber.
    Fiber* prev = g_current_fiber;
    g_current_fiber = &a;
    CHECK(aura_eval_c_stack_depth_slot() == 400, "AC1: slot sees A");
    ++aura_eval_c_stack_depth_slot();
    CHECK(a.eval_c_stack_depth_slot() == 401, "AC1: bump A");
    CHECK(b.eval_c_stack_depth_slot() == 50, "AC1: B unchanged while A current");

    g_current_fiber = &b;
    CHECK(aura_eval_c_stack_depth_slot() == 50, "AC1: slot sees B");
    aura_eval_c_stack_depth_slot() = 10;
    CHECK(b.eval_c_stack_depth_slot() == 10, "AC1: set B");
    CHECK(a.eval_c_stack_depth_slot() == 401, "AC1: A still 401");

    g_current_fiber = prev;
    a.eval_c_stack_depth_slot() = 0;
    b.eval_c_stack_depth_slot() = 0;
}

// ── AC2: host path separate ──
static void ac2_host_slot() {
    std::println("\n--- #2650 AC2: host TLS slot when no fiber ---");
    Fiber* prev = g_current_fiber;
    g_current_fiber = nullptr;
    const auto host0 = aura_eval_c_stack_depth_slot();
    aura_eval_c_stack_depth_slot() = 7;
    CHECK(aura_eval_c_stack_depth_slot() == 7, "AC2: host set");

    Fiber f([] {}, 64 * 1024);
    f.eval_c_stack_depth_slot() = 99;
    g_current_fiber = &f;
    CHECK(aura_eval_c_stack_depth_slot() == 99, "AC2: fiber overrides host");
    g_current_fiber = nullptr;
    CHECK(aura_eval_c_stack_depth_slot() == 7, "AC2: host restored when fiber cleared");
    aura_eval_c_stack_depth_slot() = host0;
    g_current_fiber = prev;
}

// ── AC3: source-cite ──
static void ac3_source_cite() {
    std::println("\n--- #2650 AC3: source cites fiber-local depth ---");
    const auto flat = read_file("src/compiler/evaluator_eval_flat.cpp");
    CHECK(flat.find("#2650") != std::string::npos, "AC3: eval_flat cites #2650");
    CHECK(flat.find("aura_eval_c_stack_depth_slot") != std::string::npos,
          "AC3: uses fiber-local slot");
    CHECK(flat.find("fiber=") != std::string::npos, "AC3: error includes fiber id");

    const auto fiber_h = read_file("src/serve/fiber.h");
    CHECK(fiber_h.find("eval_c_stack_depth_") != std::string::npos, "AC3: Fiber stores depth");
    CHECK(fiber_h.find("aura_eval_c_stack_depth_slot") != std::string::npos, "AC3: free slot API");

    const auto env = read_file("src/compiler/evaluator_env.cpp");
    CHECK(env.find("aura_env_lookup_depth_slot") != std::string::npos,
          "AC3: env lookup fiber-local");
    CHECK(env.find("#2650") != std::string::npos || env.find("#2649") != std::string::npos,
          "AC3: env cites tracker");
}

// ── AC4: load_module_file path refuse ──
static void ac4_module_path_refuse() {
    std::println("\n--- #2653 AC4: refuse non-module paths ---");
    const auto loader = read_file("src/compiler/evaluator_module_loader.cpp");
    CHECK(loader.find("#2653") != std::string::npos, "AC4: cites #2653");
    CHECK(loader.find("is_plausible_module_path") != std::string::npos, "AC4: validator present");
    CHECK(loader.find("refuse empty path") != std::string::npos, "AC4: empty refuse");
    CHECK(loader.find("all_digit") != std::string::npos ||
              loader.find("16384") != std::string::npos,
          "AC4: pure-digit refuse");

    // Live: empty / digit / prompt must not crash; (load-module "") is void/error.
    CompilerService cs;
    auto r1 = cs.eval("(load-module \"\")");
    CHECK(r1.has_value(), "AC4: empty path returns value (void/error, no crash)");
    auto r2 = cs.eval("(load-module \"16384\")");
    CHECK(r2.has_value(), "AC4: digit path no crash");
    auto r3 = cs.eval(
        "(load-module \"You are a denseness propose-edge agent for Aether on Aura Unify\")");
    CHECK(r3.has_value(), "AC4: prompt path no crash");
}

// ── AC5: shallow recursion host path ──
static void ac5_shallow_recursion() {
    std::println("\n--- #2650 AC5: shallow fact works ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (fact n) (if (<= n 1) 1 (* n (fact (- n 1)))))\")")
              .has_value(),
          "AC5: set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "AC5: eval");
    auto r = cs.eval("(fact 10)");
    CHECK(r && is_int(*r) && as_int(*r) == 3628800, "AC5: fact 10 = 3628800");
}

// ── env depth isolation ──
static void ac_env_depth_isolated() {
    std::println("\n--- #2650 env lookup depth fiber isolation ---");
    Fiber a([] {}, 64 * 1024);
    Fiber b([] {}, 64 * 1024);
    Fiber* prev = g_current_fiber;
    g_current_fiber = &a;
    aura_env_lookup_depth_slot() = 100;
    g_current_fiber = &b;
    CHECK(aura_env_lookup_depth_slot() == 0 || aura_env_lookup_depth_slot() != 100,
          "env B independent of A");
    aura_env_lookup_depth_slot() = 0;
    g_current_fiber = &a;
    aura_env_lookup_depth_slot() = 0;
    g_current_fiber = prev;
}

} // namespace

int run_test_fiber_eval_depth_isolation() {
    std::println("=== Issue #2650/#2649: fiber-local eval depth + module path refuse ===");
    ac1_fiber_depth_isolated();
    ac2_host_slot();
    ac3_source_cite();
    ac4_module_path_refuse();
    ac5_shallow_recursion();
    ac_env_depth_isolated();
    std::println("\n=== #2650: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_fiber_eval_depth_isolation();
}
#endif
