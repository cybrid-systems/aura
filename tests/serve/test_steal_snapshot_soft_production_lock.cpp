// @category: unit
// @reason: Issue #2372 — production hard-forbid AURA_STEAL_SNAPSHOT_SOFT
// + require force-deopt ABI under production Soft lock.

#include "test_harness.hpp"
#include "serve/fiber.h"
#include "compiler/security_defaults.hh"

#include <cstdint>
#include <cstdlib>
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:orchestration-steal-outermost-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_steal_snapshot_soft_production_lock() {
    std::println("=== Issue #2372: Soft production lock + force-deopt ABI ===");

    // Save / restore process Soft lock + test override around the suite.
    const bool saved_lock = aura::serve::steal_snapshot_soft_production_locked();
    aura::serve::reset_steal_snapshot_soft_for_test();

    // AC1: production lock + Soft env → Soft ignored
    {
        std::println("\n--- AC1: production lock ignores Soft env ---");
        ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "1", 1);
        aura::serve::reset_steal_snapshot_soft_for_test();
        aura::serve::set_steal_snapshot_soft_production_locked(true);
        CHECK(aura::serve::steal_snapshot_soft_production_locked(), "AC1: lock on");
        CHECK(!aura::serve::is_steal_snapshot_soft_mode(),
              "AC1: Soft env ignored under production lock");
        // Unlock → Soft env takes effect again (sandbox=off path).
        aura::serve::set_steal_snapshot_soft_production_locked(false);
        CHECK(aura::serve::is_steal_snapshot_soft_mode(), "AC1: Soft env active when unlocked");
        ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
        aura::serve::set_steal_snapshot_soft_production_locked(saved_lock);
    }

    // AC2: production + null force-deopt path documented in worker/fiber_bridge
    // (source-cite: abort on missing/weak ABI under lock). Runtime abort is
    // not exercised here (would kill the test process); wire presence is AC5.
    {
        std::println("\n--- AC2: production ABI fail-closed (source wire) ---");
        const auto wc = read_file("src/serve/worker.cpp");
        const auto fb = read_file("src/compiler/fiber_bridge.cpp");
        CHECK(wc.find("steal_snapshot_soft_production_locked") != std::string::npos,
              "AC2: worker checks production lock on null ABI");
        CHECK(wc.find("std::abort()") != std::string::npos, "AC2: worker abort");
        CHECK(wc.find("Issue #2372") != std::string::npos, "AC2: worker cites 2372");
        CHECK(fb.find("steal_snapshot_soft_production_locked") != std::string::npos,
              "AC2: fiber_bridge weak stub production-aware");
        CHECK(fb.find("std::abort()") != std::string::npos, "AC2: weak stub abort under lock");
        CHECK(fb.find("Issue #2372") != std::string::npos, "AC2: fiber_bridge cites 2372");
    }

    // AC3: test override wins over production lock (Soft usable for unit tests)
    {
        std::println("\n--- AC3: test override Soft under production lock ---");
        ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "0", 1);
        aura::serve::set_steal_snapshot_soft_production_locked(true);
        aura::serve::set_steal_snapshot_soft_for_test(true);
        CHECK(aura::serve::is_steal_snapshot_soft_mode(),
              "AC3: test Soft override wins over production lock");
        aura::serve::set_steal_snapshot_soft_for_test(false);
        CHECK(!aura::serve::is_steal_snapshot_soft_mode(),
              "AC3: test non-Soft override under lock");
        aura::serve::reset_steal_snapshot_soft_for_test();
        CHECK(!aura::serve::is_steal_snapshot_soft_mode(),
              "AC3: reset + lock → not Soft even if env was set earlier");
        // Sandbox=off path: apply_production_security_defaults unlocks Soft.
        ::setenv("AURA_SANDBOX", "off", 1);
        ::setenv("AURA_STEAL_SNAPSHOT_SOFT", "1", 1);
        aura::compiler::security::apply_production_security_defaults();
        CHECK(!aura::serve::steal_snapshot_soft_production_locked(),
              "AC3: sandbox=off unlocks Soft production lock");
        CHECK(aura::serve::is_steal_snapshot_soft_mode(),
              "AC3: Soft usable under AURA_SANDBOX=off");
        ::unsetenv("AURA_SANDBOX");
        ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
        aura::serve::reset_steal_snapshot_soft_for_test();
        aura::serve::set_steal_snapshot_soft_production_locked(saved_lock);
    }

    // AC4: happy path — consistent Soft check is lock-first (no getenv under lock)
    {
        std::println("\n--- AC4: happy path (lock-first Soft check) ---");
        const auto fh = read_file("src/serve/fiber.h");
        CHECK(fh.find("relaxed") != std::string::npos ||
                  fh.find("Happy path") != std::string::npos ||
                  fh.find("happy path") != std::string::npos,
              "AC4: happy path documented in fiber.h");
        // Under production lock, Soft check returns false without needing Soft env.
        aura::serve::reset_steal_snapshot_soft_for_test();
        aura::serve::set_steal_snapshot_soft_production_locked(true);
        ::unsetenv("AURA_STEAL_SNAPSHOT_SOFT");
        CHECK(!aura::serve::is_steal_snapshot_soft_mode(), "AC4: lock-only Soft=false");
        aura::serve::set_steal_snapshot_soft_production_locked(saved_lock);
    }

    // AC5: query schema-2372 + source-cite
    {
        std::println("\n--- AC5: query schema-2372 + source-cite ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2372") == 2372, "schema-2372");
        CHECK(href(cs, "issue-2372") == 2372, "issue-2372");
        CHECK(href(cs, "steal-snapshot-soft-forbidden-wired") == 1, "soft-forbidden-wired");
        const auto locked = href(cs, "steal-snapshot-soft-production-locked");
        CHECK(locked == 0 || locked == 1, "soft-production-locked 0|1");
        // Retain prior schema keys.
        CHECK(href(cs, "schema-2346") == 2346, "2346 retained");
        CHECK(href(cs, "schema-2310") == 2310, "2310 retained");
        CHECK(href(cs, "schema-2184") == 2184, "2184 retained");

        const auto fh = read_file("src/serve/fiber.h");
        const auto fc = read_file("src/serve/fiber.cpp");
        const auto sd = read_file("src/compiler/security_defaults.hh");
        const auto q = read_file("src/compiler/evaluator_primitives_obs_eval.cpp");
        CHECK(fh.find("Issue #2372") != std::string::npos, "fiber.h #2372");
        CHECK(fc.find("Issue #2372") != std::string::npos, "fiber.cpp #2372");
        CHECK(sd.find("set_steal_snapshot_soft_production_locked(!dev_off)") != std::string::npos,
              "security_defaults wires lock");
        CHECK(q.find("schema-2372") != std::string::npos, "query schema");
        CHECK(q.find("steal-snapshot-soft-forbidden-wired") != std::string::npos,
              "query wired key");
    }

    // Restore process Soft policy state.
    aura::serve::reset_steal_snapshot_soft_for_test();
    aura::serve::set_steal_snapshot_soft_production_locked(saved_lock);

    std::println("\n=== #2372 results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_steal_snapshot_soft_production_lock();
}
#endif
