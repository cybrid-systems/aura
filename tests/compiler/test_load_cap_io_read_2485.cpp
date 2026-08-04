// @category: unit
// @reason: Issue #2485 — load requires kCapIoRead (capability bypass
//          closed; arbitrary file read gated like read-file).
//
//   AC1: sandbox + no io-read → capability denied error
//   AC2: sandbox + kCapIoRead (or kCapIo / wildcard) → allowed
//   AC3: sandbox off → allowed
//   AC4: source cites #2485 + kCapIoRead + path deny
//   AC5: gate wiring

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"

#include <fstream>
#include <print>
#include <string>

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

// Harmless .aura snippet for load success paths (no side effects).
static const char* kTinyAura = "(define load-cap-ok-2485 1)";

static void write_temp_aura(const char* path) {
    std::ofstream out(path);
    out << kTinyAura << "\n";
}

// ── AC1: deny without io-read under sandbox ──
static void ac1_denied_without_cap() {
    std::println("\n--- #2485 AC1: sandbox without io-read → denied ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2); // Strict → sandbox_mode_ on
    CHECK(ev.sandbox_mode(), "AC1: sandbox active");
    write_temp_aura("/tmp/aura_load_cap_2485.aura");
    const auto den0 = ev.capability_denial_count();
    auto r = cs.eval("(load \"/tmp/aura_load_cap_2485.aura\")");
    CHECK(r.has_value(), "AC1: eval returns a value");
    CHECK(r && is_error(*r), "AC1: capability denied error");
    CHECK(ev.capability_denial_count() > den0, "AC1: denial counter bumped");
}

// ── AC2: allow with io-read ──
static void ac2_allowed_with_io_read() {
    std::println("\n--- #2485 AC2: sandbox + io-read → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(kCapIoRead);
    write_temp_aura("/tmp/aura_load_cap_2485.aura");
    const auto den0 = ev.capability_denial_count();
    auto r = cs.eval("(load \"/tmp/aura_load_cap_2485.aura\")");
    CHECK(r.has_value(), "AC2: eval returns a value");
    CHECK(r && !is_error(*r), "AC2: not capability-denied error");
    CHECK(ev.capability_denial_count() == den0, "AC2: no denial bump");
}

static void ac2b_allowed_with_io() {
    std::println("\n--- #2485 AC2b: sandbox + kCapIo → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(kCapIo);
    write_temp_aura("/tmp/aura_load_cap_2485.aura");
    auto r = cs.eval("(load \"/tmp/aura_load_cap_2485.aura\")");
    CHECK(r.has_value() && !is_error(*r), "AC2b: kCapIo allows");
}

static void ac2c_allowed_with_wildcard() {
    std::println("\n--- #2485 AC2c: sandbox + wildcard → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(2);
    ev.grant_capability(kCapWildcard);
    write_temp_aura("/tmp/aura_load_cap_2485.aura");
    auto r = cs.eval("(load \"/tmp/aura_load_cap_2485.aura\")");
    CHECK(r.has_value() && !is_error(*r), "AC2c: wildcard allows");
}

// ── AC3: sandbox off → allowed without grant ──
static void ac3_sandbox_off() {
    std::println("\n--- #2485 AC3: sandbox off → allowed ---");
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    CHECK(!ev.sandbox_mode(), "AC3: sandbox off");
    write_temp_aura("/tmp/aura_load_cap_2485.aura");
    auto r = cs.eval("(load \"/tmp/aura_load_cap_2485.aura\")");
    CHECK(r.has_value() && !is_error(*r), "AC3: allowed when sandbox off");
}

// ── AC4: source ──
static void ac4_source() {
    std::println("\n--- #2485 AC4: source cites gate ---");
    auto src = read_file("src/compiler/evaluator_primitives_eval.cpp");
    CHECK(!src.empty(), "AC4: read evaluator_primitives_eval.cpp");
    CHECK(src.find("Issue #2485") != std::string::npos, "AC4: cites #2485");
    auto pos = src.find("add(\"load\"");
    CHECK(pos != std::string::npos, "AC4: load present");
    if (pos != std::string::npos) {
        auto win = src.substr(pos, 1800);
        CHECK(win.find("kCapIoRead") != std::string::npos, "AC4: kCapIoRead");
        CHECK(win.find("io-read required for load") != std::string::npos, "AC4: denial message");
        CHECK(win.find("sandbox_mode") != std::string::npos, "AC4: sandbox_mode gate");
        CHECK(win.find("/proc/self/mem") != std::string::npos, "AC4: path deny list");
    }
}

// ── AC5: gate ──
static void ac5_gate() {
    std::println("\n--- #2485 AC5: test + gate wiring ---");
    auto build = read_file("build.py");
    auto cmake = read_file("CMakeLists.txt");
    auto script = read_file("scripts/coverage/checks/check_load_cap_io_read_2485.py");
    CHECK(build.find("check_load_cap_io_read_2485") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_load_cap_io_read_coverage") != std::string::npos, "AC5: coverage cmd");
    CHECK(cmake.find("test_load_cap_io_read_2485") != std::string::npos, "AC5: cmake test");
    CHECK(!script.empty() && script.find("2485") != std::string::npos, "AC5: check script exists");
}

} // namespace

int run_test_load_cap_io_read_2485() {
    std::println("=== Issue #2485: load kCapIoRead capability gate ===");
    ac1_denied_without_cap();
    ac2_allowed_with_io_read();
    ac2b_allowed_with_io();
    ac2c_allowed_with_wildcard();
    ac3_sandbox_off();
    ac4_source();
    ac5_gate();
    std::println("\n=== #2485 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_load_cap_io_read_2485();
}
#endif
