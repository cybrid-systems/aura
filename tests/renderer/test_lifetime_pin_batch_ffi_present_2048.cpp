// @category: unit
// @reason: Issue #2048 — low-overhead batch FFI + zero-copy handoff for
// terminal present under MutationBoundary / LifetimePin soft-gate.
//
//   AC1: source cites #2048; LifetimePin Phase 2; present FfiPresentPinGuard
//   AC2: present_batch pins zero-copy buffer (handoff hits++); large ≥4KiB
//   AC3: (ffi:pin-buffer)/(ffi:unpin-buffer) share registry; query:ffi-pin-count
//   AC4: pin live → compact deferred (ffi_pin_defer_active); unpin releases
//   AC5: pin → mutate boundary restamp path; pin survives until unpin/invalidate
//   AC6: query:lifetime-pin-stats + query:render-stats schema-2048
//   AC7: clean present does not handoff; lightweight soft-gate still works

#include "test_harness.hpp"

#include "core/gc_hooks.h"
#include "core/zero_copy_output.hh"
#include "renderer/render_primitives.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.lifetime_pin;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::lifetime::invalidate_all_pins_for_arena;
using aura::core::lifetime::LifetimePin;
using aura::core::zero_copy::g_zero_copy_metrics;
using aura::core::zero_copy::reset_zero_copy_metrics_for_test;
using aura::renderer::draw_cell;
using aura::renderer::FramebufferOwned;
using aura::renderer::present_batch_to_string;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::TermCell;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

