// evaluator_primitives_render3d.cpp — Issue #1986 / Epic #1979
// render3d:* Aura surface over the software voxel engine (#1980–#1985).
//
// Gated by AURA_ENABLE_TUI (same commercial UI vertical as tui:*).
// Domain prefix render3d: is deferred + budgeted in check_primitive_surface.py.
//
// Opaque volume handles are 1-based ints; no raw pointers to Aura.

module;

#include "runtime_shared.h"
#include "observability_metrics.h"
#include "primitives_detail.h"
#include "renderer/render3d_runtime.hh"
#include "renderer/voxel_frame.hh"
#include "renderer/voxel_shade.hh"

#include <cstdint>
#include <mutex>
#include <string>

#ifndef AURA_ENABLE_TUI
#define AURA_ENABLE_TUI 1
#endif

module aura.compiler.evaluator;

import std;
import aura.compiler.value;

namespace aura::compiler::primitives_detail {

using EvalValue = types::EvalValue;
using PrimRegistrar = std::function<void(std::string, PrimFn)>;

using types::as_int;
using types::is_int;
using types::make_bool;
using types::make_hash;
using types::make_int;
using types::make_string;
using types::make_void;

namespace {

    using aura::renderer::build_demo_scene;
    using aura::renderer::default_materials;
    using aura::renderer::FrameStats;
    using aura::renderer::global_render3d;
    using aura::renderer::kDefaultMaterialCount;
    using aura::renderer::render_frame;
    using aura::renderer::RenderFrameOptions;

    std::int64_t arg_i(std::span<const EvalValue> a, std::size_t i, std::int64_t def = 0) {
        if (i < a.size() && is_int(a[i]))
            return as_int(a[i]);
        return def;
    }

    EvalValue make_stats_hash(Evaluator& ev, const FrameStats& st) {
        auto* ht = FlatHashTable::create(16);
        if (!ht)
            return make_void();
        auto meta = ht->metadata();
        auto keys = ht->keys();
        auto vals = ht->values();
        auto hcap = ht->capacity;
        auto insert_kv = [&](const char* k_str, std::int64_t v) {
            std::uint64_t h = ::aura::compiler::stats::kFnvOffsetBasis;
            for (const char* p = k_str; *p; ++p)
                h = (h ^ static_cast<std::uint8_t>(*p)) * ::aura::compiler::stats::kFnvPrime;
            auto fp = static_cast<std::uint8_t>((h >> 57) & 0x7F) | 0x80;
            if (fp == 0xFF)
                fp = 0xFE;
            for (std::size_t at = 0; at < hcap; ++at) {
                auto idx = ((h >> 1) + at) & (hcap - 1);
                if (meta[idx] == 0xFF) {
                    meta[idx] = fp;
                    const auto kidx = static_cast<std::uint64_t>(ev.push_string_heap(k_str));
                    keys[idx] = make_string(kidx).val;
                    vals[idx] = make_int(v).val;
                    ht->size++;
                    return;
                }
            }
        };
        insert_kv("schema", 1986);
        insert_kv("issue", 1986);
        insert_kv("pixels", static_cast<std::int64_t>(st.pixels));
        insert_kv("hits", static_cast<std::int64_t>(st.hits));
        insert_kv("misses", static_cast<std::int64_t>(st.misses));
        insert_kv("rays", static_cast<std::int64_t>(st.rays));
        insert_kv("present-bytes", st.present_bytes);
        insert_kv("elapsed-us", static_cast<std::int64_t>(st.elapsed_ms * 1000.0));
        insert_kv("fps-x100", static_cast<std::int64_t>(st.fps() * 100.0));
        auto hidx = g_hash_tables.size();
        g_hash_tables.push_back(ht);
        return make_hash(hidx);
    }

} // namespace

void register_render3d_primitives(PrimRegistrar add, Evaluator& ev) {
#if !AURA_ENABLE_TUI
    (void)add;
    (void)ev;
    return;
#else
    // 1. (render3d:create-volume sx sy sz) → vol-id (1-based) or 0
    add("render3d:create-volume", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        const int sx = static_cast<int>(arg_i(a, 0, 32));
        const int sy = static_cast<int>(arg_i(a, 1, 16));
        const int sz = static_cast<int>(arg_i(a, 2, 32));
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        return make_int(st.create_volume(sx, sy, sz));
    });

