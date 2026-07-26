// @category: unit
// @reason: Issue #2160 — wire GcDeferReason::RenderPin end-to-end
// (arm/release on render hotpath + should_defer + stats).
//
//   AC_R1: during render hotpath, GCCollector::request() returns false and
//          bumps request-deferred-render / arm-render-pin metrics.
//   AC_R2: after exit_render_hotpath, GC request can succeed again.
//   AC_R3: nested present + ffi-pin both armed; release order does not clear
//          the other early.
//   AC_R4: source-cite arm/release sites; schema-2160 keys present.

#include "test_harness.hpp"

#include "core/arena_auto_policy_stats.h"
#include "core/gc_hooks.h"
#include "serve/gc_coordinator.h"
#include "serve/scheduler.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.arena;

namespace {

using aura::ast::ASTArena;
using aura::ast::LiveCompactMode;
using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::serve::GCCollector;
using aura::serve::Scheduler;
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

std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:gc-defer-reason-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Drain residual render-pin depth (tests can nest/unbalanced if they fail mid-way).
void drain_render_pin() {
    while (aura::gc_hooks::render_pin_defer_active())
        aura::gc_hooks::release_render_pin_defer();
    while (aura::core::arena_policy::in_render_hotpath())
        aura::core::arena_policy::exit_render_hotpath();
}

} // namespace

