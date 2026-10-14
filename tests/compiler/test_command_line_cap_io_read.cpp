// @category: unit
// @reason: Issue #2478 — command-line requires kCapIoRead (capability
//          bypass closed; /proc/self/cmdline secret leak gated).
//
//   AC1: sandbox + no io-read → capability denied error
//   AC2: sandbox + kCapIoRead (or kCapIo / wildcard) → allowed
//   AC3: sandbox off → allowed (legacy deny_io short-circuit)
//   AC4: source cites #2478 + deny_io(kCapIoRead)
//   AC5: gate wiring
//
// Issue #4504 — getenv (and stdin read) fail closed behind the same
// io-read fence in the production face.
//
//   AC6:  Restricted + MT + production defaults, tenant without io-read →
//         (getenv "PATH") is the capability-denied error, counter bumps,
//         value never interned
//   AC7:  same tenant + io-read grant → value returned
//   AC8:  unset name → #f on the allowed path (#4494 preserved)
//   AC9:  Soft/Off → value with no denial (deny_io passthrough)
//   AC10: (command-line) behavior unchanged in the production face
//   AC11: read deny does not consume stdin
//   AC12: source cites #4504 + deny_io(kCapIoRead) in both prims

#include "test_harness.hpp"

#include "compiler/pipeline_policy.hh"
#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh" // #4504 apply_production_security_defaults
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh" // #4504 set_multi_tenant_env_active

#include <cstdint>
#include <cstdlib>
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
using aura::compiler::security::kCapIo;
using aura::compiler::security::kCapIoRead;
using aura::compiler::security::kCapWildcard;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

