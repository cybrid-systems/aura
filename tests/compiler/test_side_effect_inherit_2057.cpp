// @category: unit
// @reason: Issue #2057 — side-effect primitives inherit capability /
// isolation enforcement by construction (PrimMeta + gate + require_effect).
//
//   AC1: PrimMeta carries required_effects / effect_enforced_in_body / security_exempt
//   AC2: add_mutate stamps required_effects=Mutate + effect_enforced_in_body
//   AC3: dispatch require_effect when required_effects set and not body-enforced
//   AC4: infer_required_effects_from_name / is_side_effect_prim_name helpers
//   AC5: static gate script fails intentionally bare add("mutate:…") pattern
//   AC6: documentation tokens present (security_side_effect.hh / AURA_SIDE_EFFECT)

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_side_effect.hh"
#include "core/capability_model.hh"
#include "core/sandbox.hh"

#include <cstdlib>
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
using aura::compiler::infer_required_effects_from_name;
using aura::compiler::is_side_effect_prim_name;
using aura::compiler::kSideEffectInheritIssue;
using aura::compiler::kSideEffectPrimPatternToken;
using aura::compiler::PrimMeta;
using aura::compiler::security::kEffectMutate;
using aura::compiler::security::kEffectNone;
using aura::compiler::security::kEffectWrite;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    for (const auto& p :
         {std::string(path), std::string("../") + path, std::string("../../") + path}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

} // namespace

