// @category: unit
// @reason: Issue #2534 — query:security-posture + security-correlated-trail.
//
//   AC1: posture returns schema-2534
//   AC2: after EffectDeny, correlated-trail sees se/cap hits for mid
//   AC3: mid=0 empty
//   AC4: Soft callable
//   AC5: security-health still works
//   AC6: source-cite

#include "test_harness.hpp"
#include "core/capability_model.hh"
#include "core/security_event.hh"
#include "core/workspace_epoch.hh"
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::capability::Effect;
using aura::core::capability::EffectProvenance;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::reset_capability_effects_for_test;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r =
        cs.eval(std::format("(hash-ref (engine:metrics \"query:security-posture\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}
std::int64_t trail(CompilerService& cs, std::uint64_t mid, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:security-correlated-trail\" {}) \"{}\")", mid, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}
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

int main() {
    std::println("=== Issue #2534: security posture + correlated trail ===");
    reset_capability_effects_for_test();
    aura::core::security_event::reset_security_event_ring_for_test();
    CompilerService cs;
    auto& ev = cs.evaluator();
    ev.set_effect_sandbox_mode(1);

    CHECK(href(cs, "schema-2534") == 2534, "AC1: schema");
    CHECK(href(cs, "security-posture-wired") == 1, "AC1: wired");
    CHECK(href(cs, "cap-audit-ring-size") == 1024, "AC1: cap ring size");
    CHECK(href(cs, "iso-audit-ring-size") == 1024, "AC1: iso ring size");

    CHECK(trail(cs, 0, "match-count") == 0, "AC3: mid=0 empty");

    // EffectDeny with mid
    EffectProvenance prov;
    prov.mutation_id = 4242;
    prov.epoch = 4242;
    g_capability_registry().sandbox_mode = EffectSandboxMode::Restricted;
    g_capability_registry().record_audit(Effect::Mutate, Effect::None, 1, prov, true, "deny-test");

    CHECK(trail(cs, 4242, "capability-hits") >= 1, "AC2: cap audit hit");
    CHECK(trail(cs, 4242, "se-hits") >= 1, "AC2: SE hit");

    auto r = cs.eval("(hash-ref (engine:metrics \"query:security-health\") \"schema-2389\")");
    CHECK(r && is_int(*r) && as_int(*r) == 2389, "AC5: health preserved");

    auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(sec.find("query:security-posture") != std::string::npos, "AC6: posture");
    CHECK(sec.find("query:security-correlated-trail") != std::string::npos, "AC6: trail");
    CHECK(sec.find("2534") != std::string::npos, "AC6: cite");

    std::println("\n=== #2534: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
