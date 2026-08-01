// @category: unit
// @reason: Issue #2478 — command-line requires kCapIoRead (capability
//          bypass closed; /proc/self/cmdline secret leak gated).
//
//   AC1: sandbox + no io-read → capability denied error
//   AC2: sandbox + kCapIoRead (or kCapIo / wildcard) → allowed
//   AC3: sandbox off → allowed (legacy deny_io short-circuit)
//   AC4: source cites #2478 + deny_io(kCapIoRead)
//   AC5: gate wiring

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::kCapIo;
using aura::compiler::security::kCapIoRead;
using aura::compiler::security::kCapWildcard;
using aura::compiler::types::is_error;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_void;
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

// ── AC1: deny without io-read under sandbox ──
static void ac1_denied_without_cap() {
    std::println("\n--- #2478 AC1: sandbox without io-read → denied ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    // Engage legacy deny_io path (sandbox_mode_ true).
    ev.set_effect_sandbox_mode(2); // Strict → sandbox_mode_ on
    CHECK(ev.sandbox_mode(), "AC1: sandbox active");
    // No io-read / io / wildcard grants.
    const auto den0 = ev.capability_denial_count();
    auto r = cs.eval("(command-line)");
    CHECK(r.has_value(), "AC1: eval returns a value");
    CHECK(r && is_error(*r), "AC1: capability denied error");
    CHECK(ev.capability_denial_count() > den0, "AC1: denial counter bumped");
}

// ── AC2: allow with io-read ──
static void ac2_allowed_with_io_read() {
    std::println("\n--- #2478 AC2: sandbox + io-read → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(kCapIoRead);
    const auto den0 = ev.capability_denial_count();
    auto r = cs.eval("(command-line)");
    CHECK(r.has_value(), "AC2: eval returns a value");
    CHECK(r && !is_error(*r), "AC2: not capability-denied error");
    // Result is void (empty cmdline after argv0) or pair list.
    CHECK(r && (is_void(*r) || is_pair(*r)), "AC2: void or pair list");
    CHECK(ev.capability_denial_count() == den0, "AC2: no denial bump");
}

// ── AC2b: kCapIo parent also allows ──
static void ac2b_allowed_with_io() {
    std::println("\n--- #2478 AC2b: sandbox + kCapIo → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(kCapIo);
    auto r = cs.eval("(command-line)");
    CHECK(r.has_value() && !is_error(*r), "AC2b: kCapIo allows");
}

// ── AC2c: wildcard allows ──
static void ac2c_allowed_with_wildcard() {
    std::println("\n--- #2478 AC2c: sandbox + wildcard → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(kCapWildcard);
    auto r = cs.eval("(command-line)");
    CHECK(r.has_value() && !is_error(*r), "AC2c: wildcard allows");
}

// ── AC3: sandbox off → allowed without grant ──
static void ac3_sandbox_off() {
    std::println("\n--- #2478 AC3: sandbox off → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off → sandbox_mode_ false
    CHECK(!ev.sandbox_mode(), "AC3: sandbox off");
    auto r = cs.eval("(command-line)");
    CHECK(r.has_value() && !is_error(*r), "AC3: allowed when sandbox off");
}

// ── AC4: source cite ──
static void ac4_source() {
    std::println("\n--- #2478 AC4: source cites gate ---");
    auto src = read_file("src/compiler/evaluator_primitives_file.cpp");
    CHECK(!src.empty(), "AC4: read evaluator_primitives_file.cpp");
    CHECK(src.find("Issue #2478") != std::string::npos, "AC4: cites #2478");
    auto pos = src.find("add(\"command-line\"");
    CHECK(pos != std::string::npos, "AC4: command-line present");
    if (pos != std::string::npos) {
        auto win = src.substr(pos, 900);
        CHECK(win.find("deny_io") != std::string::npos, "AC4: deny_io in command-line");
        CHECK(win.find("kCapIoRead") != std::string::npos, "AC4: kCapIoRead");
        CHECK(win.find("io-read required") != std::string::npos, "AC4: denial message");
    }
}

// ── AC5: gate ──
static void ac5_gate() {
    std::println("\n--- #2478 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/check_command_line_cap_io_read_2478.py");
    CHECK(build.find("check_command_line_cap_io_read_2478") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_command_line_cap_io_read_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_command_line_cap_io_read_2478") != std::string::npos, "AC5: cmake test");
    CHECK(!script.empty() && script.find("2478") != std::string::npos, "AC5: check script exists");
}

} // namespace

int main() {
    std::println("=== Issue #2478: command-line kCapIoRead capability gate ===");
    ac1_denied_without_cap();
    ac2_allowed_with_io_read();
    ac2b_allowed_with_io();
    ac2c_allowed_with_wildcard();
    ac3_sandbox_off();
    ac4_source();
    ac5_gate();
    std::println("\n=== #2478 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
