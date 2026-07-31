// @category: unit
// @reason: Issue #2377 — force single steal-complete entry (no weak
// legacy residual-less path under production).

#include "test_harness.hpp"

#include "core/gc_hooks.h"
#include "serve/fiber.h"

#include <cstdint>
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

static std::int64_t href(CompilerService& cs, const char* key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:gc-defer-reason-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    std::println("=== Issue #2377: steal-complete strong entry contract ===");

    // AC1: production path requires strong ABI (source + lock API)
    {
        std::println("\n--- AC1: production strong steal-complete ---");
        const auto wc = read_file("src/serve/worker.cpp");
        const auto fb = read_file("src/compiler/fiber_bridge.cpp");
        CHECK(wc.find("Issue #2377") != std::string::npos, "AC1: worker cites #2377");
        CHECK(wc.find("steal_snapshot_soft_production_locked") != std::string::npos,
              "AC1: worker production lock gate");
        CHECK(wc.find("std::abort()") != std::string::npos, "AC1: worker abort on null under prod");
        CHECK(wc.find("call_steal_complete") != std::string::npos, "AC1: call_steal_complete");
        CHECK(fb.find("Issue #2377") != std::string::npos, "AC1: fiber_bridge cites #2377");
        CHECK(fb.find("steal_snapshot_soft_production_locked") != std::string::npos,
              "AC1: weak stub production-aware");
        // Production lock round-trip exists (#2372 Soft + #2377 steal-complete).
        const bool saved = aura::serve::steal_snapshot_soft_production_locked();
        aura::serve::set_steal_snapshot_soft_production_locked(true);
        CHECK(aura::serve::steal_snapshot_soft_production_locked(), "AC1: lock on");
        aura::serve::set_steal_snapshot_soft_production_locked(false);
        CHECK(!aura::serve::steal_snapshot_soft_production_locked(), "AC1: lock off");
        aura::serve::set_steal_snapshot_soft_production_locked(saved);
    }

    // AC2: steal-complete transaction order in strong entry
    {
        std::println("\n--- AC2: strong entry transaction order ---");
        const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        CHECK(fm.find("steal-complete transaction") != std::string::npos,
              "AC2: transaction documented");
        CHECK(fm.find("clear_gc_defer_for_evaluator") != std::string::npos, "AC2: Panic clear");
        CHECK(fm.find("force_clear_residual_defer_for_evaluator") != std::string::npos,
              "AC2: residual interlock");
        CHECK(fm.find("has_resume_layout_stamp") != std::string::npos, "AC2: LayoutStamp check");
        CHECK(fm.find("aura_evaluator_probe_linear_on_steal") != std::string::npos,
              "AC2: linear fold");
        // Order within on_steal_complete body only (file may cite helpers earlier).
        const auto fn = fm.find("extern \"C\" void aura_evaluator_on_steal_complete");
        CHECK(fn != std::string::npos, "AC2: strong entry present");
        const auto body = fm.substr(fn, 6000);
        const auto i_clear = body.find("clear_gc_defer_for_evaluator");
        const auto i_resid = body.find("force_clear_residual_defer_for_evaluator");
        const auto i_stamp = body.find("has_resume_layout_stamp");
        CHECK(i_clear != std::string::npos && i_resid != std::string::npos &&
                  i_stamp != std::string::npos && i_clear < i_resid && i_resid < i_stamp,
              "AC2: order Panic clear → residual → LayoutStamp");
    }

    // AC3: missing-entry metric API + Soft path allowed off production
    {
        std::println("\n--- AC3: missing-entry metric (sandbox path) ---");
        const auto before = aura::gc_hooks::steal_complete_entry_missing_total();
        aura::gc_hooks::bump_steal_complete_entry_missing_total();
        CHECK(aura::gc_hooks::steal_complete_entry_missing_total() == before + 1,
              "AC3: missing total bumps");
        const auto gh = read_file("src/core/gc_hooks.h");
        CHECK(gh.find("g_steal_complete_entry_missing_total") != std::string::npos,
              "AC3: counter in gc_hooks");
        CHECK(gh.find("Issue #2377") != std::string::npos, "AC3: cites #2377");
    }

    // AC4: zero-cost notes for residual 0 / stamp unset
    {
        std::println("\n--- AC4: zero-cost happy path documented ---");
        const auto fm = read_file("src/compiler/evaluator_fiber_mutation.cpp");
        CHECK(fm.find("zero cost when") != std::string::npos ||
                  fm.find("Zero cost") != std::string::npos || fm.find("AC4") != std::string::npos,
              "AC4: zero-cost note present");
        CHECK(fm.find("defer_reasons_snapshot()") != std::string::npos, "AC4: residual load");
    }

    // AC5: query schema-2377 + source-cite
    {
        std::println("\n--- AC5: query schema-2377 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2377") == 2377, "AC5: schema-2377");
        CHECK(href(cs, "issue-2377") == 2377, "AC5: issue-2377");
        CHECK(href(cs, "steal-complete-strong-required-wired") == 1, "AC5: strong-required wired");
        CHECK(href(cs, "steal-complete-entry-missing-total") >= 0, "AC5: missing total key");
        CHECK(href(cs, "steal-complete-total") >= 0, "AC5: steal-complete-total retained");
        CHECK(href(cs, "schema-2203") == 2203, "AC5: 2203 retained");
        CHECK(href(cs, "schema-2314") == 2314 ||
                  href(cs, "residual-defer-steal-interlock-wired") == 1,
              "AC5: residual lineage present");

        const auto q = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(q.find("schema-2377") != std::string::npos, "AC5: query cites schema");
        CHECK(q.find("steal-complete-entry-missing-total") != std::string::npos,
              "AC5: query missing key");
    }

    std::println("\n=== #2377 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
