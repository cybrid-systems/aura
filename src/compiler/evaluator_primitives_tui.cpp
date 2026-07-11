// evaluator_primitives_tui.cpp — Issues #1331–#1343 Phase 1: 10 tui:* primitives
// Headless-safe wrapper over src/tui/tui_runtime.hh

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "security_capabilities.h"
#include "tui/tui_runtime.hh"
#include <cstdint>
#include <string>

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::as_int;
using types::as_string_idx;
using types::is_int;
using types::is_string;
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
        }
    }

} // namespace

void register_tui_primitives(PrimRegistrar add, Evaluator& ev) {
    // 1. (tui:init [title [cols [rows]]]) → #t/#f  — headless by default
    add("tui:init", [&ev](std::span<const EvalValue> a) -> EvalValue {
        std::string title = "aura-tui";
        int cols = 80, rows = 24;
        if (!a.empty() && is_string(a[0])) {
            auto i = as_string_idx(a[0]);
            if (i < ev.string_heap_.size())
                title = ev.string_heap_[i];
        }
        if (a.size() >= 2 && is_int(a[1]))
            cols = static_cast<int>(as_int(a[1]));
        if (a.size() >= 3 && is_int(a[2]))
            rows = static_cast<int>(as_int(a[2]));
        auto& tui = aura::tui::global_tui();
        if (tui.is_initialized())
            tui.shutdown();
        bool ok = tui.init(title, cols, rows, /*force_tty=*/false);
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
    add("tui:cell", [&ev](std::span<const EvalValue> a) -> EvalValue {
        if (a.size() < 3 || !is_int(a[0]) || !is_int(a[1]) || !is_string(a[2]))
            return make_bool(false);
        auto& tui = aura::tui::global_tui();
        if (!tui.is_initialized())
            return make_bool(false);
        auto sidx = as_string_idx(a[2]);
        if (sidx >= ev.string_heap_.size())
            return make_bool(false);
        auto ch = first_codepoint(ev.string_heap_[sidx]);
        auto fg =
            a.size() >= 4 && is_int(a[3]) ? static_cast<std::uint32_t>(as_int(a[3])) : 0xFFFFFFu;
        auto bg = a.size() >= 5 && is_int(a[4]) ? static_cast<std::uint32_t>(as_int(a[4])) : 0u;
        auto attr =
            a.size() >= 6 && is_int(a[5]) ? static_cast<std::uint8_t>(as_int(a[5]) & 0xFF) : 0u;
        bool ok = tui.put_cell(static_cast<int>(as_int(a[0])), static_cast<int>(as_int(a[1])), ch,
                               fg, bg, attr);
        bump_tui_metrics(ev);
        return make_bool(ok);
    });

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
    add("tui:present", [&ev](std::span<const EvalValue>) -> EvalValue {
        auto& tui = aura::tui::global_tui();
        if (tui.is_initialized())
            tui.present();
        bump_tui_metrics(ev);
        return make_void();
    });

    // 7. (tui:read-event [timeout-ms]) → event list | #f
    add("tui:read-event", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto& tui = aura::tui::global_tui();
        int timeout = 0;
        if (!a.empty() && is_int(a[0]))
            timeout = static_cast<int>(as_int(a[0]));
        auto ev_opt = tui.is_initialized() ? tui.read_event(timeout) : std::nullopt;
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
        return make_bool(false);
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
    add("tui:clear", [&ev](std::span<const EvalValue> a) -> EvalValue {
        auto& tui = aura::tui::global_tui();
        if (!tui.is_initialized())
            return make_bool(false);
        auto fg =
            a.size() >= 1 && is_int(a[0]) ? static_cast<std::uint32_t>(as_int(a[0])) : 0xFFFFFFu;
        auto bg = a.size() >= 2 && is_int(a[1]) ? static_cast<std::uint32_t>(as_int(a[1])) : 0u;
        tui.clear(fg, bg);
        return make_bool(true);
    });

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

    // Test helper: last frame ANSI string
    add("tui:frame-ansi", [&ev](std::span<const EvalValue>) -> EvalValue {
        auto& tui = aura::tui::global_tui();
        auto sidx = static_cast<std::uint64_t>(
            ev.push_string_heap(tui.is_initialized() ? tui.last_frame_ansi() : ""));
        return make_string(sidx);
    });

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
}

} // namespace aura::compiler::primitives_detail