// #3090: Restricted/Strict refuse mid==0 grants. Stamp bound mid so
// effect-mapped io/io-read/* actually land in the registry.
static void grant_io_cap(CompilerService& cs, const char* cap) {
    auto& ev = cs.evaluator();
    ev.grant_capability(cap);
    auto prov = aura::core::capability::make_grant_provenance(1, true, 0, 0);
    // Issue #4504: same-tenant self-grant (caller = the evaluator's own
    // principal) so the #3409 foreign-tenant TA fence does not refuse the
    // row under Restricted/Strict — mirrors how the evaluator's own
    // grant_effect path passes capability_tenant_id_. Tenant 0 (AC1/AC2)
    // resolves to the same default_tenant caller as before.
    aura::core::capability::g_capability_registry().grant(
        ev.capability_tenant_id(), cap, aura::core::capability::effect_for_cap_name(cap), prov,
        /*single_use=*/false, /*session_bound=*/false,
        /*caller_principal=*/ev.capability_tenant_id());
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

// #4504: getenv is a core boot prim (evaluator_ctor), so cs.eval resolves it
// directly; the capability face is what's under test.
static aura::compiler::EvalResult invoke_getenv(CompilerService& cs, const char* name) {
    return cs.eval(std::string("(getenv \"") + name + "\")");
}

static aura::compiler::EvalResult invoke_read(CompilerService& cs) {
    auto& ev = cs.evaluator();
    auto p = ev.primitives().lookup("read");
    if (p)
        return aura::compiler::EvalResult((*p)({}));
    return cs.eval("(read)");
}

// #4504 Verify face: Restricted + AURA_MULTI_TENANT + production defaults.
static void arm_production_face() {
    aura::core::provenance::set_multi_tenant_env_active(true);
    aura::compiler::security::apply_production_security_defaults();
}

static void disarm_multi_tenant_env() {
    aura::core::provenance::set_multi_tenant_env_active(false);
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

// ── #4504 AC6: production face, tenant without io-read → getenv denied ──
static void ac6_production_getenv_denied() {
    std::println("\n--- #4504 AC6: production face without io-read → getenv denied ---");
    reset_eval_face();
    arm_production_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);       // Restricted
    ev.set_capability_tenant_id(450601); // non-zero tenant, no io-read grant
    CHECK(ev.sandbox_mode(), "AC6: sandbox active");

    const char* path = ::getenv("PATH");
    CHECK(path != nullptr, "AC6: host PATH present");
    const auto den0 = ev.capability_denial_count();
    auto r = invoke_getenv(cs, "PATH");
    CHECK(r.has_value(), "AC6: eval returns a value");
    CHECK(r && is_error(*r), "AC6: capability denied error");
    CHECK(r && !is_string(*r), "AC6: result is not the env value");
    CHECK(ev.capability_denial_count() > den0, "AC6: denial counter bumped");
    if (path) {
        bool interned = false;
        for (const auto& s : ev.string_heap())
            if (s == path)
                interned = true;
        CHECK(!interned, "AC6: PATH value never interned into string_heap_");
    }
    disarm_multi_tenant_env();
}

// ── #4504 AC7: same tenant + io-read grant → value returned ──
static void ac7_production_getenv_allowed_with_grant() {
    std::println("\n--- #4504 AC7: production face + io-read grant → value returned ---");
    reset_eval_face();
    arm_production_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    ev.set_capability_tenant_id(450602);
    grant_io_cap(cs, kCapIoRead);

    const std::string sentinel(72, 'z');
    ::setenv("AURA_4504_SENTINEL", sentinel.c_str(), /*overwrite=*/1);
    const auto den0 = ev.capability_denial_count();
    auto r = invoke_getenv(cs, "AURA_4504_SENTINEL");
    CHECK(r.has_value() && is_string(*r), "AC7: value returned with io-read");
    if (r && is_string(*r)) {
        const auto idx = as_string_idx(*r);
        const auto heap = ev.string_heap();
        CHECK(idx < heap.size() && heap[idx] == sentinel, "AC7: sentinel value matches");
    }
    CHECK(ev.capability_denial_count() == den0, "AC7: no denial bump");
    ::unsetenv("AURA_4504_SENTINEL");
    disarm_multi_tenant_env();
}

// ── #4504 AC8: unset name → #f on the allowed path (#4494) ──
static void ac8_unset_returns_false_on_allowed_path() {
    std::println("\n--- #4504 AC8: unset name → #f on allowed path ---");
    reset_eval_face();
    arm_production_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(450603);
    grant_io_cap(cs, kCapIoRead); // allowed path

    auto r = invoke_getenv(cs, "AURA_4504_DEFINITELY_UNSET");
    CHECK(r.has_value() && is_bool(*r) && !as_bool(*r), "AC8: unset → #f (#4494 preserved)");
    disarm_multi_tenant_env();
}

// ── #4504 AC9: Soft/Off → value with no denial ──
static void ac9_sandbox_off_passthrough() {
    std::println("\n--- #4504 AC9: sandbox Off → value with no denial ---");
    reset_eval_face();
    arm_production_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0); // Off
    CHECK(!ev.sandbox_mode(), "AC9: sandbox off");
    // No grant — deny_io allows before the cap check when sandbox is off.

    const char* path = ::getenv("PATH");
    CHECK(path != nullptr, "AC9: host PATH present");
    const auto den0 = ev.capability_denial_count();
    auto r = invoke_getenv(cs, "PATH");
    CHECK(r.has_value() && is_string(*r), "AC9: value returned with sandbox off");
    if (r && is_string(*r) && path) {
        const auto idx = as_string_idx(*r);
        const auto heap = ev.string_heap();
        CHECK(idx < heap.size() && heap[idx] == path, "AC9: PATH value matches");
    }
    CHECK(ev.capability_denial_count() == den0, "AC9: no denial bump");
    disarm_multi_tenant_env();
}

// ── #4504 AC10: (command-line) behavior unchanged in the production face ──
static void ac10_command_line_unchanged() {
    std::println("\n--- #4504 AC10: command-line behavior unchanged ---");
    reset_eval_face();
    arm_production_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    ev.set_capability_tenant_id(450605);
    install_process_prims(cs);     // BEFORE arming: it drops the effect sandbox to install
    ev.set_effect_sandbox_mode(1); // Restricted

    const auto r = invoke_command_line(cs);
    CHECK(r.has_value() && is_error(*r), "AC10: still denied without io-read (#2478)");

    grant_io_cap(cs, kCapIoRead);
    const auto r2 = invoke_command_line(cs);
    CHECK(r2.has_value() && !is_error(*r2), "AC10: still allowed with io-read");
    disarm_multi_tenant_env();
}

