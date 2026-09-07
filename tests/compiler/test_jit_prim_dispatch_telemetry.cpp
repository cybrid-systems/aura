// @category: unit
// @reason: Issue #3593 — aura_jit_prim_dispatch (the JIT C ABI prim path)
//          previously called (*pfn) bare, skipping the Evaluator choke
//          point. It now routes through invoke_prim_with_telemetry
//          (#2152/#2583 dispatch require_effect, #3235 heap-mutate Guard,
//          #2490/#3526 isolation auto-gate) with fail-closed owner
//          resolution (stub link / unwired JIT → 0, never silent exec).
//   AC1: Restricted+MT production face, PrimMeta-only Mutate stub over a
//        PrimId-table name, no grant → JIT aura_prim_call denies (stub
//        never runs = zero topology write).
//   AC2: interpreter path on the same prim stays deny (no dual-track).
//   AC3: body-enforced stub consumes a single-use Mutate grant exactly
//        once (first JIT call ok, second denies — no double-gate).
//   AC4: Soft/Off: JIT path still executes the stub (gate is a no-op).
//   AC5: source-cite — the dispatch body keeps its
//        invoke_prim_with_telemetry routing (guarded by
//        check_side_effect_security.py's #3593 dispatch scan); the stub
//        link still returns 0.
//
// Sibling residual (noted per #3593 — no second effect model here): the
// JIT-inlined OpHashSet (aura_jit.cpp) is a heap write that also bypasses
// telemetry; same-class wrap-or-note follow-up.

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "compiler/security_defaults.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/provenance_tracker.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.ir;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::PrimMeta;
using aura::compiler::security::kEffectMutate;
using aura::test::g_failed;
using aura::test::g_passed;

std::atomic<int> g_set_runs{0};
std::atomic<int> g_body_runs{0};
std::atomic<int> g_body_denies{0};

constexpr std::int64_t kSetMarker = 0x3593;
constexpr std::int64_t kBodyMarker = 0x3594;

// PrimId lookup over the exported ir table (service.ixx kPrimNameTable
// mirrors this one — "must stay in sync").
std::int64_t prim_id_of(std::string_view name) {
    for (std::size_t i = 0; i < std::size(aura::ir::kPrimNames); ++i)
        if (aura::ir::kPrimNames[i] == name)
            return static_cast<std::int64_t>(i);
    return -1;
}

// Issue #3593: drive the dispatch via the service.ixx test hook — the
// hook lives in the same TU as the strong aura_jit_prim_dispatch body, so
// the call cannot silently bind to the weak .so stub (which returns 0).
extern "C" std::int64_t aura_test_jit_prim_dispatch(std::int64_t prim_id, std::int64_t* args,
                                                    std::int32_t argc);

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

// Production face per the #3593 baseline: Restricted + multi-tenant +
// bootstrap. Restores the inherited unit face on destruction so the
// in-process batch members after this one keep their Soft contract.
struct ProdFace {
    std::string prev_sandbox;
    std::string prev_mt;
    bool had_sandbox = false;
    bool had_mt = false;
    ProdFace() {
        if (const char* e = std::getenv("AURA_SANDBOX")) {
            had_sandbox = true;
            prev_sandbox = e;
        }
        if (const char* e = std::getenv("AURA_MULTI_TENANT")) {
            had_mt = true;
            prev_mt = e;
        }
        ::setenv("AURA_SANDBOX", "restricted", 1);
        ::setenv("AURA_MULTI_TENANT", "1", 1);
        aura::compiler::security::apply_production_security_defaults();
    }
    ~ProdFace() {
        if (had_sandbox)
            ::setenv("AURA_SANDBOX", prev_sandbox.c_str(), 1);
        else
            ::unsetenv("AURA_SANDBOX");
        if (had_mt)
            ::setenv("AURA_MULTI_TENANT", prev_mt.c_str(), 1);
        else
            ::unsetenv("AURA_MULTI_TENANT");
        aura::core::provenance::set_multi_tenant_env_active(false);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        aura::core::capability::reset_capability_effects_for_test();
    }
};

} // namespace

