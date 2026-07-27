// evaluator_primitives_tui.cpp — Issues #1331–#1343/#1353: tui:* primitives
// Headless-safe wrapper over src/tui/tui_runtime.hh + tui_input.hh (#1353 raw input)
//
// Issue #1967: tui:* is a commercial UI vertical (DOMAIN_STATUS deferred).
// Registration is gated by AURA_ENABLE_TUI (CMake option, default ON).
// Slim/core builds: -DAURA_ENABLE_TUI=OFF → register_tui_primitives is a no-op.
// See docs/tui.md + scripts/check_primitive_surface.py COMMERCIAL_DOMAIN_BUDGETS.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "render_prim_template.hh"
#include "security_capabilities.h"
#include "terminal_buffer_registry.hh"    // Issue #2134: TermBuf + DirtyRegion
#include "renderer/render_frame_arena.hh" // Issue #2214: LinearCellGrid active
#include "renderer/render_primitives.hh"
#include "renderer/batch_terminal.hh"
#include "renderer/render_strategy.hh" // #2138
#include "tui/tui_input.hh"
#include "tui/tui_runtime.hh"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

// Default ON when the TU is compiled outside the CMake graph (tools/IDE).
#ifndef AURA_ENABLE_TUI
#define AURA_ENABLE_TUI 1
#endif

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::as_bool;
using types::as_int;
using types::as_string_idx;
using types::as_vector_idx;
using types::is_bool;
using types::is_int;
using types::is_string;
using types::is_vector;
using types::make_bool;
using types::make_int;
using types::make_pair;
using types::make_string;
using types::make_void;

namespace {

    // Decode first Unicode codepoint from UTF-8 string (ASCII-fast path).
    std::uint32_t first_codepoint(const std::string& s) {
        if (s.empty())
            return static_cast<std::uint32_t>(' ');
        auto c0 = static_cast<unsigned char>(s[0]);
        if (c0 < 0x80)
            return c0;
        if ((c0 & 0xE0) == 0xC0 && s.size() >= 2)
            return (static_cast<std::uint32_t>(c0 & 0x1F) << 6) |
                   (static_cast<unsigned char>(s[1]) & 0x3F);
        if ((c0 & 0xF0) == 0xE0 && s.size() >= 3)
            return (static_cast<std::uint32_t>(c0 & 0x0F) << 12) |
                   ((static_cast<unsigned char>(s[1]) & 0x3F) << 6) |
                   (static_cast<unsigned char>(s[2]) & 0x3F);
        return c0;
    }

