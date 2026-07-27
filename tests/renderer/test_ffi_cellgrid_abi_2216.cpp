// @category: unit
// @reason: Issue #2216 — FFI CellGrid ABI (TermCell* + DirtyRegion* handoff).
//
//   AC1: abi_from_signature recognizes cellgrid / TermCell* / DirtyRegion
//   AC2: dispatch_cellgrid hit/miss + cellgrid_invoke_total
//   AC3: present_batch prefers registered CellGrid backend
//   AC4: query:render-ffi-available / render-stats schema-2216
//   AC5: BatchArgs / Nullary paths still work; source cites

#include "test_harness.hpp"

#include "compiler/ffi_hot_path.hh"
#include "renderer/batch_terminal.hh"
#include "renderer/render_pass.hh"
#include "renderer/render_primitives.hh"
#include "stdlib/render_ffi.hh"

#include <array>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::ffi_hot::abi_from_signature;
using aura::compiler::ffi_hot::clear_cellgrid_present_backend;
using aura::compiler::ffi_hot::ffi_sig_hash;
using aura::compiler::ffi_hot::g_ffi_hot_path_stats;
using aura::compiler::ffi_hot::global_ffi_batch_hot_path;
using aura::compiler::ffi_hot::kCellGridSignature;
using aura::compiler::ffi_hot::register_cellgrid_present_backend;
using aura::compiler::ffi_hot::RenderFfiAbi;
using aura::compiler::ffi_hot::reset_ffi_hot_path_for_test;
using aura::compiler::ffi_hot::snapshot_ffi_hot_path;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::renderer::DirtyRegion;
using aura::renderer::present_batch;
using aura::renderer::reset_render_engine_counters_for_test;
using aura::renderer::TermCell;
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

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Test CellGrid backend: returns w*h + dirty_cells (or w*h if dirty null).
static std::atomic<std::uint64_t> g_stub_calls{0};
static std::atomic<std::int32_t> g_stub_last_w{0};
static std::atomic<std::int32_t> g_stub_last_h{0};

extern "C" std::int64_t aura_test_cellgrid_present(const TermCell* cells, std::int32_t w,
                                                   std::int32_t h, const DirtyRegion* dirty) {
    g_stub_calls.fetch_add(1, std::memory_order_relaxed);
    g_stub_last_w.store(w, std::memory_order_relaxed);
    g_stub_last_h.store(h, std::memory_order_relaxed);
    if (!cells || w <= 0 || h <= 0)
        return -1;
    std::int64_t base = static_cast<std::int64_t>(w) * static_cast<std::int64_t>(h);
    if (dirty && dirty->is_dirty())
        return base + static_cast<std::int64_t>(dirty->cell_count());
    return base;
}

extern "C" std::int64_t aura_test_batch_fn(const std::int64_t* args, std::size_t argc) {
    std::int64_t s = 0;
    for (std::size_t i = 0; i < argc; ++i)
        s += args[i];
    return s;
}

extern "C" void aura_test_nullary_fn() {}

} // namespace

