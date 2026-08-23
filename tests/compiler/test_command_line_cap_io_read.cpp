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

#include "compiler/pipeline_policy.hh"
#include "compiler/security_capabilities.h"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"

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

// #3090: Restricted/Strict refuse mid==0 grants. Stamp bound mid so
// effect-mapped io/io-read/* actually land in the registry.
static void grant_io_cap(CompilerService& cs, const char* cap) {
    auto& ev = cs.evaluator();
    ev.grant_capability(cap);
    auto prov = aura::core::capability::make_grant_provenance(1, true, 0, 0);
    aura::core::capability::g_capability_registry().grant(
        ev.capability_tenant_id(), cap, aura::core::capability::effect_for_cap_name(cap), prov);
}

// #3174: command-line is a std/process host prim, not core boot.
// ensure_std_host_prims("std/process") requires kEffectExec when any
// sandbox is already armed (CompilerService ctor may start Restricted).
// Drop the effect sandbox for the install, then each AC re-arms.
static void install_process_prims(CompilerService& cs) {
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    (void)ev.ensure_std_host_prims("std/process");
}

// Deferred host prims are not in the typecheck env / IR prim table, so
// cs.eval("(command-line)") can return UnboundVariable Diagnostic even
// when the prim is registered. Invoke the deny_io body directly.
static aura::compiler::EvalResult invoke_command_line(CompilerService& cs) {
    auto& ev = cs.evaluator();
    auto p = ev.primitives().lookup("command-line");
    if (p)
        return aura::compiler::EvalResult((*p)({}));
    return cs.eval("(command-line)");
}

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
static void reset_eval_face() {
    aura::compiler::reset_tree_walker_fallback_policy_for_test();
    aura::compiler::typed_audit::apply_dev_audit_defaults();
}

static void ac1_denied_without_cap() {
    std::println("\n--- #2478 AC1: sandbox without io-read → denied ---");
    reset_eval_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    install_process_prims(cs);
    // Engage legacy deny_io path (sandbox_mode_ true).
    ev.set_effect_sandbox_mode(2); // Strict → sandbox_mode_ on
    CHECK(ev.sandbox_mode(), "AC1: sandbox active");
    // No io-read / io / wildcard grants.
    const auto den0 = ev.capability_denial_count();
    auto r = invoke_command_line(cs);
    CHECK(r.has_value(), "AC1: eval returns a value");
    CHECK(r && is_error(*r), "AC1: capability denied error");
    CHECK(ev.capability_denial_count() > den0, "AC1: denial counter bumped");
}

// ── AC2: allow with io-read ──
static void ac2_allowed_with_io_read() {
    std::println("\n--- #2478 AC2: sandbox + io-read → allowed ---");
    reset_eval_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    install_process_prims(cs);
    ev.set_effect_sandbox_mode(2);
    grant_io_cap(cs, kCapIoRead);
    const auto den0 = ev.capability_denial_count();
    auto r = invoke_command_line(cs);
    CHECK(r.has_value(), "AC2: eval returns a value");
    CHECK(r && !is_error(*r), "AC2: not capability-denied error");
    // Result is void (empty cmdline after argv0) or pair list.
    CHECK(r && (is_void(*r) || is_pair(*r)), "AC2: void or pair list");
    CHECK(ev.capability_denial_count() == den0, "AC2: no denial bump");
}

// ── AC2b: kCapIo parent also allows ──
static void ac2b_allowed_with_io() {
    std::println("\n--- #2478 AC2b: sandbox + kCapIo → allowed ---");
    reset_eval_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    install_process_prims(cs);
    ev.set_effect_sandbox_mode(2);
    grant_io_cap(cs, kCapIo);
    auto r = invoke_command_line(cs);
    CHECK(r.has_value() && !is_error(*r), "AC2b: kCapIo allows");
}

// ── AC2c: wildcard allows ──
static void ac2c_allowed_with_wildcard() {
    std::println("\n--- #2478 AC2c: sandbox + wildcard → allowed ---");
    reset_eval_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    install_process_prims(cs);
    ev.set_effect_sandbox_mode(2);
    grant_io_cap(cs, kCapWildcard);
    auto r = invoke_command_line(cs);
    CHECK(r.has_value() && !is_error(*r), "AC2c: wildcard allows");
}

// ── AC3: sandbox off → allowed without grant ──
static void ac3_sandbox_off() {
    std::println("\n--- #2478 AC3: sandbox off → allowed ---");
    reset_eval_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    install_process_prims(cs);
    ev.set_effect_sandbox_mode(0); // Off → sandbox_mode_ false
    CHECK(!ev.sandbox_mode(), "AC3: sandbox off");
    auto r = invoke_command_line(cs);
    CHECK(r.has_value() && !is_error(*r), "AC3: allowed when sandbox off");
}

// ── AC4: source cite ──
static void ac4_source() {
    std::println("\n--- #2478 AC4: source cites gate ---");
    auto src = read_file("src/compiler/evaluator_primitives_file.cpp");
    CHECK(!src.empty(), "AC4: read evaluator_primitives_file.cpp");
    CHECK(src.find("Issue #2478") != std::string::npos, "AC4: cites #2478");
    auto pos = src.find("defer_std_host_prim(\"command-line\"");
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
    auto script = read_file("scripts/coverage/checks/check_command_line_cap_io_read_2478.py");
    CHECK(build.find("check_command_line_cap_io_read_2478") != std::string::npos,
          "AC5: check script in build.py");
    CHECK(build.find("cmd_command_line_cap_io_read_coverage") != std::string::npos,
          "AC5: coverage cmd");
    CHECK(cmake.find("test_command_line_cap_io_read") != std::string::npos, "AC5: cmake test");
    CHECK(!script.empty() && script.find("2478") != std::string::npos, "AC5: check script exists");
}

} // namespace

int run_test_command_line_cap_io_read() {
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

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_command_line_cap_io_read();
}
#endif
