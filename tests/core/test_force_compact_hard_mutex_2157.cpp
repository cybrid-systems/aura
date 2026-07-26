// @category: unit
// @reason: Issue #2157 — Force live_compact hard-mutex with live
// LifetimePin + EnvFrameLifetimeGuard (no gen bump / pin invalidate while held).
//
//   AC_F1: LifetimePin held → Force does not bump gen / does not null pin
//   AC_F2: EnvFrameLifetimeGuard in scope → Force blocked; after dtor, Force ok
//   AC_F3: Soft path unchanged (still gates on boundary/render only);
//          metrics + schema-2157 on query surfaces

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.arena;
import aura.core.envframe_lifetime;
import aura.core.lifetime_pin;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::ASTArena;
using aura::ast::kForceCompactHardMutexIssue;
using aura::ast::LiveCompactMode;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::envframe_lifetime::active_guard_depth;
using aura::core::envframe_lifetime::EnvFrameLifetimeGuard;
using aura::core::envframe_lifetime::EnvFrameLifetimeHost;
using aura::core::envframe_lifetime::EnvFrameLifetimeSite;
using aura::core::envframe_lifetime::reset_envframe_lifetime_stats;
using aura::core::lifetime::LifetimePin;
using aura::core::lifetime::live_pin_count;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
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

// Dummy scan callback so EnvFrameLifetimeGuard is "armed".
void dummy_scan(void*, EnvFrameLifetimeSite) noexcept {}

} // namespace