// ── #4504 AC11: read deny does not consume stdin ──
static void ac11_read_deny_preserves_stdin() {
    std::println("\n--- #4504 AC11: read deny leaves stdin unconsumed ---");
    reset_eval_face();
    arm_production_face();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);       // Restricted
    ev.set_capability_tenant_id(450606); // no io-read yet

    int fds[2];
    CHECK(::pipe(fds) == 0, "AC11: stdin pipe");
    const std::string payload = "aura-4504-payload\n";
    CHECK(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()),
          "AC11: payload written");
    ::close(fds[1]);
    const int saved_stdin = ::dup(STDIN_FILENO);
    CHECK(saved_stdin >= 0, "AC11: stdin saved");
    CHECK(::dup2(fds[0], STDIN_FILENO) >= 0, "AC11: stdin redirected");
    ::close(fds[0]);
    std::cin.clear();

    // Deny: no io-read — the fence fires before getline, stdin untouched.
    const auto den0 = ev.capability_denial_count();
    auto rd = invoke_read(cs);
    CHECK(rd.has_value() && is_error(*rd), "AC11: read denied without io-read");
    CHECK(ev.capability_denial_count() > den0, "AC11: denial counter bumped");

    // Grant: the payload must still be there (nothing was consumed).
    grant_io_cap(cs, kCapIoRead);
    auto rd2 = invoke_read(cs);
    CHECK(rd2.has_value() && is_string(*rd2), "AC11: read allowed after grant");
    if (rd2 && is_string(*rd2)) {
        const auto idx = as_string_idx(*rd2);
        const auto heap = ev.string_heap();
        CHECK(idx < heap.size() && heap[idx] == "aura-4504-payload",
              "AC11: denied read did not consume stdin");
    }

    // Restore the real stdin for the rest of the process.
    ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);
    std::cin.clear();
    disarm_multi_tenant_env();
}

// ── #4504 AC12: source cites ──
static void ac12_source_4504() {
    std::println("\n--- #4504 AC12: source cites gate ---");
    auto io_src = read_file("src/compiler/evaluator_primitives_io.cpp");
    CHECK(!io_src.empty(), "AC12: read evaluator_primitives_io.cpp");
    auto pos = io_src.find("add(\"getenv\"");
    CHECK(pos != std::string::npos, "AC12: getenv present");
    if (pos != std::string::npos) {
        auto win = io_src.substr(pos, 900);
        CHECK(win.find("4504") != std::string::npos, "AC12: getenv cites #4504");
        CHECK(win.find("deny_io") != std::string::npos, "AC12: deny_io in getenv");
        CHECK(win.find("kCapIoRead") != std::string::npos, "AC12: kCapIoRead");
        CHECK(win.find("io-read required") != std::string::npos, "AC12: denial message");
        CHECK(win.find("::getenv") != std::string::npos, "AC12: ::getenv after the fence");
    }

    auto file_src = read_file("src/compiler/evaluator_primitives_file.cpp");
    CHECK(!file_src.empty(), "AC12: read evaluator_primitives_file.cpp");
    auto rpos = file_src.find("add(\"read\"");
    CHECK(rpos != std::string::npos, "AC12: read present");
    if (rpos != std::string::npos) {
        auto win = file_src.substr(rpos, 700);
        CHECK(win.find("4504") != std::string::npos, "AC12: read cites #4504");
        CHECK(win.find("deny_io") != std::string::npos, "AC12: deny_io in read");
        CHECK(win.find("kCapIoRead") != std::string::npos, "AC12: read uses kCapIoRead");
        CHECK(win.find("std::getline") != std::string::npos, "AC12: getline after the fence");
    }
}

int run_test_command_line_cap_io_read() {
    std::println("=== Issue #2478: command-line kCapIoRead capability gate ===");
    ac1_denied_without_cap();
    ac2_allowed_with_io_read();
    ac2b_allowed_with_io();
    ac2c_allowed_with_wildcard();
    ac3_sandbox_off();
    ac4_source();
    ac5_gate();
    std::println("\n=== Issue #4504: getenv / read io-read fence (production face) ===");
    ac6_production_getenv_denied();
    ac7_production_getenv_allowed_with_grant();
    ac8_unset_returns_false_on_allowed_path();
    ac9_sandbox_off_passthrough();
    ac10_command_line_unchanged();
    ac11_read_deny_preserves_stdin();
    ac12_source_4504();

    // #4504 teardown: restore the process-global dev/test face for the
    // later members of this batch. Production defaults arm Hard globals
    // (lock-order audit Hard is the #2557 canary that aborted #3915B when
    // left armed). AURA_SANDBOX=off routes apply_production_security_defaults
    // down its dev_off branch: Off sandbox, Sampled/4 audit, lock-order OFF,
    // Soft linear/mutate gates, K=0 retain window. Then clear the registry
    // rows this member granted.
    disarm_multi_tenant_env();
    ::setenv("AURA_SANDBOX", "off", /*overwrite=*/1);
    aura::compiler::security::apply_production_security_defaults();
    ::unsetenv("AURA_SANDBOX");
    ::unsetenv("AURA_MULTI_TENANT");
    aura::core::capability::reset_capability_effects_for_test();

    std::println("\n=== #2478 + #4504 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_command_line_cap_io_read();
}
#endif