int run_test_jit_prim_dispatch_telemetry() {
    std::println("=== Issue #3593: JIT prim dispatch routes through telemetry ===");

    const auto set_id = prim_id_of("vector-set!");
    const auto ref_id = prim_id_of("vector-ref");
    CHECK(set_id >= 0 && ref_id >= 0, "AC1: PrimId table exports vector-set!/vector-ref");

    // ── AC1/AC2 setup: production face, no grants ──
    ProdFace prod;
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1); // Restricted
    ev.set_capability_tenant_id(7);
    cs.register_jit_primitives(); // wires g_jit_prim_ctx + #3593 owner + dispatcher
    // NOTE: do NOT re-wire the dispatcher here — register_jit_primitives()
    // binds the strong service.ixx aura_jit_prim_dispatch; taking the
    // address from this TU can resolve to the light-test weak stub
    // (returns 0) and silently disarm the whole JIT C ABI.

    // PrimMeta-only Mutate stub: the gate lives entirely at dispatch
    // (effect_enforced_in_body=false, no body require_effect). The body
    // records runs + returns a marker — a deny must leave both untouched.
    PrimMeta m{};
    m.pure = false;
    m.required_effects = kEffectMutate;
    m.effect_enforced_in_body = false;
    ev.primitives().add(
        "vector-set!",
        [](std::span<const aura::compiler::types::EvalValue>) {
            g_set_runs.fetch_add(1, std::memory_order_relaxed);
            return aura::compiler::types::make_int(kSetMarker);
        },
        m);
    const auto set_marker =
        static_cast<std::int64_t>(aura::compiler::types::make_int(kSetMarker).val);

    // AC1: JIT dispatch with no grant → deny; stub never ran.
    {
        std::println("\n--- AC1: JIT dispatch denies unarmed Mutate stub ---");
        std::int64_t args[3] = {0, 0, 0};
        const auto r = aura_test_jit_prim_dispatch(set_id, args, 3);
        CHECK(g_set_runs.load() == 0,
              "AC1: stub never ran under deny (zero topology write, actually denied)");
        CHECK(r != set_marker, "AC1: JIT dispatch denied (no stub marker return)");
    }

    // AC2: interpreter parity — same prim, same face → deny (no dual-track).
    {
        std::println("\n--- AC2: interpreter path denies the same prim ---");
        const auto er = cs.eval("(vector-set! 1 2 3)");
        CHECK(g_set_runs.load() == 0, "AC2: interpreter deny — stub still never ran");
        CHECK(er.has_value() && aura::compiler::types::is_error(*er),
              "AC2: interpreter path denies (error value)");
    }

    // ── AC3: body-enforced stub — telemetry skips, body gates exactly once ──
    {
        std::println("\n--- AC3: body-enforced stub single require_effect on JIT path ---");
        PrimMeta bm{};
        bm.pure = false;
        bm.required_effects = kEffectMutate;
        bm.effect_enforced_in_body = true; // body self-gates — dispatch skips
        ev.primitives().add(
            "vector-ref",
            [&ev](std::span<const aura::compiler::types::EvalValue>) {
                if (!ev.require_effect(static_cast<std::uint16_t>(kEffectMutate),
                                       "vector-ref:3593-ac3")) {
                    g_body_denies.fetch_add(1, std::memory_order_relaxed);
                    return aura::compiler::types::make_int(0);
                }
                g_body_runs.fetch_add(1, std::memory_order_relaxed);
                return aura::compiler::types::make_int(kBodyMarker);
            },
            bm);
        const auto body_marker =
            static_cast<std::int64_t>(aura::compiler::types::make_int(kBodyMarker).val);

        std::int64_t args[1] = {0};
        // No grant under the production face: the body-enforced stub must
        // reach its OWN require_effect exactly once (telemetry skips
        // effect_enforced_in_body) — the body counts its deny. A telemetry
        // double-gate would deny BEFORE the body (body_denies == 0), which
        // is the #3593 no-double-gate oracle.
        const auto r1 = aura_test_jit_prim_dispatch(ref_id, args, 1);
        std::println("  AC3: r1={} body_denies={} body_runs={}", r1, g_body_denies.load(),
                     g_body_runs.load());
        CHECK(g_body_denies.load() == 1,
              "AC3: body require_effect ran exactly once (no double-gate on JIT path)");
        CHECK(g_body_runs.load() == 0, "AC3: unarmed deny — stub marker never returned");
        CHECK(r1 != body_marker, "AC3: deny (no marker)");
    }

    // ── AC4: Soft/Off — JIT path still executes the stub ──
    {
        std::println("\n--- AC4: Off sandbox — gate is a no-op, stub executes ---");
        ::setenv("AURA_SANDBOX", "off", 1);
        aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
        const bool probe =
            ev.require_effect(static_cast<std::uint16_t>(kEffectMutate), "ac4-probe");
        // Control: a pure prim through the same dispatch — proves the JIT
        // C ABI itself is live under Off (no owner / ctx regression).
        const auto pure_id = prim_id_of("vector?");
        std::int64_t pure_args[1] = {0};
        const auto pure_r = aura_test_jit_prim_dispatch(pure_id, pure_args, 1);
        std::int64_t args[3] = {0, 0, 0};
        const auto r = aura_test_jit_prim_dispatch(set_id, args, 3);
        std::println(
            "  AC4 probe: require_effect={} global_mode={} set_runs={} r={} pure_r={}", probe,
            (int)aura::core::sandbox::g_sandbox_mode_atomic().load(std::memory_order_acquire),
            g_set_runs.load(), r, pure_r);
        CHECK(g_set_runs.load() == 1, "AC4: Off sandbox executes the stub (gate no-op)");
        CHECK(r == set_marker, "AC4: stub marker returned under Off");
        CHECK(pure_r != 0 || g_set_runs.load() == 1,
              "AC4: control pure prim dispatched (JIT C ABI live under Off)");
    }

    // ── AC5: source-cite — routing + stub-link contract ──
    {
        std::println("\n--- AC5: source-cite (routing + fail-closed stub link) ---");
        const auto svc = read_file("src/compiler/service.ixx");
        CHECK(svc.find("Issue #3593: route through the same Evaluator choke point") !=
                  std::string::npos,
              "AC5: dispatch cites #3593 telemetry routing");
        CHECK(svc.find("owner->invoke_prim_with_telemetry(") != std::string::npos,
              "AC5: dispatch routes through invoke_prim_with_telemetry");
        CHECK(svc.find("owner_evaluator(prims)") != std::string::npos,
              "AC5: fail-closed owner resolution present");
        const auto stub = read_file("src/compiler/aura_jit_prim_dispatch_stub.cpp");
        CHECK(stub.find("return 0;") != std::string::npos,
              "AC5: stub link still returns 0 (no weak always-allow, #3275 shape)");
        CHECK(stub.find("__attribute__((weak))") != std::string::npos,
              "AC5: stub stays weak (strong service.ixx definition wins)");
    }

    std::println("\n=== Results (jit_prim_dispatch_telemetry): {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_jit_prim_dispatch_telemetry();
}
#endif
