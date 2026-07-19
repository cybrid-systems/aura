// @category: unit
// @reason: Issue #1768 — try_lock_workspace_shared/unique must update
// lock_order TLS (on_acquire on try, on_release on fail or unlock).
//
//   AC1: source cites #1768; try_lock calls on_acquire + rollback
//   AC2: successful try_lock_shared → Workspace is_held; unlock clears
//   AC3: failed try (already unique-held) does not leave depth stuck
//   AC4: try_lock_unique success + unlock_unique balances TLS

#include "test_harness.hpp"
#include "compiler/lock_order_audit.h"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::lock_order::g_lock_order_acquire_total;
using aura::compiler::lock_order::g_lock_order_release_total;
using aura::compiler::lock_order::is_held;
using aura::compiler::lock_order::Level;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: try_lock wires lock_order ---");
        std::string ixx;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            ixx = read_file(p);
            if (!ixx.empty())
                break;
        }
        CHECK(!ixx.empty(), "read evaluator.ixx");
        CHECK(ixx.find("#1768") != std::string::npos, "cites #1768");
        auto pos = ixx.find("bool try_lock_workspace_shared()");
        CHECK(pos != std::string::npos, "try_lock_workspace_shared present");
        auto win = ixx.substr(pos, 900);
        CHECK(win.find("lock_order::on_acquire") != std::string::npos, "on_acquire in try shared");
        CHECK(win.find("try_lock_shared()") != std::string::npos, "calls try_lock_shared");
        CHECK(win.find("lock_order::on_release") != std::string::npos, "on_release rollback");
        CHECK(ixx.find("bool try_lock_workspace_unique()") != std::string::npos,
              "try_lock_workspace_unique sibling");
    }

    // ── AC2: success path ──
    {
        std::println("\n--- AC2: try shared success updates TLS ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(!is_held(Level::Workspace), "Workspace not held initially");
        const auto a0 = g_lock_order_acquire_total.load(std::memory_order_relaxed);
        const auto r0 = g_lock_order_release_total.load(std::memory_order_relaxed);
        CHECK(ev.try_lock_workspace_shared(), "try_lock_shared succeeds");
        CHECK(is_held(Level::Workspace), "Workspace held after try");
        CHECK(g_lock_order_acquire_total.load(std::memory_order_relaxed) >= a0 + 1,
              "acquire total +1");
        ev.unlock_workspace_shared();
        CHECK(!is_held(Level::Workspace), "Workspace released after unlock");
        CHECK(g_lock_order_release_total.load(std::memory_order_relaxed) >= r0 + 1,
              "release total +1");
    }

    // ── AC3: failed try does not stick depth ──
    {
        std::println("\n--- AC3: failed try rolls back TLS ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        // Hold unique so shared try_lock fails (or would deadlock if blocking).
        ev.lock_workspace_unique();
        CHECK(is_held(Level::Workspace), "unique held");
        // Depth is already 1 from unique. A failed shared try must not
        // leave depth at 2.
        const bool got = ev.try_lock_workspace_shared();
        // Some platforms allow shared after unique on same thread (undefined
        // for shared_mutex). Prefer: if fail, depth still 1; if succeed,
        // unlock shared and still consistent.
        if (!got) {
            CHECK(is_held(Level::Workspace), "still held via unique");
            // Depth should still be 1 (acquire rolled back).
            // We can only observe is_held, not depth; a second unlock
            // must clear fully.
            ev.unlock_workspace_unique();
            CHECK(!is_held(Level::Workspace), "cleared after unique unlock (no stuck try)");
        } else {
            // Same-thread shared-after-unique allowed: release both.
            ev.unlock_workspace_shared();
            ev.unlock_workspace_unique();
            CHECK(!is_held(Level::Workspace), "cleared after both unlocks");
            CHECK(true, "platform allowed shared under unique; unlocked cleanly");
        }
    }

    // ── AC4: try unique ──
    {
        std::println("\n--- AC4: try_lock_workspace_unique ---");
        CompilerService cs;
        auto& ev = cs.evaluator();
        CHECK(ev.try_lock_workspace_unique(), "try unique succeeds");
        CHECK(is_held(Level::Workspace), "held after try unique");
        ev.unlock_workspace_unique();
        CHECK(!is_held(Level::Workspace), "released after unlock unique");
    }

    std::println("\n=== test_try_lock_workspace_lock_order_1768: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