    // 2. (render3d:destroy-volume id) → #t/#f
    add("render3d:destroy-volume", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        const int id = static_cast<int>(arg_i(a, 0, 0));
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        return make_bool(st.destroy_volume(id));
    });

    // 3. (render3d:set-block vol x y z block-id) → #t/#f
    add("render3d:set-block", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        const int id = static_cast<int>(arg_i(a, 0, 0));
        const int x = static_cast<int>(arg_i(a, 1, 0));
        const int y = static_cast<int>(arg_i(a, 2, 0));
        const int z = static_cast<int>(arg_i(a, 3, 0));
        const auto bid = static_cast<aura::renderer::BlockId>(arg_i(a, 4, 1));
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        auto* vol = st.volume(id);
        if (!vol)
            return make_bool(false);
        return make_bool(vol->set(x, y, z, bid));
    });

    // 4. (render3d:get-block vol x y z) → block-id
    add("render3d:get-block", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        const int id = static_cast<int>(arg_i(a, 0, 0));
        const int x = static_cast<int>(arg_i(a, 1, 0));
        const int y = static_cast<int>(arg_i(a, 2, 0));
        const int z = static_cast<int>(arg_i(a, 3, 0));
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        auto* vol = st.volume(id);
        if (!vol)
            return make_int(0);
        return make_int(static_cast<std::int64_t>(vol->get(x, y, z)));
    });

    // 5. (render3d:set-camera x y z yaw pitch [fov]) → #t
    //    yaw/pitch/fov are milliradians (int) for integer-only EDSL args.
    add("render3d:set-camera", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        st.camera.position.x = static_cast<float>(arg_i(a, 0, 0));
        st.camera.position.y = static_cast<float>(arg_i(a, 1, 0));
        st.camera.position.z = static_cast<float>(arg_i(a, 2, 0));
        // milliradians → radians
        st.camera.yaw = static_cast<float>(arg_i(a, 3, 0)) * 0.001f;
        st.camera.pitch = static_cast<float>(arg_i(a, 4, 0)) * 0.001f;
        if (a.size() >= 6 && is_int(a[5]))
            st.camera.fov_y = static_cast<float>(as_int(a[5])) * 0.001f;
        return make_bool(true);
    });

    // 6. (render3d:resize-fb cols rows) → #t
    add("render3d:resize-fb", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        const int cols = static_cast<int>(arg_i(a, 0, 40));
        const int rows = static_cast<int>(arg_i(a, 1, 20));
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        st.ensure_fb(cols, rows);
        return make_bool(true);
    });

    // 7. (render3d:build-demo vol) → #t/#f
    add("render3d:build-demo", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        const int id = static_cast<int>(arg_i(a, 0, 0));
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        auto* vol = st.volume(id);
        if (!vol)
            return make_bool(false);
        build_demo_scene(*vol);
        return make_bool(true);
    });

    // 8. (render3d:frame [vol] [headless?]) → #t
    //    headless? non-zero → capture ANSI into last_ansi (no tty write).
    //    vol defaults to 1 if omitted.
    add("render3d:frame", [&ev](std::span<const EvalValue> a) -> EvalValue {
        (void)ev;
        const int id = static_cast<int>(arg_i(a, 0, 1));
        const bool headless = arg_i(a, 1, 0) != 0;
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        auto* vol = st.volume(id);
        if (!vol)
            return make_bool(false);
        st.ensure_fb(st.fb_cols, st.fb_rows);
        auto pf = st.pixel_view();
        RenderFrameOptions opt;
        opt.time_it = true;
        if (headless) {
            st.last_ansi.clear();
            opt.present_out = &st.last_ansi;
        } else {
            opt.present_fd = 1;
        }
        st.last_stats = render_frame(st.camera, *vol, default_materials(), kDefaultMaterialCount,
                                     st.shade, pf, opt);
        return make_bool(true);
    });

    // 9. (render3d:frame-ansi [vol]) → string (headless render + return ANSI)
    add("render3d:frame-ansi", [&ev](std::span<const EvalValue> a) -> EvalValue {
        const int id = static_cast<int>(arg_i(a, 0, 1));
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        auto* vol = st.volume(id);
        if (!vol) {
            const auto kidx = static_cast<std::uint64_t>(ev.push_string_heap(std::string{}));
            return make_string(kidx);
        }
        st.ensure_fb(st.fb_cols, st.fb_rows);
        auto pf = st.pixel_view();
        st.last_ansi.clear();
        RenderFrameOptions opt;
        opt.time_it = true;
        opt.present_out = &st.last_ansi;
        st.last_stats = render_frame(st.camera, *vol, default_materials(), kDefaultMaterialCount,
                                     st.shade, pf, opt);
        const auto kidx = static_cast<std::uint64_t>(ev.push_string_heap(st.last_ansi));
        return make_string(kidx);
    });

    // 10. (render3d:stats) → hash (last frame counters)
    add("render3d:stats", [&ev](std::span<const EvalValue>) -> EvalValue {
        auto& st = global_render3d();
        std::lock_guard lock(st.mu);
        return make_stats_hash(ev, st.last_stats);
    });

#endif // AURA_ENABLE_TUI
}

} // namespace aura::compiler::primitives_detail
