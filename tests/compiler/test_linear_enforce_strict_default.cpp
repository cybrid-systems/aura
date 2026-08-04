// @category: unit
// @reason: Issue #2207 — LinearEnforceMode Strict production process
// default + harden provenance hot path (refine #2103 / #2182).
//
//   AC1: Default mode is Strict; Soft only via set_linear_enforce_mode(Soft)
//        or env (AURA_LINEAR_ENFORCE=soft / AURA_SANDBOX=off).
//   AC2: Under Strict, incomplete provenance on validate (Move/Borrow/Drop
//        dual-path contract) hard-fails — no silent continue.
//   AC3: Soft-mode still works when forced Soft; Soft incomplete continues.
//   AC4: Metrics linear_provenance_hard_fail_total + linear_enforce_mode
//        on query:post-steal-closed-loop-stats / linear-ownership-enforcement
//        with schema-2207.
//   AC5: Composite commit hard-block path unchanged (#2108).

#include "test_harness.hpp"

#include "compiler/security_defaults.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::security::apply_production_security_defaults;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::provenance::g_linear_soft_incomplete_continue_total;
using aura::core::provenance::g_linear_strict_hard_fail_total;
using aura::core::provenance::kLinearOwned;
using aura::core::provenance::linear_enforce_mode;
using aura::core::provenance::linear_enforce_require_complete;
using aura::core::provenance::linear_provenance_hard_fail_total_atomic;
using aura::core::provenance::LinearEnforceMode;
using aura::core::provenance::reset_linear_enforce_mode_for_test;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::restore_linear_enforce_production_default_for_test;
using aura::core::provenance::set_linear_enforce_mode;
using aura::core::provenance::validate_linear_provenance;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
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

static void clear_env(const char* k) {
#if defined(_WIN32)
    _putenv_s(k, "");
#else
    unsetenv(k);
#endif
}

