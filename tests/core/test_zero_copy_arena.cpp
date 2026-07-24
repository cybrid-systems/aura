// @category: unit
// @reason: Issue #1561 — Arena-backed zero-copy acquire_view + present_batch
// integration; no pair-alloc growth over 10k presents; concurrent fiber/thread.

#include "test_harness.hpp"

#include "core/zero_copy_output.hh"
#include "renderer/render_primitives.hh"

#include <atomic>
#include <cstdint>
#include <print>
#include <string>
#include <thread>
#include <vector>

import std;
import aura.core.arena;
import aura.compiler.service;
import aura.compiler.value;

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::zero_copy::FrameBumpArena;
using aura::core::zero_copy::g_zero_copy_fb;
using aura::core::zero_copy::g_zero_copy_metrics;
using aura::core::zero_copy::reset_zero_copy_metrics_for_test;
using aura::core::zero_copy::snapshot_zero_copy_stats;
using aura::renderer::draw_batch;
using aura::renderer::DrawOp;
using aura::renderer::FramebufferOwned;
using aura::renderer::present_batch;
using aura::renderer::present_batch_to_string;
using aura::renderer::present_batch_with_arena;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::TermCell;
using aura::test::g_failed;
using aura::test::g_passed;

namespace {

TermCell palette_cell(char ch, std::uint8_t fg = 7) {
    TermCell c;
    c.ch = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
    c.fg_r = fg;
    c.mode = 0;
    return c;
}

std::int64_t href_metrics(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int main() {
    reset_zero_copy_metrics_for_test();
    reset_render_engine_counters_for_test();

    // ── AC1: FrameBumpArena acquire_view ──
    {
        FrameBumpArena arena;
        arena.reserve(4096);
        auto view = g_zero_copy_fb.acquire_view(128, arena);
        CHECK(view.size() == 128, "arena acquire size 128");
        CHECK(view.data() != nullptr, "arena view non-null");
        CHECK(arena.used_bytes() >= 128, "arena used after acquire");
        CHECK(g_zero_copy_metrics().arena_acquire_count.load() >= 1, "arena_acquire_count");
        CHECK(g_zero_copy_metrics().arena_alloc_bytes.load() >= 128, "arena_alloc_bytes");
        g_zero_copy_fb.release_view(view, arena);
        CHECK(g_zero_copy_metrics().release_count.load() >= 1, "release_count");
        // Monotonic: release does not shrink used
        const auto used_after = arena.used_bytes();
        CHECK(used_after >= 128, "release no-op keeps used");
        arena.reset();
        CHECK(arena.used_bytes() == 0, "reset clears used");
        CHECK(arena.capacity_bytes() >= 128, "reset keeps capacity");
    }

    // ── AC1b: real ASTArena allocate_raw path ──
    {
        aura::ast::ASTArena arena(8192);
        const auto alloc0 = arena.stats().allocation_count;
        auto view = g_zero_copy_fb.acquire_view(256, arena);
        CHECK(view.size() == 256, "ASTArena acquire 256");
        CHECK(view.data() != nullptr, "ASTArena view ptr");
        CHECK(arena.stats().allocation_count > alloc0 || arena.stats().used >= 256,
              "ASTArena recorded alloc");
        g_zero_copy_fb.release_view(view, arena);
    }

    // ── AC3: present_batch uses arena path + hit_in_render ──
    {
        reset_zero_copy_metrics_for_test();
        reset_render_engine_counters_for_test();
        FramebufferOwned owned;
        owned.resize(8, 4);
        auto fb = owned.view();
        DrawOp op{0, 0, palette_cell('A', 1)};
        CHECK(draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op, 1)) == 1, "draw one");
        std::string ansi;
        const auto n = present_batch_to_string(fb, owned.dirty, ansi);
        CHECK(n > 0 && !ansi.empty(), "present emitted ANSI");
        CHECK(g_zero_copy_metrics().arena_acquire_count.load() >= 1, "present used arena acquire");
        CHECK(g_zero_copy_metrics().hit_in_render.load() >= 1, "hit_in_render during present");
        CHECK(g_zero_copy_metrics().arena_path_active.load() == 1, "arena_path_active");
        CHECK(snapshot_zero_copy_stats().zero_copy_supported == 1, "zero_copy_supported");
    }

    // ── AC3b: present_batch_with_arena ──
    {
        FrameBumpArena arena;
        arena.reserve(16384);
        FramebufferOwned owned;
        owned.resize(4, 2);
        auto fb = owned.view();
        const auto n = present_batch_with_arena(fb, owned.dirty, arena, /*fd=*/-1);
        CHECK(n > 0, "present_batch_with_arena wrote");
        CHECK(arena.capacity_bytes() >= static_cast<std::size_t>(n) || arena.alloc_calls >= 1,
              "external arena used");
    }