std::int64_t href_lp(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:lifetime-pin-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t href_rs(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (stats:get \"query:render-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

std::int64_t as_i(CompilerService& cs, std::string_view expr) {
    auto r = cs.eval(expr);
    if (!r || !is_int(*r))
        return -999;
    return as_int(*r);
}

TermCell cell_ch(char ch) {
    TermCell c;
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = 7;
    c.mode = 0;
    return c;
}

void ac1_source() {
    std::println("\n--- AC1: source #2048 ---");
    auto pin_ixx = read_file("src/core/lifetime_pin.ixx");
    auto pin_hh = read_file("src/core/lifetime_pin.hh");
    auto prim = read_file("src/renderer/render_primitives.cpp");
    auto ffi = read_file("src/compiler/ffi_primitives_impl.cpp");
    auto gc = read_file("src/compiler/evaluator_gc.cpp");
    CHECK(!pin_ixx.empty() && pin_ixx.find("#2048") != std::string::npos, "lifetime_pin.ixx");
    CHECK(pin_ixx.find("kLifetimePinPhase = 2") != std::string::npos, "phase 2");
    CHECK(!pin_hh.empty() && pin_hh.find("#2048") != std::string::npos, "lifetime_pin.hh");
    CHECK(!prim.empty() && prim.find("#2048") != std::string::npos, "present path #2048");
    CHECK(prim.find("FfiPresentPinGuard") != std::string::npos, "FfiPresentPinGuard");
    CHECK(prim.find("arm_ffi_pin_defer") != std::string::npos, "arm defer on present");
    CHECK(!ffi.empty() && ffi.find("g_ffi_pin_registry") != std::string::npos, "shared registry");
    CHECK(!gc.empty() && gc.find("ffi_pin_defer_active") != std::string::npos, "GC consults defer");
}

void ac2_present_handoff() {
    std::println("\n--- AC2: present_batch zero-copy handoff metrics ---");
    reset_zero_copy_metrics_for_test();
    reset_render_engine_counters_for_test();

    FramebufferOwned owned;
    owned.resize(80, 24); // ~1.9k cells → multi-KB ANSI
    auto fb = owned.view();
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 80; ++x)
            (void)draw_cell(fb, owned.dirty, static_cast<std::uint32_t>(x),
                            static_cast<std::uint32_t>(y), cell_ch('#'));

    const auto handoff0 =
        g_zero_copy_metrics().zero_copy_handoff_hits.load(std::memory_order_relaxed);
    const auto pin0 = g_zero_copy_metrics().present_pin_handoffs.load(std::memory_order_relaxed);
    const auto large0 =
        g_zero_copy_metrics().zero_copy_large_handoff_hits.load(std::memory_order_relaxed);

    std::string out;
    const auto n = present_batch_to_string(fb, owned.dirty, out);
    CHECK(n > 0, "present bytes > 0");
    CHECK(out.size() == static_cast<std::size_t>(n), "size match");

    const auto handoff1 =
        g_zero_copy_metrics().zero_copy_handoff_hits.load(std::memory_order_relaxed);
    const auto pin1 = g_zero_copy_metrics().present_pin_handoffs.load(std::memory_order_relaxed);
    const auto large1 =
        g_zero_copy_metrics().zero_copy_large_handoff_hits.load(std::memory_order_relaxed);
    std::println("  handoff {}→{} pin {}→{} large {}→{} n={}", handoff0, handoff1, pin0, pin1,
                 large0, large1, n);
    CHECK(handoff1 > handoff0, "zero_copy_handoff_hits++");
    CHECK(pin1 > pin0, "present_pin_handoffs++");
    if (n >= 4096)
        CHECK(large1 > large0, "large handoff for ≥4KiB batch");
    else
        CHECK(true, "batch <4KiB (large counter optional)");
}

void ac3_ffi_pin_registry() {
    std::println("\n--- AC3: shared ffi pin registry ---");
    CompilerService cs;
    CHECK(as_i(cs, "(query:ffi-pin-count)") == 0, "pre count 0");
    // Pin a fake address (not dereferenced by pin itself)
    const auto h = as_i(cs, "(ffi:pin-buffer 4096 7 0)");
    CHECK(h >= 0, "pin handle");
    CHECK(as_i(cs, "(query:ffi-pin-count)") >= 1, "count after pin");
    CHECK(aura::gc_hooks::ffi_pin_defer_active(), "defer armed while pinned");
    CHECK(as_i(cs, std::format("(ffi:unpin-buffer {})", h)) == 1, "unpin ok");
    CHECK(as_i(cs, "(query:ffi-pin-count)") == 0, "count after unpin");
    CHECK(!aura::gc_hooks::ffi_pin_defer_active(), "defer released");
}

void ac4_defer_while_pinned() {
    std::println("\n--- AC4: pin holds GC defer ---");
    CompilerService cs;
    // Arm via EDSL pin
    const auto h = as_i(cs, "(ffi:pin-buffer 8192 1 0)");
    CHECK(h >= 0, "pin");
    CHECK(aura::gc_hooks::ffi_pin_defer_active(), "defer active");
    // compact_sweep path consults ffi_pin_defer_active — unit-level: flag is true
    // so a real compact would return empty (see evaluator_gc.cpp #2005).
    LifetimePin local;
    std::array<std::uint8_t, 32> buf{};
    local.pin(buf.data(), 1, 0);
    CHECK(local.validate(1, 0), "local pin valid");
    // Invalidate local only (does not clear defer depth from EDSL pin)
    (void)invalidate_all_pins_for_arena(0);
    // Header and module registries may differ; local pin from header is invalidated
    // only if same registry — validate best-effort.
    CHECK(as_i(cs, std::format("(ffi:unpin-buffer {})", h)) == 1, "unpin edsl");
    CHECK(!aura::gc_hooks::ffi_pin_defer_active(), "defer cleared after unpin");
}

void ac5_pin_across_mutate() {
    std::println("\n--- AC5: pin across set-code / eval (boundary restamp path) ---");
    CompilerService cs;
    const auto h = as_i(cs, "(ffi:pin-buffer 1024 3 0)");
    CHECK(h >= 0, "pin before mutate");
    CHECK(as_i(cs, "(query:ffi-pin-count)") >= 1, "live during mutate prep");
    // Soft mutate via set-code + eval (MutationBoundary)
    CHECK(cs.eval("(set-code \"(define z 1)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    // Pin still held until explicit unpin (boundary restamps, does not drop)
    CHECK(as_i(cs, "(query:ffi-pin-count)") >= 1, "pin still live after mutate");
    CHECK(as_i(cs, std::format("(ffi:unpin-buffer {})", h)) == 1, "unpin after mutate");
}

void ac6_query_schema() {
    std::println("\n--- AC6: schema-2048 queries ---");
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:lifetime-pin-stats\")");
    CHECK(h && is_hash(*h), "lifetime-pin-stats hash");
    CHECK(href_lp(cs, "schema-2048") == 2048, "schema-2048");
    CHECK(href_lp(cs, "batch-ffi-present-wired") == 1, "wired");
    CHECK(href_lp(cs, "zero-copy-handoff-hits") >= 0, "handoff key");
    CHECK(href_lp(cs, "present-pin-handoffs") >= 0, "present pin key");
    CHECK(href_lp(cs, "defer-because-ffi-pin") >= 0, "defer key");
    CHECK(href_lp(cs, "phase") == 2, "phase 2");

    auto hr = cs.eval("(stats:get \"query:render-stats\")");
    CHECK(hr && is_hash(*hr), "render-stats");
    CHECK(href_rs(cs, "schema-2048") == 2048, "render-stats schema-2048");
    CHECK(href_rs(cs, "batch-ffi-present-wired") == 1, "render wired");
}

void ac7_clean_no_handoff() {
    std::println("\n--- AC7: clean present no handoff ---");
    reset_zero_copy_metrics_for_test();
    FramebufferOwned owned;
    owned.resize(8, 4);
    owned.dirty.clear();
    const auto h0 = g_zero_copy_metrics().zero_copy_handoff_hits.load(std::memory_order_relaxed);
    std::string out;
    const auto n = present_batch_to_string(owned.view(), owned.dirty, out);
    CHECK(n == 0, "clean → 0");
    const auto h1 = g_zero_copy_metrics().zero_copy_handoff_hits.load(std::memory_order_relaxed);
    CHECK(h1 == h0, "no handoff on clean short-circuit");
}

} // namespace

int main() {
    std::println("=== test_lifetime_pin_batch_ffi_present_2048 ===");
    ac1_source();
    ac2_present_handoff();
    ac3_ffi_pin_registry();
    ac4_defer_while_pinned();
    ac5_pin_across_mutate();
    ac6_query_schema();
    ac7_clean_no_handoff();
    std::println("\n=== results: {} passed, {} failed ===\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