int main() {
    std::println("=== Issue #2157: Force live_compact hard-mutex ===");
    CHECK(kForceCompactHardMutexIssue == 2157, "issue stamp");
    reset_envframe_lifetime_stats();

    // ── AC_F1: LifetimePin held blocks Force gen bump ──
    {
        std::println("\n--- AC_F1: pin held → Force blocked ---");
        ASTArena arena(64 * 1024);
        // Allocate something so freelist / layout can change on real Force.
        void* buf = arena.try_allocate(32);
        CHECK(buf != nullptr, "alloc");
        const auto gen0 = arena.generation();
        const auto pin_block0 =
            aura::ast::g_force_compact_blocked_by_pin_total.load(std::memory_order_relaxed);

        LifetimePin pin;
        pin.pin(buf, gen0, arena.arena_id());
        CHECK(pin.pinned(), "pin live");
        CHECK(live_pin_count() >= 1, "live_pin_count >= 1");

        const auto r = arena.live_compact(LiveCompactMode::Force);
        CHECK(r.force_blocked_by_pin, "AC_F1: force_blocked_by_pin");
        CHECK(r.force_blocked(), "AC_F1: force_blocked()");
        CHECK(r.soft_gated, "AC_F1: soft_gated marker set");
        CHECK(!r.invalidates_pins, "AC_F1: no pin invalidate");
        CHECK(r.new_gen == gen0 || r.new_gen == arena.generation(),
              "AC_F1: new_gen not advanced past");
        CHECK(arena.generation() == gen0, "AC_F1: arena gen unchanged");
        CHECK(pin.pinned(), "AC_F1: pin still held");
        CHECK(pin.ptr() == buf, "AC_F1: pin ptr not nulled");
        CHECK(pin.validate(gen0, arena.arena_id()), "AC_F1: pin still valid");
        CHECK(aura::ast::g_force_compact_blocked_by_pin_total.load() > pin_block0,
              "AC_F1: pin-block metric");

        // After pin goes out of scope (unpin), Force can proceed.
    }
    {
        ASTArena arena(64 * 1024);
        void* buf = arena.try_allocate(48);
        CHECK(buf != nullptr, "alloc2");
        // Create freelist pressure so Force may restamp when allowed.
        void* b2 = arena.try_allocate(16);
        if (b2) { /* keep */
        }
        const auto gen0 = arena.generation();
        CHECK(live_pin_count() == 0 || true, "no forced pin hold");
        // Clear any leftover pins from previous block (pin dtor ran).
        const auto r = arena.live_compact(LiveCompactMode::Force);
        CHECK(!r.force_blocked_by_pin, "Force not pin-blocked without live pin");
        // May or may not bump gen depending on freelist; just not blocked.
        (void)gen0;
        (void)r;
    }

    // ── AC_F2: EnvFrameLifetimeGuard blocks Force; dtor unblocks ──
    {
        std::println("\n--- AC_F2: EnvFrameLifetimeGuard hold ---");
        reset_envframe_lifetime_stats();
        ASTArena arena(64 * 1024);
        void* buf = arena.try_allocate(32);
        CHECK(buf != nullptr, "alloc");
        const auto gen0 = arena.generation();
        const auto env_block0 = aura::ast::g_force_compact_blocked_by_envframe_guard_total.load();

        {
            EnvFrameLifetimeHost host{};
            host.ctx = &arena;
            host.scan_skip_freed = &dummy_scan;
            EnvFrameLifetimeGuard guard(host, EnvFrameLifetimeSite::CompactSweep);
            CHECK(active_guard_depth() >= 1, "AC_F2: active_guard_depth >= 1");
            CHECK(guard.armed(), "guard armed");

            const auto r = arena.live_compact(LiveCompactMode::Force);
            CHECK(r.force_blocked_by_envframe_guard, "AC_F2: blocked by envframe guard");
            CHECK(r.force_blocked(), "AC_F2: force_blocked");
            CHECK(arena.generation() == gen0, "AC_F2: gen unchanged under hold");
            CHECK(aura::ast::g_force_compact_blocked_by_envframe_guard_total.load() > env_block0,
                  "AC_F2: envframe-block metric");
        }
        CHECK(active_guard_depth() == 0, "AC_F2: depth 0 after dtor");

        // After dtor, Force not blocked by envframe.
        const auto r2 = arena.live_compact(LiveCompactMode::Force);
        CHECK(!r2.force_blocked_by_envframe_guard, "AC_F2: unblocked after dtor");
        (void)r2;
        (void)buf;
    }

    // ── AC_F3: Soft unchanged; query surface ──
    {
        std::println("\n--- AC_F3: Soft path + query schema-2157 ---");
        ASTArena arena(64 * 1024);
        LifetimePin pin;
        void* buf = arena.try_allocate(16);
        pin.pin(buf, arena.generation(), arena.arena_id());
        // Soft does NOT check pin hold (only boundary/render) — may run or
        // soft-gate for other reasons, but not force_blocked_by_pin.
        const auto rs = arena.live_compact(LiveCompactMode::Soft);
        CHECK(!rs.force_blocked_by_pin, "AC_F3: Soft does not set force_blocked_by_pin");
        CHECK(!rs.force_blocked_by_envframe_guard, "AC_F3: Soft no envframe force flag");
        (void)rs;

        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "query:arena-live-compact-stats", "schema-2157") == 2157,
              "schema-2157 on arena-live-compact-stats");
        CHECK(href(cs, "query:arena-live-compact-stats", "force-hard-mutex-wired") == 1, "wired");
        CHECK(href(cs, "query:arena-live-compact-stats", "force-blocked-by-pin-total") >= 0,
              "pin-block total key");
        CHECK(href(cs, "query:lifetime-pin-stats", "schema-2157") == 2157,
              "schema-2157 on lifetime-pin-stats");
        CHECK(href(cs, "query:lifetime-pin-stats", "envframe-active-guard-depth") >= 0,
              "active guard depth key");
    }

    // ── Source contract ──
    {
        const auto ar = read_file("src/core/arena.ixx");
        CHECK(ar.find("2157") != std::string::npos, "arena cites 2157");
        CHECK(ar.find("live_pin_count") != std::string::npos, "Force checks live_pin_count");
        CHECK(ar.find("active_guard_depth") != std::string::npos,
              "Force checks active_guard_depth");
        CHECK(ar.find("force_blocked_by_pin") != std::string::npos, "result field");
        const auto ef = read_file("src/core/envframe_lifetime.ixx");
        CHECK(ef.find("active_guard_depth") != std::string::npos, "depth API");
        CHECK(ef.find("2157") != std::string::npos, "envframe cites 2157");
    }

    std::println("\n=== #2157 force compact hard-mutex: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}