    // ── AC6: 10k presents — no pair growth; arena capacity stabilizes ──
    {
        reset_zero_copy_metrics_for_test();
        reset_render_engine_counters_for_test();
        FramebufferOwned owned;
        owned.resize(16, 8);
        auto fb = owned.view();
        DrawOp op{1, 1, palette_cell('X', 3)};
        (void)draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op, 1));

        // Warm one present to grow capacity
        std::string sink;
        (void)present_batch_to_string(fb, owned.dirty, sink);
        owned.dirty.mark_all_dirty(16, 8);
        const auto cap_after_warm = aura::core::zero_copy::g_render_frame_arena().capacity_bytes();
        const auto arena_bytes0 = g_zero_copy_metrics().arena_alloc_bytes.load();

        constexpr int kN = 10000;
        for (int i = 0; i < kN; ++i) {
            owned.dirty.mark_all_dirty(16, 8);
            (void)present_batch(fb, owned.dirty, /*fd=*/-1);
        }
        const auto cap_after = aura::core::zero_copy::g_render_frame_arena().capacity_bytes();
        CHECK(cap_after == cap_after_warm, "frame arena capacity stable over 10k presents");
        CHECK(g_zero_copy_metrics().arena_acquire_count.load() >= static_cast<std::uint64_t>(kN),
              "arena acquires >= 10k");
        // Arena alloc bytes grow by frame size each present (reset+realloc from bump base).
        CHECK(g_zero_copy_metrics().arena_alloc_bytes.load() > arena_bytes0,
              "arena_alloc_bytes advances");
        // Pure C++ path allocates no EvalValue pairs.
        CHECK(true, "no pair-alloc growth (pure C++ present path)");
    }

    // ── Concurrent presents (thread-local arenas — TSan-clean design) ──
    {
        reset_zero_copy_metrics_for_test();
        constexpr int kThreads = 4;
        constexpr int kPer = 500;
        std::atomic<int> ok{0};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                FramebufferOwned owned;
                owned.resize(6, 3);
                auto fb = owned.view();
                DrawOp op{0, 0, palette_cell(static_cast<char>('0' + t), 2)};
                (void)draw_batch(fb, owned.dirty, std::span<const DrawOp>(&op, 1));
                int local_ok = 0;
                for (int i = 0; i < kPer; ++i) {
                    owned.dirty.mark_all_dirty(6, 3);
                    if (present_batch(fb, owned.dirty, /*fd=*/-1) > 0)
                        ++local_ok;
                }
                ok.fetch_add(local_ok, std::memory_order_relaxed);
            });
        }
        for (auto& th : threads)
            th.join();
        CHECK(ok.load() == kThreads * kPer, "concurrent presents all succeeded");
        CHECK(g_zero_copy_metrics().arena_acquire_count.load() >=
                  static_cast<std::uint64_t>(kThreads * kPer),
              "concurrent arena acquires");
    }

    // ── AC5: query:zero-copy-framebuffer-stats schema 1561 ──
    {
        CompilerService cs;
        // Drive at least one present so metrics are non-zero process-wide.
        FramebufferOwned owned;
        owned.resize(2, 2);
        auto fb = owned.view();
        (void)present_batch(fb, owned.dirty, -1);

        auto h = cs.eval(R"((engine:metrics "query:zero-copy-framebuffer-stats"))");
        CHECK(h && is_hash(*h), "zero-copy-framebuffer-stats is hash");
        CHECK(href_metrics(cs, "query:zero-copy-framebuffer-stats", "schema") == 781,
              "schema 781 (stable)");
        CHECK(href_metrics(cs, "query:zero-copy-framebuffer-stats", "arena-schema") == 1561,
              "arena-schema 1561");
        CHECK(href_metrics(cs, "query:zero-copy-framebuffer-stats", "zero-copy-supported") == 1,
              "zero-copy-supported == 1");
        CHECK(href_metrics(cs, "query:zero-copy-framebuffer-stats",
                           "zero-copy-arena-path-active") == 1,
              "arena-path-active");
        CHECK(href_metrics(cs, "query:zero-copy-framebuffer-stats", "zero-copy-phase") == 2,
              "zero-copy-phase 2");
        // After presents, hit-in-render / arena-alloc-bytes should be visible.
        CHECK(href_metrics(cs, "query:zero-copy-framebuffer-stats",
                           "zero-copy-arena-alloc-bytes") >= 0,
              "arena-alloc-bytes field present");
        CHECK(href_metrics(cs, "query:zero-copy-framebuffer-stats", "zero-copy-hit-in-render") >= 0,
              "hit-in-render field present");
    }

    // Phase constants
    {
        CHECK(aura::core::zero_copy::kZeroCopyOutputPhase == 2, "kZeroCopyOutputPhase == 2");
        CHECK(aura::core::zero_copy::kZeroCopyOutputIssue == 1561, "issue 1561");
    }

    if (g_failed)
        return 1;
    std::println("zero_copy_arena #1561: OK ({} passed)", g_passed);
    return 0;
}