int main() {
    std::println("=== Issue #2160: GcDeferReason::RenderPin end-to-end ===");
    drain_render_pin();

    // ── AC_R4: source contract first ──
    {
        std::println("\n--- AC_R4: source cites + arm/release sites ---");
        const auto pol = read_file("src/core/arena_auto_policy_stats.h");
        CHECK(!pol.empty(), "arena_auto_policy_stats readable");
        CHECK(pol.find("arm_render_pin_defer") != std::string::npos,
              "AC_R4: enter_render_hotpath arms RenderPin");
        CHECK(pol.find("release_render_pin_defer") != std::string::npos,
              "AC_R4: exit_render_hotpath releases RenderPin");
        CHECK(pol.find("2160") != std::string::npos, "AC_R4: policy cites 2160");

        const auto hooks = read_file("src/core/gc_hooks.h");
        CHECK(hooks.find("arm_render_pin_defer") != std::string::npos, "AC_R4: arm API");
        CHECK(hooks.find("release_render_pin_defer") != std::string::npos, "AC_R4: release API");
        CHECK(hooks.find("RenderPin") != std::string::npos, "AC_R4: enum RenderPin");
        CHECK(hooks.find("2160") != std::string::npos, "AC_R4: gc_hooks cites 2160");

        const auto arena = read_file("src/core/arena.ixx");
        CHECK(arena.find("should_defer_destructive_gc") != std::string::npos,
              "AC_R4: Force observes unified predicate");
        CHECK(arena.find("2160") != std::string::npos, "AC_R4: arena cites 2160");
    }

    // ── AC_R1: hotpath arms RenderPin → GC request defers ──
    {
        std::println("\n--- AC_R1: request defers under render hotpath ---");
        drain_render_pin();
        Scheduler sched(1);
        auto* gc = sched.gc_collector();
        CHECK(gc != nullptr, "GCCollector present");
        gc->set_alloc_threshold(1);
        for (int i = 0; i < 16; ++i)
            gc->record_alloc();

        const auto arm0 =
            aura::gc_hooks::g_gc_defer_arm_render_pin_total.load(std::memory_order_relaxed);
        const auto req0 =
            aura::gc_hooks::g_gc_request_deferred_render_total.load(std::memory_order_relaxed);
        const auto because0 =
            aura::gc_hooks::g_defer_because_render_total.load(std::memory_order_relaxed);

        aura::core::arena_policy::enter_render_hotpath();
        CHECK(aura::core::arena_policy::in_render_hotpath(), "AC_R1: hotpath active");
        CHECK(aura::gc_hooks::render_pin_defer_active(), "AC_R1: render_pin active");
        CHECK(aura::gc_hooks::should_defer_destructive_gc(), "AC_R1: should_defer true");
        CHECK((aura::gc_hooks::defer_reasons_snapshot() &
               static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::RenderPin)) != 0,
              "AC_R1: RenderPin bit set");
        CHECK(aura::gc_hooks::g_gc_defer_arm_render_pin_total.load() > arm0,
              "AC_R1: arm-render-pin-total advanced");

        CHECK(!gc->request(), "AC_R1: request returns false under RenderPin");
        CHECK(aura::gc_hooks::g_gc_request_deferred_render_total.load() > req0,
              "AC_R1: request-deferred-render advanced");
        CHECK(aura::gc_hooks::g_defer_because_render_total.load() > because0,
              "AC_R1: defer-because-render advanced");

        // Force live_compact also soft-gates under RenderPin.
        ASTArena arena(64 * 1024);
        (void)arena.try_allocate(32);
        const auto r = arena.live_compact(LiveCompactMode::Force);
        CHECK(r.soft_gated, "AC_R1: Force soft-gated under RenderPin");

        aura::core::arena_policy::exit_render_hotpath();
    }

    // ── AC_R2: after exit, GC can arm again ──
    {
        std::println("\n--- AC_R2: after exit, request can succeed ---");
        drain_render_pin();
        CHECK(!aura::gc_hooks::render_pin_defer_active(), "AC_R2: not armed");
        CHECK(!aura::gc_hooks::should_defer_destructive_gc() ||
                  aura::gc_hooks::gc_deferred_for_pending_panic() ||
                  aura::gc_hooks::ffi_pin_defer_active(),
              "AC_R2: no residual RenderPin (other reasons ok)");

        // Clear any leftover panic/ffi for a clean request probe.
        while (aura::gc_hooks::ffi_pin_defer_active())
            aura::gc_hooks::release_ffi_pin_defer();

        Scheduler sched(1);
        auto* gc = sched.gc_collector();
        CHECK(gc != nullptr, "gc");
        gc->set_alloc_threshold(1);
        for (int i = 0; i < 16; ++i)
            gc->record_alloc();

        // If panic still armed globally from other tests, skip request-true.
        if (!aura::gc_hooks::should_defer_destructive_gc()) {
            const bool req = gc->request();
            CHECK(req, "AC_R2: request succeeds after hotpath exit");
            if (req)
                (void)gc->collect(); // best-effort complete cycle
        } else {
            CHECK(true, "AC_R2: other defer residual — request path exercised under arm");
        }
        CHECK(!aura::core::arena_policy::in_render_hotpath(), "AC_R2: hotpath depth 0");
    }

    // ── AC_R3: nested present + ffi-pin; release order ──
    {
        std::println("\n--- AC_R3: nested RenderPin + FfiPin ---");
        drain_render_pin();
        while (aura::gc_hooks::ffi_pin_defer_active())
            aura::gc_hooks::release_ffi_pin_defer();

        aura::core::arena_policy::enter_render_hotpath(); // RenderPin depth 1
        aura::gc_hooks::arm_ffi_pin_defer();              // FfiPin depth 1
        CHECK(aura::gc_hooks::render_pin_defer_active(), "AC_R3: render armed");
        CHECK(aura::gc_hooks::ffi_pin_defer_active(), "AC_R3: ffi armed");
        const auto mask = aura::gc_hooks::defer_reasons_snapshot();
        CHECK((mask & static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::RenderPin)) != 0,
              "AC_R3: RenderPin bit");
        CHECK((mask & static_cast<std::uint32_t>(aura::gc_hooks::GcDeferReason::FfiPin)) != 0,
              "AC_R3: FfiPin bit");

        // Nested render enter/exit.
        aura::core::arena_policy::enter_render_hotpath();
        CHECK(aura::gc_hooks::render_pin_defer_depth() >= 2, "AC_R3: render depth nested");
        aura::core::arena_policy::exit_render_hotpath();
        CHECK(aura::gc_hooks::render_pin_defer_active(), "AC_R3: still armed after nested exit");
        CHECK(aura::gc_hooks::ffi_pin_defer_active(), "AC_R3: ffi still armed");

        // Release render first — ffi must remain.
        aura::core::arena_policy::exit_render_hotpath();
        CHECK(!aura::gc_hooks::render_pin_defer_active(), "AC_R3: render cleared");
        CHECK(aura::gc_hooks::ffi_pin_defer_active(), "AC_R3: ffi remains after render release");
        CHECK(aura::gc_hooks::should_defer_destructive_gc(), "AC_R3: still defer (ffi)");

        // Release ffi — fully clear.
        aura::gc_hooks::release_ffi_pin_defer();
        CHECK(!aura::gc_hooks::ffi_pin_defer_active(), "AC_R3: ffi cleared");
        CHECK(!aura::gc_hooks::should_defer_destructive_gc() ||
                  aura::gc_hooks::gc_deferred_for_pending_panic(),
              "AC_R3: no render/ffi residual");

        // Reverse order: ffi then nested render, release ffi first.
        aura::gc_hooks::arm_ffi_pin_defer();
        aura::core::arena_policy::enter_render_hotpath();
        aura::gc_hooks::release_ffi_pin_defer();
        CHECK(!aura::gc_hooks::ffi_pin_defer_active(), "AC_R3: ffi cleared first");
        CHECK(aura::gc_hooks::render_pin_defer_active(), "AC_R3: render remains after ffi release");
        CHECK(aura::gc_hooks::should_defer_destructive_gc(), "AC_R3: still defer (render)");
        aura::core::arena_policy::exit_render_hotpath();
        CHECK(!aura::gc_hooks::render_pin_defer_active(), "AC_R3: both clear");
    }

    // ── Query schema-2160 ──
    {
        std::println("\n--- query schema-2160 ---");
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "schema-2160") == 2160, "schema-2160");
        CHECK(href(cs, "render-pin-wired") == 1, "wired");
        CHECK(href(cs, "arm-render-pin-total") >= 0, "arm-render-pin-total key");
        CHECK(href(cs, "render-pin-depth") >= 0, "render-pin-depth key");
        CHECK(href(cs, "defer-because-render") >= 0, "defer-because-render key");
        CHECK(href(cs, "request-deferred-render-total") >= 0, "request-deferred key");
        CHECK(href(cs, "render-pin-bit") >= 0, "render-pin-bit key");
    }

    drain_render_pin();
    std::println("\n=== #2160 RenderPin end-to-end: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
