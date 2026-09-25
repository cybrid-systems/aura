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
#include "compiler/security_side_effect.hh" // #4059: kEffectMutate
#include "compiler/typed_mutation_audit.h"  // #4059: mid join probe
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"  // #4059: set_multi_tenant_env_active
#include "core/security_event.hh"      // #4059: SE ring scan
#include "core/workspace_epoch.hh"     // #4059: Mutation epoch join
#include "core/sandbox.hh"             // #4059: set_mode (registry SOLE writer)
#include "core/workspace_isolation.hh" // #4059: aura_fiber_current_id

#include <cstdlib>
#include <filesystem>
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

static void grant_io_cap(CompilerService& cs, const char* cap) {
    auto& ev = cs.evaluator();
    ev.grant_capability(cap);
    auto prov = aura::core::capability::make_grant_provenance(1, true, 0, 0);
    aura::core::capability::g_capability_registry().grant(
        ev.capability_tenant_id(), cap, aura::core::capability::effect_for_cap_name(cap), prov);
}

static std::size_t ring_cursor_4059() {
    return aura::core::security_event::g_security_event_ring().total.load(
        std::memory_order_relaxed);
}

static std::size_t find_se_row_4059(std::size_t cursor,
                                    aura::core::security_event::SecurityEventKind kind,
                                    std::string_view op_frag, bool denied) {
    const auto& ring = aura::core::security_event::g_security_event_ring();
    const auto seq = ring.seq.load(std::memory_order_relaxed);
    const std::size_t n = std::min<std::size_t>(seq, ring.ring.size());
    const std::size_t begin = std::min<std::size_t>(cursor, n);
    for (std::size_t i = begin; i < n; ++i) {
        const auto& e = ring.ring[i];
        if (e.kind == kind && e.denied == denied &&
            std::string_view(e.op).find(op_frag) != std::string_view::npos)
            return i;
    }
    return std::size_t(-1);
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

// Harmless .aura snippet for load success paths (no side effects).
static const char* kTinyAura = "(define load-cap-ok-2485 1)";

static void write_temp_aura(const char* path) {
    std::ofstream out(path);
    out << kTinyAura << "\n";
}

// ── AC1: deny without io-read under sandbox ──
static void ac1_denied_without_cap() {
    std::println("\n--- #2485 AC1: sandbox without io-read → denied ---");
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    // Grant under Off: the #3409/#3090 grant_locked admin fence refuses
    // high-bits (Mutate) writes while the registry mode is armed — arm the
    // Restricted/Off face AFTER the grants land (the #3720 AC4 shape).
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
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

// ── AC2: io-read alone no longer authorizes the swap ──
// Issue #4059: load still requires io-read for the READ, but the
// workspace swap itself pays Mutate — an io-only grant now effect-denies
// (the allow shape runs in test_dispatch_required_effects.cpp, where the
// #4037-AC2 grant pattern is runtime-proven).
static void ac2_allowed_with_io_read() {
    std::println(
        "\n--- #2485 AC2: sandbox + io-read alone → effect-denied (#4059: swap needs Mutate) ---");
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    ev.set_capability_tenant_id(7);
    grant_io_cap(cs, kCapIoRead);
    write_temp_aura("/tmp/aura_load_cap_2485.aura");
    const auto den0 = ev.capability_denial_count();
    const auto heap0 = ev.string_heap().size();
    auto r = cs.eval("(load \"/tmp/aura_load_cap_2485.aura\")");
    CHECK(r.has_value(), "AC2: eval returns a value");
    CHECK(r && is_error(*r), "AC2: io-only load effect-denied");
    CHECK(ev.capability_denial_count() > den0, "AC2: denial bumped");
    bool mutate_deny = false;
    for (auto i = heap0; i < ev.string_heap().size(); ++i)
        if (ev.string_heap()[i].find("mutate not granted") != std::string::npos)
            mutate_deny = true;
    CHECK(mutate_deny, "AC2: deny names the missing Mutate grant");
}

static void ac2b_allowed_with_io() {
    std::println(
        "\n--- #2485 AC2b: sandbox + kCapIo alone → effect-denied (#4059: swap needs Mutate) ---");
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    ev.set_capability_tenant_id(7);
    grant_io_cap(cs, kCapIo);
    write_temp_aura("/tmp/aura_load_cap_2485.aura");
    auto r = cs.eval("(load \"/tmp/aura_load_cap_2485.aura\")");
    CHECK(r.has_value() && is_error(*r), "AC2b: io-only load effect-denied (#4059)");
}


// AC2c (wildcard allows): runtime coverage moved to the dispatch-file
// #4059 AC3 block — the json batch harness session-promotes high-bits
// grant rows invisible (the #3177/#3409 fence shape), so the wildcard
// allow runs where the other grant-based ACs live.

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
    CHECK(cmake.find("test_load_cap_io_read") != std::string::npos, "AC5: cmake test");
    CHECK(!script.empty() && script.find("2485") != std::string::npos, "AC5: check script exists");
}

// ── Issue #4059: load swaps the workspace — it must run the tenant
// host-path gate read-file / write-file already use (#3802 / #3835) and
// pay Mutate for the install. Deny leaves the workspace pointers
// untouched and reads nothing into the heap. Soft/Off keeps the
// raw-path read with zero extra rows. Each AC resets the process-global
// capability registry + SE ring first — batch members share one process
// and earlier members' grants/rows must not leak into these faces.

static void ac6_4059_host_path_escape_deny() {
    std::println("\n--- #4059 AC1: Restricted+MT out-of-root load → tenant-path-escape deny ---");
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    // Grant under Off: the #3409/#3090 grant_locked admin fence refuses
    // high-bits (Mutate) writes while the registry mode is armed — arm the
    // Restricted/Off face AFTER the grants land (the #3720 AC4 shape).
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    setenv("AURA_TENANT_FS_ROOT", "/tmp/aura_4059_tenant_root", 1);
    std::filesystem::create_directories("/tmp/aura_4059_tenant_root/t-7");
    {
        std::ofstream out("/tmp/aura_4059_tenant_root/t-7/in4059.aura");
        out << "(define *loaded4059* 7)\n";
    }
    {
        std::ofstream out("/tmp/aura_4059_escape4059.aura");
        out << "(define *escape4059* 1)\n";
    }
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    ev.set_capability_tenant_id(7);
    aura::core::provenance::set_multi_tenant_env_active(true);
    grant_io_cap(cs, kCapIoRead); // real io-read grant — the disguise is gone (#4058)
    aura::core::bump_mutation_epoch(1);
    const auto iso_epoch = aura::core::current_mutation_epoch();
    const auto fiber_now = static_cast<std::int64_t>(aura_fiber_current_id());
    const auto* ws0 = ev.workspace_flat();
    const auto heap0 = ev.string_heap().size();
    const auto cursor0 = ring_cursor_4059();
    auto r = cs.eval("(load \"/tmp/aura_4059_escape4059.aura\")");
    CHECK(r.has_value() && !is_error(*r), "4059 AC1: deny is a silent void (SE row audits)");
    CHECK(ev.workspace_flat() == ws0, "4059 AC1: workspace pointer unchanged");
    bool leaked = false;
    for (auto i = heap0; i < ev.string_heap().size(); ++i)
        if (ev.string_heap()[i].find("*escape4059*") != std::string::npos)
            leaked = true;
    CHECK(!leaked, "4059 AC1: out-of-root file content never entered the heap");
    const auto idx = find_se_row_4059(
        cursor0, aura::core::security_event::SecurityEventKind::IsolationDeny, "load", true);
    CHECK(idx != std::size_t(-1), "4059 AC1: IsolationDeny row present");
    if (idx != std::size_t(-1)) {
        const auto& e = aura::core::security_event::g_security_event_ring().ring[idx];
        CHECK(std::string_view(e.reason).find("tenant-path-escape") != std::string_view::npos,
              "4059 AC1: deny reason is tenant-path-escape");
        CHECK(e.epoch == iso_epoch, "4059 AC1: row epoch is the Mutation epoch");
        CHECK(e.fiber_id == fiber_now, "4059 AC1: row carries the fiber id");
    }
    unsetenv("AURA_TENANT_FS_ROOT");
}

static void ac7_4059_no_mutate_effect_deny() {
    std::println(
        "\n--- #4059 AC2: in-root load without Mutate → EffectDeny, workspace unchanged ---");
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    // Grant under Off: the #3409/#3090 grant_locked admin fence refuses
    // high-bits (Mutate) writes while the registry mode is armed — arm the
    // Restricted/Off face AFTER the grants land (the #3720 AC4 shape).
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    setenv("AURA_TENANT_FS_ROOT", "/tmp/aura_4059_tenant_root", 1);
    std::filesystem::create_directories("/tmp/aura_4059_tenant_root/t-7");
    {
        std::ofstream out("/tmp/aura_4059_tenant_root/t-7/in4059.aura");
        out << "(define *loaded4059* 7)\n";
    }
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    ev.set_capability_tenant_id(7);
    aura::core::provenance::set_multi_tenant_env_active(true);
    grant_io_cap(cs, kCapIoRead); // io ok — Mutate is the missing piece
    const auto* ws0 = ev.workspace_flat();
    const auto cursor0 = ring_cursor_4059();
    auto r = cs.eval("(load \"/tmp/aura_4059_tenant_root/t-7/in4059.aura\")");
    CHECK(r.has_value() && is_error(*r), "4059 AC2: load denied (effect-deny error)");
    CHECK(ev.workspace_flat() == ws0, "4059 AC2: workspace pointer unchanged");
    const auto idx = find_se_row_4059(
        cursor0, aura::core::security_event::SecurityEventKind::EffectDeny, "load", true);
    CHECK(idx != std::size_t(-1), "4059 AC2: EffectDeny row present");
    unsetenv("AURA_TENANT_FS_ROOT");
}

static void ac9_4059_soft_off_unchanged() {
    std::println("\n--- #4059 AC4: Soft/Off — raw-path load unchanged, zero deny rows ---");
    aura::core::capability::reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    // Grant under Off: the #3409/#3090 grant_locked admin fence refuses
    // high-bits (Mutate) writes while the registry mode is armed — arm the
    // Restricted/Off face AFTER the grants land (the #3720 AC4 shape).
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(0);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
    CHECK(!ev.sandbox_mode(), "4059 AC4: sandbox off");
    write_temp_aura("/tmp/aura_load_cap_4059_soft.aura");
    auto r = cs.eval("(load \"/tmp/aura_load_cap_4059_soft.aura\")");
    CHECK(r.has_value() && !is_error(*r), "4059 AC4: Soft/Off load still works");
    CHECK(find_se_row_4059(0, aura::core::security_event::SecurityEventKind::EffectDeny, "load",
                           true) == std::size_t(-1) &&
              find_se_row_4059(0, aura::core::security_event::SecurityEventKind::IsolationDeny,
                               "load", true) == std::size_t(-1),
          "4059 AC4: no host-path/effect deny rows on the Soft face");
}

} // namespace

int run_test_load_cap_io_read() {
    std::println("=== Issue #2485: load kCapIoRead capability gate ===");
    ac1_denied_without_cap();
    ac2_allowed_with_io_read();
    ac2b_allowed_with_io();
    ac3_sandbox_off();
    ac4_source();
    ac5_gate();
    ac6_4059_host_path_escape_deny();
    ac7_4059_no_mutate_effect_deny();
    ac9_4059_soft_off_unchanged();
    std::println("\n=== #2485 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_load_cap_io_read();
}
#endif