    std::string codepoint_to_utf8(std::uint32_t cp) {
        std::string out;
        if (cp <= 0x7F)
            out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back('?');
        }
        return out;
    }

    void bump_tui_metrics(Evaluator& ev) {
        if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
            m->tui_init_total.store(aura::tui::g_tui_init_total.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
            m->tui_present_total.store(
                aura::tui::g_tui_present_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_cell_writes.store(aura::tui::g_tui_cell_writes.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
            m->tui_diff_cells_emitted.store(
                aura::tui::g_tui_diff_cells_emitted.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_sync_output_frames.store(
                aura::tui::g_tui_sync_output_frames.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_half_block_pixels.store(
                aura::tui::g_tui_half_block_pixels.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_mouse_enable_total.store(
                aura::tui::g_tui_mouse_enable_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            // #1353 input metrics
            m->tui_raw_mode_on_total.store(
                aura::tui::g_tui_raw_mode_on_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_raw_mode_off_total.store(
                aura::tui::g_tui_raw_mode_off_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_poll_event_total.store(
                aura::tui::g_tui_poll_event_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_poll_event_hits.store(
                aura::tui::g_tui_poll_event_hits.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_key_events_total.store(
                aura::tui::g_tui_key_events_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_mouse_events_total.store(
                aura::tui::g_tui_mouse_events_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_quit_events_total.store(
                aura::tui::g_tui_quit_events_total.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            m->tui_input_active.store(1, std::memory_order_relaxed);
        }
    }

} // namespace

void register_tui_primitives(PrimRegistrar add, Evaluator& ev) {
#if !AURA_ENABLE_TUI
    // Issue #1967: commercial UI vertical disabled for this build.
    (void)add;
    (void)ev;
    return;
#else
    // 1. (tui:init [title [cols [rows [live?]]]]) → #t/#f
    //    Headless by default (CI-safe). live?=#t or AURA_TUI_LIVE=1 writes
    //    present() frames to stdout when it is a TTY.
    add("tui:init", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::string title = "aura-tui";
        int cols = 80, rows = 24;
        bool live = false;
        if (!a.empty() && is_string(a[0])) {
            auto i = as_string_idx(a[0]);
            if (i < ev.string_heap_.size())
                title = ev.string_heap_[i];
        }
        if (a.size() >= 2 && is_int(a[1]))
            cols = static_cast<int>(as_int(a[1]));
        if (a.size() >= 3 && is_int(a[2]))
            rows = static_cast<int>(as_int(a[2]));
        if (a.size() >= 4 && is_bool(a[3]))
            live = as_bool(a[3]);
        if (const char* env = std::getenv("AURA_TUI_LIVE")) {
            if (env[0] == '1' || env[0] == 'y' || env[0] == 'Y' || env[0] == 't' || env[0] == 'T')
                live = true;
        }
        auto& tui = aura::tui::global_tui();
        if (tui.is_initialized())
            tui.shutdown();
        bool ok = tui.init(title, cols, rows, /*force_tty=*/live);
        bump_tui_metrics(ev);
        return make_bool(ok);
    });

    // 2. (tui:shutdown)
    add("tui:shutdown", [&ev](std::span<const EvalValue>) -> EvalValue {
        aura::tui::global_tui().shutdown();
        bump_tui_metrics(ev);
        return make_void();
    });

    // 3. (tui:size) → (cols . rows)
    add("tui:size", [&ev](std::span<const EvalValue>) -> EvalValue {
        auto& tui = aura::tui::global_tui();
        int c = tui.is_initialized() ? tui.cols() : 80;
        int r = tui.is_initialized() ? tui.rows() : 24;
        auto pidx = ev.pairs_.size();
        ev.pairs_.push_back({make_int(c), make_int(r)});
        return make_pair(pidx);
    });

    // 4. (tui:cell col row char [fg [bg [attr]]])
    // Issue #1676/#1677/#2217: register_render_hot_prim + hot entry (draw half).
    register_render_hot_prim(
        add, ev, "tui:cell", 3,
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);
            if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_string(a[2]))
                return make_bool(false);
            auto& tui = aura::tui::global_tui();
            if (!tui.is_initialized())
                return make_bool(false);
            auto sidx = as_string_idx(a[2]);
            if (sidx >= ev.string_heap_.size())
                return make_bool(false);
            auto ch = first_codepoint(ev.string_heap_[sidx]);
            auto fg = a.size() >= 4 && is_int(a[3]) ? static_cast<std::uint32_t>(as_int(a[3]))
                                                    : 0xFFFFFFu;
            auto bg = a.size() >= 5 && is_int(a[4]) ? static_cast<std::uint32_t>(as_int(a[4])) : 0u;
            auto attr =
                a.size() >= 6 && is_int(a[5]) ? static_cast<std::uint8_t>(as_int(a[5]) & 0xFF) : 0u;
            bool ok = tui.put_cell(static_cast<int>(as_int(a[0])), static_cast<int>(as_int(a[1])),
                                   ch, fg, bg, attr);
            bump_tui_metrics(ev);
            return make_bool(ok);
        },
        "Write TUI cell (render-tier hot path, #1676).",
        "(int int string [int [int [int]]]) -> bool");

    // 5. (tui:get-cell col row) → (char . (fg . (bg . attr))) | #f
    add("tui:get-cell", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 2 || !is_int(a[0]) || !is_int(a[1]))
            return make_bool(false);
        auto& tui = aura::tui::global_tui();
        if (!tui.is_initialized())
            return make_bool(false);
        auto cell = tui.get_cell(static_cast<int>(as_int(a[0])), static_cast<int>(as_int(a[1])));
        auto cs = codepoint_to_utf8(cell.ch);
        auto cidx = static_cast<std::uint64_t>(ev.push_string_heap(cs));
        auto p3 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(cell.bg), make_int(cell.attr)});
        auto p2 = ev.pairs_.size();
        ev.pairs_.push_back({make_int(cell.fg), make_pair(p3)});
        auto p1 = ev.pairs_.size();
        ev.pairs_.push_back({make_string(cidx), make_pair(p2)});
        return make_pair(p1);
    });

    // 6. (tui:present)
    // Issue #1676/#1677/#2217: register_render_hot_prim + hot entry.
    register_render_hot_prim(
        add, ev, "tui:present", 0,
        [&ev](std::span<const EvalValue>) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);
            auto& tui = aura::tui::global_tui();
            if (tui.is_initialized()) {
                tui.present();
                // Issue #1674: wire term_render_present (was dead).
                ev.bump_term_render_present();
            }
            bump_tui_metrics(ev);
            return make_void();
        },
        "Present TUI framebuffer (linear/epoch fenced, #1676).", "() -> void");

    // 7. (tui:read-event [timeout-ms]) → (tag . payload) | #f
    // #1353: works with raw mode / inject-bytes even without tui:init (input path).
    add("tui:read-event", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto& tui = aura::tui::global_tui();
        int timeout = 0;
        if (!a.empty() && is_int(a[0]))
            timeout = static_cast<int>(as_int(a[0]));
        // Prefer TUIRuntime queue when initialized; otherwise poll global input directly.
        std::optional<aura::tui::Event> ev_opt;
        if (tui.is_initialized())
            ev_opt = tui.read_event(timeout);
        else {
            auto ie = aura::tui::global_tui_input().poll_event(timeout);
            if (ie) {
                aura::tui::Event e;
                using K = aura::tui::InputEvent::Kind;
                if (ie->kind == K::Key) {
                    e.type = aura::tui::Event::Type::Key;
                    e.key = ie->ch;
                } else if (ie->kind == K::Quit) {
                    e.type = aura::tui::Event::Type::Quit;
                } else if (ie->kind == K::Mouse) {
                    e.type = aura::tui::Event::Type::Mouse;
                    e.mouse_button = ie->btn;
                    e.mouse_x = ie->col;
                    e.mouse_y = ie->row;
                } else {
                    bump_tui_metrics(ev);
                    return make_bool(false);
                }
                ev_opt = e;
            }
        }
        bump_tui_metrics(ev);
        if (!ev_opt)
            return make_bool(false);
        using T = aura::tui::Event::Type;
        if (ev_opt->type == T::Quit) {
            auto sidx = static_cast<std::uint64_t>(ev.push_string_heap("quit"));
            auto p = ev.pairs_.size();
            ev.pairs_.push_back({make_string(sidx), make_void()});
            return make_pair(p);
        }
        if (ev_opt->type == T::Key) {
            auto ks = codepoint_to_utf8(ev_opt->key);
            auto kidx = static_cast<std::uint64_t>(ev.push_string_heap(ks));
            auto tidx = static_cast<std::uint64_t>(ev.push_string_heap("key"));
            auto p = ev.pairs_.size();
            ev.pairs_.push_back({make_string(tidx), make_string(kidx)});
            return make_pair(p);
        }
        if (ev_opt->type == T::Mouse) {
            auto tidx = static_cast<std::uint64_t>(ev.push_string_heap("mouse"));
            auto p2 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(ev_opt->mouse_x), make_int(ev_opt->mouse_y)});
            auto p1 = ev.pairs_.size();
            ev.pairs_.push_back({make_int(ev_opt->mouse_button), make_pair(p2)});
            auto p0 = ev.pairs_.size();
            ev.pairs_.push_back({make_string(tidx), make_pair(p1)});
            return make_pair(p0);
        }
        return make_bool(false);
    });

    // #1353: (tui:raw-mode-on) → #t/#f  — enable raw mode (idempotent)
    add("tui:raw-mode-on", [&ev](std::span<const EvalValue>) -> EvalValue {
        bool ok = aura::tui::global_tui_input().enable_raw_mode();
        bump_tui_metrics(ev);
        return make_bool(ok);
    });

    // #1353: (tui:raw-mode-off) → #t
    add("tui:raw-mode-off", [&ev](std::span<const EvalValue>) -> EvalValue {
        bool ok = aura::tui::global_tui_input().disable_raw_mode();
        bump_tui_metrics(ev);
        return make_bool(ok);
    });

    // #1353: (tui:is-raw-mode) → #t/#f
    add("tui:is-raw-mode", [](std::span<const EvalValue>) -> EvalValue {
        return make_bool(aura::tui::global_tui_input().is_raw_mode());
    });

    // #1353: (tui:terminal-size) → (rows . cols) via TIOCGWINSZ
    add("tui:terminal-size", [&ev](std::span<const EvalValue>) -> EvalValue {
        auto [rows, cols] = aura::tui::global_tui_input().terminal_size();
        auto pidx = ev.pairs_.size();
        ev.pairs_.push_back({make_int(rows), make_int(cols)});
        return make_pair(pidx);
    });

    // #1353: (tui:enable-mouse) / alias of (tui:mouse 1) with SGR emit
    add("tui:enable-mouse", [&ev](std::span<const EvalValue>) -> EvalValue {
        aura::tui::global_tui().set_mouse_enabled(true);
        bump_tui_metrics(ev);
        return make_bool(true);
    });

    // #1353: (tui:inject-bytes "...") — feed raw CSI/UTF-8 for headless tests
    add("tui:inject-bytes", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto i = as_string_idx(a[0]);
        if (i >= ev.string_heap_.size())
            return make_bool(false);
        aura::tui::global_tui_input().inject_bytes(ev.string_heap_[i]);
        return make_bool(true);
    });

    // 8. (tui:hide-cursor)
    add("tui:hide-cursor", [](std::span<const EvalValue>) -> EvalValue {
        aura::tui::global_tui().hide_cursor();
        return make_void();
    });

    // 9. (tui:show-cursor)
    add("tui:show-cursor", [](std::span<const EvalValue>) -> EvalValue {
        aura::tui::global_tui().show_cursor();
        return make_void();
    });

    // 10. (tui:set-title title)
    add("tui:set-title", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_void();
        auto i = as_string_idx(a[0]);
        if (i < ev.string_heap_.size())
            aura::tui::global_tui().set_title(ev.string_heap_[i]);
        return make_void();
    });

    // Extras for demos / opts / tests (still Phase 1 surface)
    // Issue #1676/#2217: hot-tier clear (body opens with AURA_RENDER_HOT_ENTRY).
    register_render_hot_prim(
        add, ev, "tui:clear", 0,
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);
            auto& tui = aura::tui::global_tui();
            if (!tui.is_initialized())
                return make_bool(false);
            auto fg = a.size() >= 1 && is_int(a[0]) ? static_cast<std::uint32_t>(as_int(a[0]))
                                                    : 0xFFFFFFu;
            auto bg = a.size() >= 2 && is_int(a[1]) ? static_cast<std::uint32_t>(as_int(a[1])) : 0u;
            tui.clear(fg, bg);
            return make_bool(true);
        },
        "Clear TUI framebuffer (#1676 render-tier).", "() -> void");

    // #1342: half-block pixel
    add("tui:pixel", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 4 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]))
            return make_bool(false);
        auto& tui = aura::tui::global_tui();
        if (!tui.is_initialized())
            return make_bool(false);
        bool ok = tui.put_half_block_pixel(
            static_cast<int>(as_int(a[0])), static_cast<int>(as_int(a[1])),
            static_cast<std::uint32_t>(as_int(a[2])), static_cast<std::uint32_t>(as_int(a[3])));
        bump_tui_metrics(ev);
        return make_bool(ok);
    });

    // #1343: mouse enable (stub tracking)
    add("tui:mouse", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_int(a[0]))
            return make_bool(false);
        auto& tui = aura::tui::global_tui();
        tui.set_mouse_enabled(as_int(a[0]) != 0);
        bump_tui_metrics(ev);
        return make_bool(true);
    });

    // Test helper: last frame ANSI string (#1676/#2217 render-tier).
    register_render_hot_prim(
        add, ev, "tui:frame-ansi", 0,
        [&ev](std::span<const EvalValue>) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);
            auto& tui = aura::tui::global_tui();
            auto sidx = static_cast<std::uint64_t>(
                ev.push_string_heap(tui.is_initialized() ? tui.last_frame_ansi() : ""));
            return make_string(sidx);
        },
        "Last TUI frame as ANSI string (#1676 render-tier).", "() -> string");

    // Test helper: inject key for headless event loop
    add("tui:inject-key", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.empty() || !is_string(a[0]))
            return make_bool(false);
        auto i = as_string_idx(a[0]);
        if (i >= ev.string_heap_.size())
            return make_bool(false);
        aura::tui::global_tui().inject_key(first_codepoint(ev.string_heap_[i]));
        return make_bool(true);
    });

    // ═══════════════════════════════════════════════════════════════════
    // Issue #2134: Agent batch draw + dirty AABB present over TermBuf.
    // Preferred full-frame path vs O(cells) tui:cell loops. Uses the same
    // registry as make-terminal-buffer / terminal-present-batch.
    // Issue #2217: register via register_render_hot_prim (no ad-hoc set_meta).
    // ═══════════════════════════════════════════════════════════════════
    using term_registry::s_term_bufs;
    using term_registry::s_term_registry_mtx;
    using TermCell = aura::renderer::TermCell;

    // (tui:draw-batch buf-id x y ch [fg [bg]]) → cells-written
    // (tui:draw-batch buf-id packed-vector) → cells-written
    //   packed-vector flat ints: x y ch fg bg  (stride 5, fg/bg optional as 0)
    register_render_hot_prim(
        add, ev, "tui:draw-batch", 2,
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);
            if (a.empty() || !is_int(a[0]))
                return make_int(-1);
            const auto id = as_int(a[0]);

            std::vector<aura::renderer::DrawOp> ops;
            ops.reserve(8);
            auto push_op = [&](std::int64_t x, std::int64_t y, std::int64_t ch, std::int64_t fg,
                               std::int64_t bg) {
                if (x < 0 || y < 0)
                    return;
                aura::renderer::DrawOp op;
                op.x = static_cast<std::uint32_t>(x);
                op.y = static_cast<std::uint32_t>(y);
                op.cell.ch = static_cast<std::uint32_t>(ch > 0x10FFFF ? ' ' : ch);
                op.cell.fg_r = static_cast<std::uint8_t>(fg & 0xFF);
                op.cell.bg_r = static_cast<std::uint8_t>(bg & 0xFF);
                op.cell.mode = 0;
                ops.push_back(op);
            };

            if (a.size() >= 2 && is_vector(a[1])) {
                // Packed vector path: [x y ch fg bg]*
                auto vidx = as_vector_idx(a[1]);
                if (vidx >= ev.vector_heap_.size())
                    return make_int(-1);
                const auto& vec = ev.vector_heap_[vidx];
                for (std::size_t i = 0; i + 2 < vec.size();) {
                    if (!is_int(vec[i]) || !is_int(vec[i + 1]) || !is_int(vec[i + 2]))
                        break;
                    std::int64_t fg = 7, bg = 0;
                    if (i + 3 < vec.size() && is_int(vec[i + 3]))
                        fg = as_int(vec[i + 3]);
                    if (i + 4 < vec.size() && is_int(vec[i + 4]))
                        bg = as_int(vec[i + 4]);
                    push_op(as_int(vec[i]), as_int(vec[i + 1]), as_int(vec[i + 2]), fg, bg);
                    // stride 5 when colors present, else 3
                    if (i + 4 < vec.size() && is_int(vec[i + 3]) && is_int(vec[i + 4]))
                        i += 5;
                    else
                        i += 3;
                }
            } else if (a.size() >= 4 && is_int(a[1]) && is_int(a[2]) && is_int(a[3])) {
                // Single-cell: (buf x y ch [fg [bg]])
                const auto fg = a.size() >= 5 && is_int(a[4]) ? as_int(a[4]) : 7;
                const auto bg = a.size() >= 6 && is_int(a[5]) ? as_int(a[5]) : 0;
                push_op(as_int(a[1]), as_int(a[2]), as_int(a[3]), fg, bg);
            } else {
                return make_int(-1);
            }

            std::int64_t written = -1;
            {
                std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                    !s_term_bufs[static_cast<std::size_t>(id)])
                    return make_int(-1);
                auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
                std::unique_lock<std::shared_mutex> buf(b.rwlock);
                aura::renderer::FramebufferSoA fb{b.w, b.h, b.cells.data()};
                written = aura::renderer::draw_batch(fb, b.dirty, ops);
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->tui_draw_batch_total.fetch_add(1, std::memory_order_relaxed);
                if (written > 0)
                    m->tui_draw_batch_cells_written.fetch_add(static_cast<std::uint64_t>(written),
                                                              std::memory_order_relaxed);
                m->term_render_draw_batch_total.fetch_add(1, std::memory_order_relaxed);
            }
            bump_tui_metrics(ev);
            return make_int(written);
        },
        "Batch write TermBuf cells + expand DirtyRegion (#2134).",
        "(int int int int [int [int]]) | (int vector) -> int");

    // (tui:fill-rect buf-id x y w h ch [fg [bg]]) → cells-written
    register_render_hot_prim(
        add, ev, "tui:fill-rect", 6,
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);
            if (a.size() < 6 || !is_int(a[0]) || !is_int(a[1]) || !is_int(a[2]) || !is_int(a[3]) ||
                !is_int(a[4]) || !is_int(a[5]))
                return make_int(-1);
            const auto id = as_int(a[0]);
            const auto x0 = as_int(a[1]);
            const auto y0 = as_int(a[2]);
            const auto ww = as_int(a[3]);
            const auto hh = as_int(a[4]);
            auto ch = as_int(a[5]);
            if (ch < 0 || ch > 0x10FFFF)
                ch = ' ';
            const auto fg = a.size() >= 7 && is_int(a[6]) ? as_int(a[6]) : 7;
            const auto bg = a.size() >= 8 && is_int(a[7]) ? as_int(a[7]) : 0;
            if (ww <= 0 || hh <= 0)
                return make_int(0);

            TermCell cell = TermCell::space_palette();
            cell.ch = static_cast<std::uint32_t>(ch);
            cell.fg_r = static_cast<std::uint8_t>(fg & 0xFF);
            cell.bg_r = static_cast<std::uint8_t>(bg & 0xFF);

            std::int64_t written = 0;
            {
                std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                    !s_term_bufs[static_cast<std::size_t>(id)])
                    return make_int(-1);
                auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
                std::unique_lock<std::shared_mutex> buf(b.rwlock);
                for (std::int64_t dy = 0; dy < hh; ++dy) {
                    const auto y = y0 + dy;
                    if (y < 0 || y >= b.h)
                        continue;
                    for (std::int64_t dx = 0; dx < ww; ++dx) {
                        const auto x = x0 + dx;
                        if (x < 0 || x >= b.w)
                            continue;
                        b.cells[static_cast<std::size_t>(y * b.w + x)] = cell;
                        b.dirty.mark_dirty(static_cast<std::uint32_t>(x),
                                           static_cast<std::uint32_t>(y));
                        ++written;
                    }
                }
            }
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->tui_fill_rect_total.fetch_add(1, std::memory_order_relaxed);
                if (written > 0)
                    m->tui_fill_rect_cells_written.fetch_add(static_cast<std::uint64_t>(written),
                                                             std::memory_order_relaxed);
            }
            bump_tui_metrics(ev);
            return make_int(written);
        },
        "Fill rectangle in TermBuf + expand DirtyRegion (#2134).",
        "(int int int int int int [int [int]]) -> int");

    // (tui:present-batch buf-id [fd]) → bytes-written (0 = clean skip)
    // Issue #2134/#2135: Dirty AABB via present_batch; default FrameBumpArena /
    // RenderFrameArena path with direct-to-arena ANSI when capacity allows.
    register_render_hot_prim(
        add, ev, "tui:present-batch", 1,
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);
            if (a.empty() || !is_int(a[0]))
                return make_int(-1);
            const auto id = as_int(a[0]);
            int fd = 1;
            if (a.size() >= 2 && is_int(a[1]))
                fd = static_cast<int>(as_int(a[1]));

            std::int64_t n = -1;
            std::uint64_t dirty_cells = 0;
            std::uint64_t skips_before = aura::renderer::render_engine_counters().present_skips;
            const auto t0 = std::chrono::steady_clock::now();
            {
                std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
                if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                    !s_term_bufs[static_cast<std::size_t>(id)])
                    return make_int(-1);
                auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
                std::unique_lock<std::shared_mutex> buf(b.rwlock);
                dirty_cells = b.dirty.cell_count();
                aura::renderer::FramebufferSoA fb{b.w, b.h, b.cells.data()};
                // Issue #2138: consult evolvable RenderStrategy (kernel stays stable).
                n = aura::renderer::strategy::present_batch_with_strategy(fb, b.dirty, fd);
            }
            const auto us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - t0)
                                               .count());
            const bool skipped =
                aura::renderer::render_engine_counters().present_skips > skips_before || n == 0;
            if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                m->tui_present_batch_total.fetch_add(1, std::memory_order_relaxed);
                m->tui_present_batch_us_total.fetch_add(us, std::memory_order_relaxed);
                m->terminal_present_batch_total.fetch_add(1, std::memory_order_relaxed);
                if (skipped) {
                    m->tui_present_batch_skip_clean.fetch_add(1, std::memory_order_relaxed);
                } else {
                    m->tui_present_batch_dirty_cells.fetch_add(dirty_cells,
                                                               std::memory_order_relaxed);
                    if (n > 0)
                        m->terminal_present_bytes_total.fetch_add(static_cast<std::uint64_t>(n),
                                                                  std::memory_order_relaxed);
                }
                m->tui_present_total.fetch_add(1, std::memory_order_relaxed);
            }
            bump_tui_metrics(ev);
            return make_int(n);
        },
        "Present TermBuf dirty AABB only (#2134); clean short-circuits to 0.",
        "(int [int]) -> int");

    // ═══════════════════════════════════════════════════════════════════
    // Issue #2214: (tui:present-dirty) / (tui:present-dirty x0 y0 x1 y1)
    // Differential zero-copy present — Agent-facing default hot path for
    // sparse mutation (5–20% cells). Uses LinearCellGrid.dirty when active,
    // else TUIRuntime per-cell dirty AABB, else optional TermBuf id.
    // Clean dirty region short-circuits (parity #2047). Full-frame remains
    // via tui:present. Prefer present-dirty after soft dirty / set-body on
    // evolution-named defines (see render_prim_template.hh #2051/#2214).
    // ═══════════════════════════════════════════════════════════════════
    // Signatures:
    //   (tui:present-dirty)                    → use current dirty AABB
    //   (tui:present-dirty x0 y0 x1 y1)        → explicit inclusive AABB
    //   (tui:present-dirty buf-id [fd])        → TermBuf dirty (batch)
    //   (tui:present-dirty buf-id x0 y0 x1 y1) → TermBuf + explicit AABB
    // Returns bytes written (0 = short-circuit, -1 = error).
    // Issue #2217: register_render_hot_prim (no ad-hoc set_meta).
    register_render_hot_prim(
        add, ev, "tui:present-dirty", 0,
        [&ev](std::span<const EvalValue> a) -> EvalValue {
            AURA_RENDER_HOT_ENTRY(ev);

            auto note_metrics = [&](std::int64_t n, bool skipped, std::uint64_t cells,
                                    bool partial) {
                if (auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics())) {
                    m->tui_present_dirty_total.fetch_add(1, std::memory_order_relaxed);
                    m->tui_present_total.fetch_add(1, std::memory_order_relaxed);
                    if (skipped) {
                        m->tui_present_dirty_short_circuit.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        if (partial)
                            m->tui_present_dirty_partial_total.fetch_add(1,
                                                                         std::memory_order_relaxed);
                        m->tui_present_dirty_cells_emitted.fetch_add(cells,
                                                                     std::memory_order_relaxed);
                        if (n > 0)
                            m->tui_present_dirty_bytes_total.fetch_add(
                                static_cast<std::uint64_t>(n), std::memory_order_relaxed);
                    }
                }
                bump_tui_metrics(ev);
            };

            // Parse optional TermBuf id / AABB / fd.
            std::optional<std::int64_t> buf_id;
            std::optional<std::array<std::uint32_t, 4>> explicit_aabb;
            int fd = -1; // default: to-string / headless (no TTY write)

            auto as_u32 = [](const EvalValue& v) -> std::uint32_t {
                return static_cast<std::uint32_t>(std::max<std::int64_t>(0, as_int(v)));
            };

            if (a.size() == 1 && is_int(a[0])) {
                // TermBuf id only — default fd=-1 (build ANSI, no TTY write; headless-safe).
                buf_id = as_int(a[0]);
                fd = -1;
            } else if (a.size() == 2 && is_int(a[0]) && is_int(a[1])) {
                buf_id = as_int(a[0]);
                fd = static_cast<int>(as_int(a[1]));
            } else if (a.size() == 4 && is_int(a[0]) && is_int(a[1]) && is_int(a[2]) &&
                       is_int(a[3])) {
                explicit_aabb = std::array<std::uint32_t, 4>{as_u32(a[0]), as_u32(a[1]),
                                                             as_u32(a[2]), as_u32(a[3])};
            } else if (a.size() == 5 && is_int(a[0]) && is_int(a[1]) && is_int(a[2]) &&
                       is_int(a[3]) && is_int(a[4])) {
                // buf-id + AABB, no fd → headless build
                buf_id = as_int(a[0]);
                explicit_aabb = std::array<std::uint32_t, 4>{as_u32(a[1]), as_u32(a[2]),
                                                             as_u32(a[3]), as_u32(a[4])};
                fd = -1;
            } else if (!a.empty()) {
                return make_int(-1);
            }

            // ── Path 1: TermBuf by id ──
            if (buf_id) {
                const auto id = *buf_id;
                std::int64_t n = -1;
                std::uint64_t dirty_cells = 0;
                bool partial = false;
                std::uint64_t skips_before = aura::renderer::render_engine_counters().present_skips;
                {
                    std::shared_lock<std::shared_mutex> reg(s_term_registry_mtx);
                    if (id < 0 || static_cast<std::size_t>(id) >= s_term_bufs.size() ||
                        !s_term_bufs[static_cast<std::size_t>(id)])
                        return make_int(-1);
                    auto& b = *s_term_bufs[static_cast<std::size_t>(id)];
                    std::unique_lock<std::shared_mutex> buf(b.rwlock);
                    if (explicit_aabb) {
                        b.dirty.clean = false;
                        b.dirty.empty_aabb = false;
                        b.dirty.x0 = (*explicit_aabb)[0];
                        b.dirty.y0 = (*explicit_aabb)[1];
                        b.dirty.x1 = (*explicit_aabb)[2];
                        b.dirty.y1 = (*explicit_aabb)[3];
                        (void)b.dirty.clamp_to(static_cast<std::uint32_t>(b.w),
                                               static_cast<std::uint32_t>(b.h));
                    }
                    dirty_cells = b.dirty.cell_count();
                    partial = b.dirty.is_dirty() &&
                              dirty_cells <
                                  static_cast<std::uint64_t>(b.w) * static_cast<std::uint64_t>(b.h);
                    aura::renderer::FramebufferSoA fb{b.w, b.h, b.cells.data()};
                    n = aura::renderer::present_batch(fb, b.dirty, fd);
                }
                const bool skipped =
                    aura::renderer::render_engine_counters().present_skips > skips_before || n == 0;
                note_metrics(n, skipped, dirty_cells, partial && !skipped);
                return make_int(n);
            }

            // ── Path 2: active LinearCellGrid (#2214 AC3) ──
            if (auto* grid = aura::renderer::active_linear_cell_grid(); grid && grid->valid()) {
                if (explicit_aabb) {
                    grid->dirty.clean = false;
                    grid->dirty.empty_aabb = false;
                    grid->dirty.x0 = (*explicit_aabb)[0];
                    grid->dirty.y0 = (*explicit_aabb)[1];
                    grid->dirty.x1 = (*explicit_aabb)[2];
                    grid->dirty.y1 = (*explicit_aabb)[3];
                    (void)grid->dirty.clamp_to(static_cast<std::uint32_t>(grid->width),
                                               static_cast<std::uint32_t>(grid->height));
                }
                const auto dirty_cells = grid->dirty.cell_count();
                const bool partial = grid->dirty.is_dirty() &&
                                     dirty_cells < static_cast<std::uint64_t>(grid->width) *
                                                       static_cast<std::uint64_t>(grid->height);
                std::uint64_t skips_before = aura::renderer::render_engine_counters().present_skips;
                auto fb = grid->view();
                // fd=-1: present_batch still builds ANSI + notes metrics; no TTY write.
                const auto n = aura::renderer::present_batch(fb, grid->dirty, /*fd=*/-1);
                // present_batch clears dirty on success (including short-circuit).
                const bool skipped =
                    aura::renderer::render_engine_counters().present_skips > skips_before || n == 0;
                note_metrics(n, skipped, dirty_cells, partial && !skipped);
                return make_int(n);
            }

            // ── Path 3: TUIRuntime front buffer (per-cell dirty → AABB) ──
            auto& tui = aura::tui::global_tui();
            if (!tui.is_initialized()) {
                note_metrics(0, /*skipped=*/true, 0, false);
                return make_int(0);
            }
            const int w = tui.cols();
            const int h = tui.rows();
            if (w <= 0 || h <= 0)
                return make_int(-1);

            // Convert TUI cells → TermCell + DirtyRegion.
            thread_local std::vector<aura::renderer::TermCell> term_cells;
            term_cells.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
            aura::renderer::DirtyRegion dirty{};
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    const auto c = tui.get_cell(x, y);
                    auto& tc = term_cells[static_cast<std::size_t>(y * w + x)];
                    tc.ch = c.ch;
                    tc.mode = 1; // RGB
                    tc.fg_r = static_cast<std::uint8_t>((c.fg >> 16) & 0xFF);
                    tc.fg_g = static_cast<std::uint8_t>((c.fg >> 8) & 0xFF);
                    tc.fg_b = static_cast<std::uint8_t>(c.fg & 0xFF);
                    tc.bg_r = static_cast<std::uint8_t>((c.bg >> 16) & 0xFF);
                    tc.bg_g = static_cast<std::uint8_t>((c.bg >> 8) & 0xFF);
                    tc.bg_b = static_cast<std::uint8_t>(c.bg & 0xFF);
                    if (c.dirty)
                        dirty.mark_dirty(static_cast<std::uint32_t>(x),
                                         static_cast<std::uint32_t>(y));
                }
            }
            if (explicit_aabb) {
                dirty.clean = false;
                dirty.empty_aabb = false;
                dirty.x0 = (*explicit_aabb)[0];
                dirty.y0 = (*explicit_aabb)[1];
                dirty.x1 = (*explicit_aabb)[2];
                dirty.y1 = (*explicit_aabb)[3];
                (void)dirty.clamp_to(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
            }

            if (dirty.is_clean()) {
                aura::renderer::note_batch_terminal_short_circuit();
                note_metrics(0, /*skipped=*/true, 0, false);
                return make_int(0);
            }

            const auto dirty_cells = dirty.cell_count();
            const bool partial =
                dirty_cells < static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h);
            aura::renderer::FramebufferSoA fb{w, h, term_cells.data()};
            std::string ansi_out;
            const auto n = aura::renderer::present_batch_to_string(fb, dirty, ansi_out);
            // Clear TUI per-cell dirty after successful emit (dirty consumed).
            if (n >= 0)
                tui.clear_dirty_flags();
            const bool skipped = n == 0;
            note_metrics(n, skipped, dirty_cells, partial && !skipped);
            (void)ansi_out;
            return make_int(n);
        },
        "Differential dirty AABB present (#2214); clean short-circuits. Prefer after "
        "sparse cell/draw mutations. LinearCellGrid / TermBuf / TUIRuntime.",
        "() | (int int int int) | (int [int]) | (int int int int int) -> int");
#endif // AURA_ENABLE_TUI
}

} // namespace aura::compiler::primitives_detail