int main() {
    std::println("=== Issue #2216: FFI CellGrid ABI ===");
    reset_ffi_hot_path_for_test();
    reset_render_engine_counters_for_test();
    g_stub_calls.store(0);

    // ── AC5: source ──
    {
        std::println("\n--- AC5: source ---");
        auto fh = read_file("src/compiler/ffi_hot_path.hh");
        auto rp = read_file("src/renderer/render_primitives.cpp");
        CHECK(fh.find("CellGrid") != std::string::npos, "CellGrid ABI");
        CHECK(fh.find("CellGridPresentFn") != std::string::npos, "CellGridPresentFn");
        CHECK(fh.find("dispatch_cellgrid") != std::string::npos, "dispatch_cellgrid");
        CHECK(fh.find("#2216") != std::string::npos, "cites #2216");
        CHECK(fh.find("cellgrid_invoke_total") != std::string::npos, "metric");
        CHECK(rp.find("try_cellgrid_present") != std::string::npos ||
                  rp.find("cellgrid_present_backend") != std::string::npos,
              "present path wired");
    }

    // ── AC1: signature parse ──
    {
        std::println("\n--- AC1: abi_from_signature ---");
        CHECK(abi_from_signature("cellgrid (TermCell*, i32, i32, DirtyRegion*) -> i64") ==
                  RenderFfiAbi::CellGrid,
              "cellgrid marker");
        CHECK(abi_from_signature("fn(TermCell*)") == RenderFfiAbi::CellGrid, "TermCell*");
        CHECK(abi_from_signature("present DirtyRegion*") == RenderFfiAbi::CellGrid, "DirtyRegion");
        CHECK(abi_from_signature("batch (I64*) -> I64") == RenderFfiAbi::BatchArgs, "batch still");
        CHECK(abi_from_signature("()") == RenderFfiAbi::Nullary, "nullary still");
        CHECK(abi_from_signature("mystery") == RenderFfiAbi::MetricsOnly, "metrics-only");
        CHECK(kCellGridSignature.find("cellgrid") != std::string_view::npos, "canonical sig");
    }

    // ── AC2: dispatch hit/miss ──
    {
        std::println("\n--- AC2: dispatch_cellgrid ---");
        reset_ffi_hot_path_for_test();
        auto& hot = global_ffi_batch_hot_path();
        const auto h = ffi_sig_hash("c-present-cellgrid", kCellGridSignature);
        void* fn = reinterpret_cast<void*>(&aura_test_cellgrid_present);

        std::vector<TermCell> cells(80 * 24, TermCell::space_palette());
        DirtyRegion dirty{};
        dirty.mark_dirty(1, 2);
        dirty.mark_dirty(3, 4);

        const auto r0 =
            hot.dispatch_cellgrid(h, fn, cells.data(), 80, 24, &dirty, /*effect_ok=*/true);
        CHECK(r0 == 80 * 24 + static_cast<std::int64_t>(dirty.cell_count()), "miss invoke");
        CHECK(g_ffi_hot_path_stats().miss_total.load() == 1, "miss +1");
        CHECK(g_ffi_hot_path_stats().cellgrid_invoke_total.load() == 1, "cellgrid invoke +1");
        CHECK(g_stub_calls.load() == 1, "stub called");
        CHECK(g_stub_last_w.load() == 80 && g_stub_last_h.load() == 24, "dims");

        // Hit path
        const auto r1 =
            hot.dispatch_cellgrid(h, fn, cells.data(), 80, 24, &dirty, /*effect_ok=*/true);
        CHECK(r1 == r0, "hit same ret");
        CHECK(g_ffi_hot_path_stats().hit_total.load() == 1, "hit +1");
        CHECK(g_ffi_hot_path_stats().cellgrid_invoke_total.load() == 2, "invoke +2");

        // Effect denied
        const auto denied =
            hot.dispatch_cellgrid(h, fn, cells.data(), 80, 24, &dirty, /*effect_ok=*/false);
        CHECK(denied == -1, "effect deny");
        CHECK(g_ffi_hot_path_stats().effect_denied_render_total.load() >= 1, "denied counter");

        // Regression: BatchArgs still works
        reset_ffi_hot_path_for_test();
        std::array<std::int64_t, 2> args{3, 4};
        const auto hb = ffi_sig_hash("batch", "batch");
        const auto rb = hot.dispatch_batch(hb, reinterpret_cast<void*>(&aura_test_batch_fn),
                                           RenderFfiAbi::BatchArgs, args);
        CHECK(rb == 7, "BatchArgs regression");
        const auto hn = ffi_sig_hash("n", "()");
        const auto rn = hot.dispatch_batch(hn, reinterpret_cast<void*>(&aura_test_nullary_fn),
                                           RenderFfiAbi::Nullary, {});
        CHECK(rn == 0, "Nullary regression");
    }

    // ── AC3: present_batch uses CellGrid backend ──
    {
        std::println("\n--- AC3: present_batch CellGrid prefer ---");
        reset_ffi_hot_path_for_test();
        reset_render_engine_counters_for_test();
        g_stub_calls.store(0);
        register_cellgrid_present_backend(&aura_test_cellgrid_present);

        std::vector<TermCell> cells(40 * 12, TermCell::space_palette());
        DirtyRegion dirty{};
        dirty.mark_all_dirty(40, 12);
        aura::renderer::FramebufferSoA fb{40, 12, cells.data()};
        const auto before = g_stub_calls.load();
        const auto n = present_batch(fb, dirty, /*fd=*/-1);
        CHECK(n > 0, "present_batch returned via CellGrid");
        CHECK(g_stub_calls.load() > before, "stub invoked from present_batch");
        CHECK(dirty.is_clean(), "dirty cleared after CellGrid present");
        CHECK(g_ffi_hot_path_stats().cellgrid_invoke_total.load() >= 1, "invoke metric");

        clear_cellgrid_present_backend();
        // Without backend, ANSI path works (no stub call)
        dirty.mark_dirty(0, 0);
        const auto stub0 = g_stub_calls.load();
        const auto n2 = present_batch(fb, dirty, /*fd=*/-1);
        CHECK(n2 >= 0, "ANSI path still works");
        CHECK(g_stub_calls.load() == stub0, "no stub without backend");
    }

    // ── AC4: query ──
    {
        std::println("\n--- AC4: query schema-2216 ---");
        register_cellgrid_present_backend(&aura_test_cellgrid_present);
        CompilerService cs;
        CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
        CHECK(href(cs, "query:render-ffi-available", "schema-2216") == 2216, "ffi schema-2216");
        CHECK(href(cs, "query:render-ffi-available", "cellgrid-abi-wired") == 1, "wired");
        CHECK(href(cs, "query:render-ffi-available", "cellgrid-backend-registered") == 1,
              "backend registered");
        CHECK(href(cs, "query:render-ffi-available", "cellgrid-invoke-total") >= 0, "invoke key");
        CHECK(href(cs, "query:render-stats", "schema-2216") == 2216, "render-stats schema");
        CHECK(href(cs, "query:render-stats", "cellgrid-abi-wired") == 1, "render-stats wired");
        clear_cellgrid_present_backend();
    }

    std::println("\n=== test_ffi_cellgrid_abi_2216: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
