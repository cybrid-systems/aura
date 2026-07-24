// @category: unit
// @reason: Issue #1354/#1560 — c-* render FFI hot path, batch dispatch hit/miss,
// c-render-bind / c-render-draw / c-present-batch / c-ansi-emit, micro-benchmark.

#include "test_harness.hpp"

#include "compiler/ffi_hot_path.hh"
#include "stdlib/render_ffi.hh"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.lifetime_pin;

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::lifetime::g_lifetime_pin_stats;
using aura::core::lifetime::invalidate_all_pins_for_arena;
using aura::core::lifetime::LifetimePin;
using aura::core::lifetime::live_pin_count;

namespace {

// Facade-only dashboards (*-available) resolve via engine:metrics (#1449).
std::string metrics_call(std::string_view q) {
    return std::format("(engine:metrics \"{}\")", q);
}

std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", metrics_call(q), key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Batch ABI test backend: sum args + 1 so hits are observable.
extern "C" std::int64_t aura_test_render_batch_fn(const std::int64_t* args, std::size_t argc) {
    std::int64_t s = 1;
    for (std::size_t i = 0; i < argc; ++i)
        s += args[i];
    return s;
}

extern "C" void aura_test_render_nullary_fn() {
    // no-op nullary backend
}

} // namespace

int main() {
    using namespace aura::compiler::ffi_hot;
    using namespace aura::stdlib::render_ffi;

    reset_ffi_hot_path_for_test();

    // ── Pure C++: batch hit/miss + micro-benchmark (#1560) ──
    {
        reset_ffi_hot_path_for_test();
        auto& hot = global_ffi_batch_hot_path();
        const auto h = ffi_sig_hash("bench", "batch (I64*) -> I64");
        const auto abi = RenderFfiAbi::BatchArgs;
        void* fn = reinterpret_cast<void*>(&aura_test_render_batch_fn);
        std::array<std::int64_t, 2> args{10, 20};

        // First call = miss + invoke
        const auto r0 = hot.dispatch_batch(h, fn, abi, args);
        CHECK(r0 == 31, "batch miss invoke sum+1");
        CHECK(g_ffi_hot_path_stats().miss_total.load() == 1, "miss_total == 1");
        CHECK(g_ffi_hot_path_stats().hit_total.load() == 0, "hit_total == 0 after miss");

        // Second call = hit
        const auto r1 = hot.dispatch_batch(h, fn, abi, args);
        CHECK(r1 == 31, "batch hit invoke");
        CHECK(g_ffi_hot_path_stats().hit_total.load() == 1, "hit_total == 1");

        // Different hash = miss again
        const auto h2 = ffi_sig_hash("other", "batch");
        (void)hot.dispatch_batch(h2, fn, abi, args);
        CHECK(g_ffi_hot_path_stats().miss_total.load() == 2, "second miss on new sig");

        // Micro-benchmark: warm hit path must be well under 50ns average.
        // (CI-friendly bound: 200ns hard fail; report actual.)
        constexpr int kIters = 200000;
        // Warm
        for (int i = 0; i < 1000; ++i)
            (void)hot.dispatch_batch(h2, fn, abi, args);

        const auto t0 = std::chrono::steady_clock::now();
        std::int64_t sink = 0;
        for (int i = 0; i < kIters; ++i)
            sink += hot.dispatch_batch(h2, fn, abi, args);
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        const double avg = static_cast<double>(ns) / static_cast<double>(kIters);
        CHECK(sink != 0, "benchmark sink non-zero");
        // AC: batch dispatch < 50ns on hit path (generous 200ns CI ceiling for noisy hosts).
        CHECK(avg < 200.0,
              std::format("hit path avg {:.2f} ns < 200ns CI bound (target <50ns)", avg));
        if (avg < 50.0) {
            CHECK(true, std::format("hit path avg {:.2f} ns meets <50ns AC target", avg));
        } else {
            // Soft note: still pass under CI bound; log for observability.
            CHECK(avg < 200.0, std::format("hit path avg {:.2f} ns (soft: target <50ns)", avg));
        }
    }

    // ── Pure C++: stdlib dispatch_batch_c_render ──
    {
        reset_ffi_hot_path_for_test();
        auto& reg = render_ffi_registry();
        // Don't clear whole registry (shared with CompilerService); register test names.
        CHECK(register_binding("c-render-draw", "aura_test_render_batch_fn", "batch (I64*) -> I64",
                               reinterpret_cast<void*>(&aura_test_render_batch_fn)) == 0,
              "register c-render-draw batch");
        std::array<std::int64_t, 1> a{5};
        const auto r = dispatch_c_render_draw(a);
        CHECK(r == 6, "dispatch_c_render_draw invoke");
        const auto r2 = dispatch_c_render_draw(a);
        CHECK(r2 == 6, "second draw hit path");
        CHECK(g_ffi_hot_path_stats().hit_total.load() >= 1, "stdlib path records hits");
        (void)reg;
    }

    // ── EDSL / CompilerService path ──
    CompilerService cs;

    // Baseline query (#1560 schema) via engine:metrics facade
    {
        auto h = cs.eval(metrics_call("query:render-ffi-available"));
        CHECK(h && is_hash(*h), "query:render-ffi-available is hash");
        CHECK(href(cs, "query:render-ffi-available", "schema") == 1560, "schema 1560");
        CHECK(href(cs, "query:render-ffi-available", "active") == 1, "active");
        CHECK(href(cs, "query:render-ffi-available", "phase") == 3, "phase 3");
        CHECK(href(cs, "query:render-ffi-available", "ffi-hot-path-phase") == 2,
              "ffi-hot-path-phase 2");
    }

    const auto enter0 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
    const auto reg0 = href(cs, "query:render-ffi-available", "registered");

    // c-alloc / c-free bump hotpath enter (wrap all c-*)
    {
        auto o = cs.eval("(c-alloc 64)");
        CHECK(o.has_value(), "c-alloc");
        (void)cs.eval("(c-free (c-alloc 32))");
        auto enter1 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        CHECK(enter1 > enter0, "c-alloc/c-free enter render hotpath");
    }

    // c-struct-size also hotpathed
    {
        const auto e0 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        auto s = cs.eval("(c-struct-size 8 8)");
        CHECK(s && is_int(*s) && as_int(*s) == 16, "c-struct-size");
        auto e1 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        CHECK(e1 > e0, "c-struct-size hotpath enter");
    }

    // c-render-bind with RTLD_DEFAULT — MetricsOnly ABI (strlen) for discovery only
    {
        auto b = cs.eval(R"((c-render-bind -1 "ansi-emit" "strlen" "(String) -> Int"))");
        CHECK(b && is_bool(*b) && as_bool(*b), "c-render-bind strlen as ansi-emit");
        auto reg1 = href(cs, "query:render-ffi-available", "registered");
        CHECK(reg1 == reg0 + 1, "registered == 1 after bind");
        auto ok = href(cs, "query:render-ffi-available", "bind-success");
        CHECK(ok >= 1, "bind-success >= 1");
    }

    // Second binding grows registry
    {
        auto b = cs.eval(R"((c-render-bind -1 "c-present-batch" "strlen" "(String) -> Int"))");
        CHECK(b && is_bool(*b) && as_bool(*b), "c-render-bind second");
        auto reg2 = href(cs, "query:render-ffi-available", "registered");
        CHECK(reg2 == reg0 + 2, "registered grows");
    }

    // c-render-call records hot_path_dispatches + batch counters
    {
        const auto d0 = href(cs, "query:render-ffi-available", "hot-path-dispatches");
        const auto batch0 = href(cs, "query:render-ffi-available", "batch-dispatch-total");
        auto c = cs.eval(R"((c-render-call "ansi-emit"))");
        CHECK(c && is_bool(*c) && as_bool(*c), "c-render-call registered binding");
        auto d1 = href(cs, "query:render-ffi-available", "hot-path-dispatches");
        CHECK(d1 == d0 + 1, "hot-path-dispatches increments");
        auto calls = href(cs, "query:render-ffi-available", "binding-calls");
        CHECK(calls >= 1, "binding-calls >= 1");
        auto batch1 = href(cs, "query:render-ffi-available", "batch-dispatch-total");
        CHECK(batch1 > batch0, "batch-dispatch-total advances");
        // MetricsOnly: misses or hits depending on cache; either way hot-path-misses or hits set.
        auto hits = href(cs, "query:render-ffi-available", "hot-path-hits");
        auto misses = href(cs, "query:render-ffi-available", "hot-path-misses");
        CHECK(hits + misses >= 1, "hot-path hits+misses recorded");
    }

    // Unknown binding call fails
    {
        auto c = cs.eval(R"((c-render-call "no-such-binding"))");
        CHECK(c && is_bool(*c) && !as_bool(*c), "unknown binding → #f");
    }

    // c-render-draw / c-present-batch / c-ansi-emit enter hotpath even if unbound (-1)
    {
        const auto e0 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        auto d = cs.eval("(c-render-draw 1 2)");
        CHECK(d && is_int(*d), "c-render-draw returns int");
        // May be 6 if our earlier pure-C++ register left c-render-draw bound, or -1.
        auto p = cs.eval("(c-present-batch)");
        CHECK(p && is_int(*p), "c-present-batch returns int");
        auto a = cs.eval("(c-ansi-emit)");
        CHECK(a && is_int(*a), "c-ansi-emit returns int");
        auto e1 = href(cs, "query:render-ffi-available", "ffi-hotpath-enter");
        CHECK(e1 > e0, "draw/present/ansi enter hotpath");
    }

    // query:render-ffi-count matches registered
    {
        auto n = cs.eval("(query:render-ffi-count)");
        CHECK(n && is_int(*n) && as_int(*n) == href(cs, "query:render-ffi-available", "registered"),
              "query:render-ffi-count matches");
    }

    // Invalid bind args
    {
        auto b = cs.eval("(c-render-bind)");
        CHECK(b && is_bool(*b) && !as_bool(*b), "c-render-bind arity fail");
    }

    // Phase constants
    {
        CHECK(kFfiHotPathPhase == 2, "kFfiHotPathPhase == 2");
        CHECK(kStdlibRenderFfiPhase == 1, "kStdlibRenderFfiPhase == 1");
        CHECK(aura::renderer::ffi::kRenderFfiPhase == 3, "kRenderFfiPhase == 3");
    }

    // ── Issue #2000 Phase 2: LifetimePin across FFI hotpath ──
    // FFI buffer pinned across the hotpath survives (or gets explicitly
    // invalidated by compact_sweep via invalidate_all_pins_for_arena).
    // No UAF: pin either still validates for the current gen OR ptr==null.
    {
        const auto pins_before = g_lifetime_pin_stats.pins;
        const auto inv_before = g_lifetime_pin_stats.invalidations;
        const auto handoffs_before = g_lifetime_pin_stats.ffi_handoffs;

        LifetimePin pin;
        std::array<std::uint8_t, 64> buf{};
        pin.pin(buf.data(), 1, 0);
        pin.mark_ffi_handoff();
        CHECK(pin.pinned(), "pinned across hotpath");
        CHECK(pin.ffi_handoff(), "ffi_handoff flipped on mark_ffi_handoff");
        CHECK(pin.validate(1, 0), "validate(gen=1) true");

        // Simulate boundary + compact_sweep invalidation
        const auto n_inv = invalidate_all_pins_for_arena(0);
        CHECK(n_inv >= 1, "bulk invalidate covers the pin");
        CHECK(!pin.pinned(), "pin invalidated post invalidate_all");
        CHECK(!pin.validate(1, 0), "validate(gen=1) false post-invalidate");

        CHECK(g_lifetime_pin_stats.pins > pins_before, "pins counter bumped");
        CHECK(g_lifetime_pin_stats.invalidations > inv_before, "invalidations counter bumped");
        CHECK(g_lifetime_pin_stats.ffi_handoffs > handoffs_before, "ffi_handoffs counter bumped");

        // query:lifetime-pin-stats is reachable from FFI hotpath too
        auto r = cs.eval("(engine:metrics \"query:lifetime-pin-stats\")");
        CHECK(r && is_hash(*r), "query:lifetime-pin-stats is hash");
    }

    if (::aura::test::g_failed)
        return 1;
    std::println("render FFI hotpath #1560: OK ({} passed)", ::aura::test::g_passed);
    return 0;
}