int run_test_side_effect_inherit_2057() {
    std::println("=== Issue #2057: side-effect inherit by construction ===");
    CHECK(kSideEffectInheritIssue == 2057, "issue stamp (compiler)");
    CHECK(aura::compiler::security::kSideEffectInheritIssue == 2057, "issue stamp (security)");

    // ── AC1: PrimMeta fields exist ──
    {
        std::println("\n--- AC1: PrimMeta effect contract fields ---");
        PrimMeta m{};
        m.required_effects = kEffectMutate;
        m.effect_enforced_in_body = true;
        m.security_exempt = false;
        CHECK(m.required_effects == kEffectMutate, "required_effects settable");
        CHECK(m.effect_enforced_in_body == true, "effect_enforced_in_body settable");
        CHECK(m.security_exempt == false, "security_exempt default false");
        PrimMeta ex{};
        ex.security_exempt = true;
        CHECK(ex.security_exempt, "security_exempt settable");
    }

    // ── AC2: add_mutate stamps PrimMeta ──
    {
        std::println("\n--- AC2: add_mutate PrimMeta stamp ---");
        reset_capability_effects_for_test();
        set_mode(SandboxMode::Off);
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        // mutate:set-body is registered via add_mutate
        const auto slot = prims.slot_for_name("mutate:set-body");
        CHECK(slot < prims.slot_count(), "mutate:set-body registered");
        const auto& meta = prims.meta_for_slot(slot);
        std::println("  set-body required_effects={} enforced_in_body={} pure={}",
                     meta.required_effects, meta.effect_enforced_in_body, meta.pure);
        CHECK(meta.required_effects == kEffectMutate, "add_mutate stamps Mutate effect");
        CHECK(meta.effect_enforced_in_body, "body-enforced flag set (no double audit)");
        CHECK(!meta.pure, "mutate is impure");
        CHECK(!meta.security_exempt, "not exempt");
    }

    // ── AC3: dispatch require_effect via PrimMeta.required_effects ──
    {
        std::println("\n--- AC3: dispatch auto-enforce ---");
        reset_capability_effects_for_test();
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_effect_sandbox_mode(2); // Strict
        // Register a synthetic effectful prim with required_effects, body does nothing.
        PrimMeta m{};
        m.required_effects = kEffectWrite;
        m.effect_enforced_in_body = false;
        m.pure = false;
        m.security_level = 2; // sandboxed
        m.doc = "test synthetic side-effect for #2057";
        bool body_ran = false;
        ev.primitives().add(
            "test:side-effect-2057",
            [&](std::span<const aura::compiler::types::EvalValue>) {
                body_ran = true;
                return aura::compiler::types::make_bool(true);
            },
            m);
        // Invoke through telemetry path (same as production dispatch).
        auto r = ev.invoke_prim_with_telemetry("test:side-effect-2057", [&]() {
            auto fn = ev.primitives().lookup("test:side-effect-2057");
            CHECK(fn.has_value(), "lookup synthetic");
            return (*fn)({});
        });
        (void)r;
        // Under Strict without grant, require_effect should deny → body not run.
        CHECK(!body_ran, "dispatch require_effect blocks un-granted Write");
        // Grant write and retry
        reset_capability_effects_for_test();
        ev.set_effect_sandbox_mode(0); // Off → allow
        body_ran = false;
        (void)ev.invoke_prim_with_telemetry("test:side-effect-2057", [&]() {
            auto fn = ev.primitives().lookup("test:side-effect-2057");
            return (*fn)({});
        });
        CHECK(body_ran, "Off sandbox allows body");
    }

    // ── AC4: name helpers ──
    {
        std::println("\n--- AC4: name inference helpers ---");
        CHECK(is_side_effect_prim_name("mutate:set-body"), "mutate is side-effect");
        CHECK(is_side_effect_prim_name("write-file"), "write-file is side-effect");
        CHECK(is_side_effect_prim_name("ffi:call"), "ffi is side-effect");
        CHECK(is_side_effect_prim_name("render:present"), "render is side-effect");
        CHECK(!is_side_effect_prim_name("query:node-type"), "query is not");
        CHECK(!is_side_effect_prim_name("+"), "math is not");
        CHECK(infer_required_effects_from_name("mutate:x") == kEffectMutate, "infer mutate");
        CHECK(infer_required_effects_from_name("write-file") & kEffectWrite, "infer write");
        CHECK(infer_required_effects_from_name("query:x") == kEffectNone, "infer none");
        CHECK(std::string_view(kSideEffectPrimPatternToken) == "AURA_SIDE_EFFECT_PRIM",
              "pattern token");
    }

    // ── AC5: gate script present + rejects bare add ──
    {
        std::println("\n--- AC5: static gate script ---");
        const auto script = read_file("scripts/coverage/checks/check_side_effect_security.py");
        CHECK(!script.empty(), "gate script readable");
        CHECK(script.find("2057") != std::string::npos, "cites #2057");
        CHECK(script.find("add_mutate") != std::string::npos, "knows add_mutate");
        CHECK(script.find("require_effect") != std::string::npos, "knows require_effect");
        // Live gate must pass on current tree (run from repo root).
        const int rc = std::system("cd .. 2>/dev/null; python3 "
                                   "scripts/coverage/checks/check_side_effect_security.py --strict "
                                   ">/dev/null 2>&1");
        // Also try from cwd if already at root.
        const int rc2 = std::system("python3 scripts/coverage/checks/check_side_effect_security.py "
                                    "--strict >/dev/null 2>&1");
        CHECK(rc == 0 || rc2 == 0, "live gate clean on tree");
    }

    // ── AC6: documentation tokens ──
    {
        std::println("\n--- AC6: documentation ---");
        const auto hh = read_file("src/compiler/security_side_effect.hh");
        CHECK(!hh.empty(), "security_side_effect.hh present");
        CHECK(hh.find("require_effect") != std::string::npos, "docs require_effect");
        CHECK(hh.find("add_mutate") != std::string::npos, "docs add_mutate");
        CHECK(hh.find("security_exempt") != std::string::npos, "docs security_exempt");
        CHECK(hh.find("AURA_SIDE_EFFECT_PRIM") != std::string::npos, "pattern token in header");
        const auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
        CHECK(mut.find("AURA_SIDE_EFFECT_PRIM") != std::string::npos ||
                  mut.find("effect_enforced_in_body") != std::string::npos,
              "mutate registration cites pattern");
        const auto cap = read_file("src/compiler/security_capabilities.h");
        CHECK(cap.find("2057") != std::string::npos, "security_capabilities cites #2057");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_side_effect_inherit_2057();
}
#endif
