// @category: unit
// @reason: Issue #2217 — unified register_render_hot_prim helper + Agent meta.
//
//   AC1: Helper API stamps RENDER_PRIMITIVE_META fields (hot + render_critical)
//   AC2: TUI hot prims use helper (no bare add + set_meta)
//   AC3: primitive:describe / meta catalog show render_critical + hot tier
//   AC4: static gate script present (coverage linter)
//   AC5: dummy registration via helper → meta assert + counter bump

#include "test_harness.hpp"

#include "compiler/primitives_meta.h"
#include "compiler/render_prim_template.hh"
#include "compiler/security_capabilities.h"

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
using aura::compiler::g_register_render_hot_prim_total;
using aura::compiler::kPrimPerfHot;
using aura::compiler::kRegisterRenderHotPrimIssue;
using aura::compiler::kRenderHotPrimNamesRequired;
using aura::compiler::register_render_hot_prim;
using aura::compiler::security::kEffectRender;
using aura::compiler::types::make_bool;
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

} // namespace

int main() {
    std::println("=== Issue #2217: register_render_hot_prim ===");

    // ── AC4 / source: helper + migration wiring ──
    {
        std::println("\n--- AC source: helper + TUI migration ---");
        auto tpl = read_file("src/compiler/render_prim_template.hh");
        auto tui = read_file("src/compiler/evaluator_primitives_tui.cpp");
        auto lint = read_file("scripts/check_register_render_hot_prim_coverage.py");
        CHECK(!tpl.empty(), "render_prim_template.hh readable");
        CHECK(tpl.find("register_render_hot_prim") != std::string::npos, "helper defined");
        CHECK(tpl.find("kRenderHotPrimNamesRequired") != std::string::npos, "required names");
        CHECK(tpl.find("MUST use register_render_hot_prim") != std::string::npos ||
                  tpl.find("REQUIRED for new hot prims") != std::string::npos,
              "docs require helper");
        CHECK(tpl.find("AURA_RENDER_HOT_ENTRY") != std::string::npos,
              "docs body still needs hot entry");
        CHECK(tui.find("register_render_hot_prim") != std::string::npos, "TUI uses helper");
        CHECK(tui.find("set_meta_for_name") == std::string::npos,
              "TUI has no ad-hoc set_meta_for_name");
        CHECK(tui.find("RENDER_PRIMITIVE_META") == std::string::npos,
              "TUI has no residual RENDER_PRIMITIVE_META");
        for (auto name : kRenderHotPrimNamesRequired) {
            const auto needle = std::string("\"") + std::string(name) + "\"";
            CHECK(tui.find(needle) != std::string::npos, std::format("TUI cites {}", name));
            // Must appear as helper registration arg, not bare add("…")
            CHECK(tui.find(std::format("add(\"{}\"", name)) == std::string::npos,
                  std::format("no bare add({})", name));
        }
        CHECK(!lint.empty() && lint.find("register_render_hot_prim") != std::string::npos,
              "AC4: coverage linter present");
        CHECK(kRegisterRenderHotPrimIssue == 2217, "issue stamp 2217");
    }

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    auto& ev = cs.evaluator();
    auto& prims = ev.primitives();

    // ── AC2 / AC3: migrated TUI hot prims have render_critical + hot tier ──
    {
        std::println("\n--- AC2/AC3: TUI hot meta via helper ---");
        for (auto name : kRenderHotPrimNamesRequired) {
            const auto slot = prims.slot_for_name(std::string(name));
            if (slot >= prims.slot_count()) {
                // TUI gated off builds may omit tui:*
                std::println("  skip {} (not registered)", name);
                continue;
            }
            const auto& meta = prims.meta_for_slot(slot);
            std::println("  {} perf_tier={} render_critical={} category={}", name, meta.perf_tier,
                         meta.render_critical, meta.category);
            CHECK(meta.perf_tier == kPrimPerfHot, std::format("{} hot tier", name));
            CHECK(meta.render_critical, std::format("{} render_critical", name));
            CHECK(meta.stable_hot_path, std::format("{} stable_hot_path", name));
            CHECK(meta.category == "rendering", std::format("{} category=rendering", name));
            CHECK((meta.required_effects & kEffectRender) != 0,
                  std::format("{} stamps kEffectRender", name));
            CHECK(meta.render_critical || meta.stable_hot_path ||
                      (meta.category == "rendering" && meta.perf_tier == kPrimPerfHot),
                  std::format("{} render-critical meta contract", name));
        }
        // Callable smoke (no TTY required)
        auto p = cs.eval("(tui:present)");
        CHECK(p.has_value(), "tui:present callable");
        auto d = cs.eval("(tui:present-dirty)");
        CHECK(d.has_value(), "tui:present-dirty callable");
    }

    // ── AC1 / AC5: dummy registration via helper ──
    {
        std::println("\n--- AC1/AC5: dummy register_render_hot_prim ---");
        const auto before = g_register_render_hot_prim_total().load(std::memory_order_relaxed);
        bool body_ran = false;
        // Generic registrar — matches PrimRegistrar (string, PrimFn-convertible).
        auto add = [&](std::string name, auto&& fn) {
            prims.add(name, std::forward<decltype(fn)>(fn));
        };
        register_render_hot_prim(
            add, ev, "test:render-hot-dummy-2217", 0,
            [&](std::span<const aura::compiler::types::EvalValue>) {
                body_ran = true;
                return make_bool(true);
            },
            "Synthetic hot render prim for #2217 unit test.", "() -> bool");
        const auto after = g_register_render_hot_prim_total().load(std::memory_order_relaxed);
        CHECK(after == before + 1, "g_register_render_hot_prim_total +1");

        const auto slot = prims.slot_for_name("test:render-hot-dummy-2217");
        CHECK(slot < prims.slot_count(), "dummy registered");
        const auto& meta = prims.meta_for_slot(slot);
        CHECK(meta.perf_tier == kPrimPerfHot, "dummy hot tier");
        CHECK(meta.render_critical, "dummy render_critical");
        CHECK(meta.stable_hot_path, "dummy stable_hot_path");
        CHECK(meta.category == "rendering", "dummy category");
        CHECK((meta.required_effects & kEffectRender) != 0, "dummy kEffectRender");
        CHECK(meta.doc.find("#2217") != std::string::npos ||
                  meta.doc.find("2217") != std::string::npos,
              "dummy doc cites issue");
        CHECK(meta.schema.find("bool") != std::string::npos, "dummy schema");

        auto fn = prims.lookup("test:render-hot-dummy-2217");
        CHECK(fn.has_value(), "dummy lookup");
        (void)(*fn)({});
        CHECK(body_ran, "dummy body runnable");

        // Hot map coherent after set_meta backfill path
        auto hot = prims.lookup_cstr("test:render-hot-dummy-2217");
        CHECK(hot.has_value(), "dummy on hot-tier lookup path");
    }

    std::println("\n=== #2217 done: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