static void set_env(const char* k, const char* v) {
#if defined(_WIN32)
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

static std::int64_t href_ps(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:post-steal-closed-loop-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_enf(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:linear-ownership-enforcement-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// AC1: process default Strict; Soft only via explicit set / env
static void ac1_default_strict() {
    std::println("\n--- AC1: process default Strict; Soft explicit only ---");
    restore_linear_enforce_production_default_for_test();
    CHECK(linear_enforce_mode() == LinearEnforceMode::Strict, "AC1: process default Strict");
    CHECK(linear_enforce_require_complete(), "AC1: require_complete under Strict");

    // Explicit Soft opt-in.
    set_linear_enforce_mode(LinearEnforceMode::Soft);
    CHECK(linear_enforce_mode() == LinearEnforceMode::Soft, "AC1: Soft via set");
    CHECK(!linear_enforce_require_complete(), "AC1: Soft require_complete false");

    // Restore Strict then AURA_LINEAR_ENFORCE=soft via production defaults.
    restore_linear_enforce_production_default_for_test();
    clear_env("AURA_SANDBOX");
    clear_env("AURA_LINEAR_ENFORCE");
    set_mode(SandboxMode::Restricted);
    set_env("AURA_LINEAR_ENFORCE", "soft");
    apply_production_security_defaults();
    CHECK(linear_enforce_mode() == LinearEnforceMode::Soft, "AC1: Soft via env");
    clear_env("AURA_LINEAR_ENFORCE");

    // Production defaults without Soft env → Strict.
    set_mode(SandboxMode::Restricted);
    apply_production_security_defaults();
    CHECK(linear_enforce_mode() == LinearEnforceMode::Strict, "AC1: prod defaults Strict");

    auto pt = read_file("src/core/provenance_tracker.hh");
    CHECK(pt.find("LinearEnforceMode::Strict") != std::string::npos, "static Strict default cite");
    CHECK(pt.find("#2207") != std::string::npos, "tracker cites #2207");
}

// AC2: Strict incomplete hard-fail (Move/Borrow/Drop dual-path contract)
static void ac2_strict_hard_fail() {
    std::println("\n--- AC2: Strict incomplete → hard fail (no silent continue) ---");
    reset_provenance_enforcement_for_test();
    restore_linear_enforce_production_default_for_test();
    CHECK(linear_enforce_require_complete(), "Strict require");

    const auto hard0 = linear_provenance_hard_fail_total_atomic().load();
    const auto hard_alias0 = g_linear_strict_hard_fail_total.load();
    CHECK(hard0 == hard_alias0, "AC2: hard_fail alias same counter");

    auto r = validate_linear_provenance(kLinearOwned, 0, /*prov=*/0, /*mut=*/0, 0, 0, 0, 0,
                                        linear_enforce_require_complete());
    CHECK(!r.ok, "AC2: incomplete → !ok");
    CHECK(r.force_deopt, "AC2: force_deopt");
    CHECK(r.reason != nullptr, "AC2: reason set");
    CHECK(linear_provenance_hard_fail_total_atomic().load() > hard0, "AC2: hard_fail total");
    CHECK(g_linear_strict_hard_fail_total.load() > hard_alias0, "AC2: strict hard-fail alias");

    // Complete trail still succeeds.
    auto r2 = validate_linear_provenance(kLinearOwned, 1, 42, 99, 0, 0, 0, 0,
                                         linear_enforce_require_complete());
    CHECK(r2.ok, "AC2: complete trail ok under Strict");

    auto ir = read_file("src/compiler/ir_executor_impl.cpp");
    CHECK(ir.find("linear_enforce_require_complete") != std::string::npos, "IR enforces mode");
    CHECK(ir.find("#2207") != std::string::npos || ir.find("Issue #2207") != std::string::npos,
          "IR cites #2207");
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    CHECK(gc.find("linear_enforce_require_complete") != std::string::npos,
          "dual-path / GC uses mode");
}

// AC3: Soft forced still metric-only continue
static void ac3_soft_forced() {
    std::println("\n--- AC3: Soft forced → incomplete continues ---");
    reset_provenance_enforcement_for_test();
    set_linear_enforce_mode(LinearEnforceMode::Soft);
    CHECK(!linear_enforce_require_complete(), "Soft forced");

    const auto soft0 = g_linear_soft_incomplete_continue_total.load();
    const auto hard0 = linear_provenance_hard_fail_total_atomic().load();
    auto r = validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0,
                                        /*require_complete=*/false);
    CHECK(r.ok, "AC3: Soft incomplete continues");
    CHECK(!r.force_deopt, "AC3: no force_deopt under Soft");
    CHECK(g_linear_soft_incomplete_continue_total.load() > soft0, "AC3: soft continue metric");
    CHECK(linear_provenance_hard_fail_total_atomic().load() == hard0,
          "AC3: no hard-fail under Soft");
}

// AC4: query surface schema-2207 + hard_fail keys
static void ac4_query_schema() {
    std::println("\n--- AC4: query schema-2207 + hard_fail keys ---");
    restore_linear_enforce_production_default_for_test();
    CompilerService cs;
    CHECK(href_ps(cs, "schema-2207") == 2207, "post-steal schema-2207");
    CHECK(href_ps(cs, "issue-2207") == 2207, "post-steal issue-2207");
    CHECK(href_ps(cs, "linear-enforce-mode") == 1, "post-steal mode Strict");
    CHECK(href_ps(cs, "linear_enforce_mode") == 1, "underscore mode key");
    CHECK(href_ps(cs, "linear-provenance-hard-fail-total") >= 0, "hard-fail dash key");
    CHECK(href_ps(cs, "linear_provenance_hard_fail_total") >= 0, "hard-fail underscore key");
    CHECK(href_ps(cs, "linear-enforce-strict-default") == 1, "strict-default flag");
    CHECK(href_ps(cs, "schema-2103") == 2103, "2103 lineage retained");
    CHECK(href_ps(cs, "schema-2182") == 2182, "2182 lineage retained");

    CHECK(href_enf(cs, "schema-2207") == 2207, "enforcement schema-2207");
    CHECK(href_enf(cs, "linear-provenance-hard-fail-total") >= 0, "enforcement hard-fail");
    CHECK(href_enf(cs, "linear_enforce_mode") == 1, "enforcement mode Strict");

    // Agent-visible: after Strict incomplete, hard_fail advances on query.
    const auto hard0 = href_ps(cs, "linear-provenance-hard-fail-total");
    (void)validate_linear_provenance(kLinearOwned, 0, 0, 0, 0, 0, 0, 0, true);
    CHECK(href_ps(cs, "linear-provenance-hard-fail-total") > hard0, "query hard_fail advanced");
}

// AC5: composite commit hard-block path still present (#2108)
static void ac5_composite_unchanged() {
    std::println("\n--- AC5: composite commit hard-block path unchanged ---");
    auto esc = read_file("src/compiler/evaluator_typecheck.cpp");
    auto test2108 = read_file("tests/compiler/test_linear_escape_commit_hardblock.cpp");
    // Soft-fail if paths moved; source-cite the hard-block contract.
    const bool has_escape = esc.find("cross_batch") != std::string::npos ||
                            esc.find("linear_escape") != std::string::npos ||
                            esc.find("#2108") != std::string::npos;
    const bool has_test = !test2108.empty() && test2108.find("#2108") != std::string::npos;
    CHECK(has_escape || has_test, "AC5: #2108 composite hard-block retained");
    CHECK(has_test, "AC5: #2108 regression test still present");

    auto pt = read_file("src/core/provenance_tracker.hh");
    CHECK(pt.find("restore_linear_enforce_production_default_for_test") != std::string::npos,
          "restore helper for Strict default");
}

} // namespace

int run_test_linear_enforce_strict_default() {
    std::println("=== Issue #2207: LinearEnforceMode Strict process default ===");
    ac1_default_strict();
    ac2_strict_hard_fail();
    ac3_soft_forced();
    ac4_query_schema();
    ac5_composite_unchanged();
    // Leave Soft for subsequent consumers that expect harness Soft opt-in.
    reset_linear_enforce_mode_for_test();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_linear_enforce_strict_default();
}
#endif
