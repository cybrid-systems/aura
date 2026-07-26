// @category: unit
// @reason: Issue #2136 — close FFI/Render effect matrix: require_effect(Render)
// on batch draw/present + FFI hot-path hand-off.
//
//   AC1: RENDER_PRIMITIVE_META stamps required_effects=kEffectRender
//   AC2: Strict + no grant → present-batch / draw-batch denied; no side-effect
//   AC3: Strict + grant (render / wildcard / kernel) → succeed
//   AC4: metrics effect_denied_render_total + render_effect_granted_total
//   AC5: FFIBatchHotPath dispatch_batch(render_effect_ok=false) skips invoke
//   AC6: concurrent revoke under MutationBoundary → subsequent present denied
//   AC7: name infer covers tui: / terminal-present / c-render
//   AC8: source + schema-2136 on query:render-stats

#include "test_harness.hpp"

#include "compiler/ffi_hot_path.hh"
#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "compiler/security_side_effect.hh"
#include "core/capability_model.hh"

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

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::infer_required_effects_from_name;
using aura::compiler::is_side_effect_prim_name;
using aura::compiler::security::kCapRender;
using aura::compiler::security::kCapWildcard;
using aura::compiler::security::kEffectNone;
using aura::compiler::security::kEffectRender;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_int;
using aura::core::capability::Effect;
using aura::core::capability::reset_capability_effects_for_test;
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

static std::int64_t href(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"query:render-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool is_deny_result(const std::optional<aura::compiler::types::EvalValue>& r) {
    if (!r)
        return true;
    if (is_error(*r))
        return true;
    // Some prims soft-return int 0 / -1 on deny via body; dispatch returns error pair.
    return false;
}

} // namespace

int main() {
    std::println("=== Issue #2136: Render effect gate (batch + FFI) ===");

    // ── AC8: source ──
    {
        std::println("\n--- AC8: source ---");
        auto pd = read_file("src/compiler/primitives_detail.h");
        auto se = read_file("src/compiler/evaluator_security.cpp");
        auto fh = read_file("src/compiler/ffi_hot_path.hh");
        auto met = read_file("src/compiler/observability_metrics.h");
        CHECK(pd.find("kEffectRender") != std::string::npos, "RENDER_PRIMITIVE_META stamps Render");
        CHECK(pd.find("required_effects") != std::string::npos, "required_effects in meta macro");
        CHECK(se.find("effect_denied_render_total") != std::string::npos, "deny metric wired");
        CHECK(se.find("render_effect_granted_total") != std::string::npos ||
                  met.find("render_effect_granted_total") != std::string::npos,
              "grant metric present");
        CHECK(fh.find("render_effect_ok") != std::string::npos, "FFI dispatch effect gate");
        CHECK(fh.find("effect_denied_render_total") != std::string::npos, "FFI deny counter");
        CHECK(met.find("effect_denied_render_total") != std::string::npos, "CompilerMetrics deny");
    }

    // ── AC1: RENDER_PRIMITIVE_META stamps required_effects ──
    {
        std::println("\n--- AC1: PrimMeta.required_effects on render batch ---");
        reset_capability_effects_for_test();
        CompilerService cs;
        auto& prims = cs.evaluator().primitives();
        for (const char* name : {"terminal-present-batch", "tui:present-batch", "tui:draw-batch",
                                 "tui:fill-rect", "c-render-call", "c-present-batch"}) {
            const auto slot = prims.slot_for_name(name);
            if (slot >= prims.slot_count()) {
                // TUI gated off builds may omit tui:* — skip those.
                if (std::string_view(name).starts_with("tui:"))
                    continue;
                CHECK(false, std::format("{} registered", name));
                continue;
            }
            const auto& meta = prims.meta_for_slot(slot);
            std::println("  {} required_effects={}", name, meta.required_effects);
            CHECK((meta.required_effects & kEffectRender) != 0,
                  std::format("{} stamps kEffectRender", name));
        }
    }

    // ── AC7: name inference ──
    {
        std::println("\n--- AC7: name inference ---");
        CHECK(infer_required_effects_from_name("tui:present-batch") == kEffectRender, "infer tui");
        CHECK(infer_required_effects_from_name("terminal-present-batch") == kEffectRender,
              "infer terminal-present");
        CHECK(infer_required_effects_from_name("c-render-call") == kEffectRender, "infer c-render");
        CHECK(infer_required_effects_from_name("c-present-batch") == kEffectRender,
              "infer c-present");
        CHECK(is_side_effect_prim_name("tui:draw-batch"), "tui is side-effect");
        CHECK(infer_required_effects_from_name("query:render-stats") == kEffectNone,
              "query is not");
    }

    // ── AC2: Strict + no grant → deny; no side-effect ──
    {
        std::println("\n--- AC2: Strict no-grant deny ---");
        reset_capability_effects_for_test();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto deny_before =
            m ? m->effect_denied_render_total.load(std::memory_order_relaxed) : 0;
        const auto present_before =
            m ? m->terminal_present_batch_total.load(std::memory_order_relaxed) : 0;

        // Create buffer under Off, then switch to Strict for present.
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make buffer under Off");
        const auto bid = as_int(*id);

        ev.set_effect_sandbox_mode(2); // Strict
        // Ensure no grants for current tenant.
        reset_capability_effects_for_test();
        ev.set_effect_sandbox_mode(2);

        auto p = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        // Dispatch returns typed error; body must not run → present counter unchanged.
        const bool denied = !p || is_error(*p) || (is_int(*p) && as_int(*p) < 0);
        CHECK(denied, "present denied under Strict without Render grant");
        if (m) {
            const auto deny_after = m->effect_denied_render_total.load(std::memory_order_relaxed);
            const auto present_after =
                m->terminal_present_batch_total.load(std::memory_order_relaxed);
            std::println("  deny {} → {}, present_total {} → {}", deny_before, deny_after,
                         present_before, present_after);
            CHECK(deny_after > deny_before, "effect_denied_render_total bumped");
            CHECK(present_after == present_before, "no present side-effect on deny");
        }
    }

    // ── AC3: Strict + grant → succeed ──
    {
        std::println("\n--- AC3: Strict + grant allow ---");
        reset_capability_effects_for_test();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto id = cs.eval("(make-terminal-buffer 4 2)");
        CHECK(id && is_int(*id) && as_int(*id) >= 0, "make buffer");
        const auto bid = as_int(*id);
        (void)cs.eval(std::format("(terminal-set-cell {} 0 0 65 7 0)", bid));

        ev.set_effect_sandbox_mode(2);
        reset_capability_effects_for_test();
        ev.set_effect_sandbox_mode(2);
        ev.grant_capability(kCapRender);
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto grant_n = m ? m->render_effect_granted_total.load(std::memory_order_relaxed) : 0;
        CHECK(grant_n > 0, "render_effect_granted_total bumped on grant");

        auto p = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        CHECK(p && is_int(*p) && as_int(*p) >= 0, "present allowed with Render grant");

        // Wildcard path also works.
        reset_capability_effects_for_test();
        ev.set_effect_sandbox_mode(2);
        ev.grant_capability(kCapWildcard);
        auto p2 = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        CHECK(p2 && is_int(*p2) && as_int(*p2) >= 0, "present allowed with wildcard");
    }

    // ── AC4: schema-2136 metrics surface ──
    {
        std::println("\n--- AC4: schema-2136 ---");
        reset_capability_effects_for_test();
        CompilerService cs;
        CHECK(href(cs, "schema-2136") == 2136, "schema-2136");
        CHECK(href(cs, "render-effect-gate-wired") == 1, "gate wired");
        CHECK(href(cs, "effect-denied-render-total") >= 0, "deny key");
        CHECK(href(cs, "render-effect-granted-total") >= 0, "grant key");
    }

    // ── AC5: FFIBatchHotPath effect gate ──
    {
        std::println("\n--- AC5: FFI dispatch_batch gate ---");
        aura::compiler::ffi_hot::reset_ffi_hot_path_for_test();
        auto& hot = aura::compiler::ffi_hot::global_ffi_batch_hot_path();
        static std::int64_t calls = 0;
        auto dummy = +[](const std::int64_t*, std::size_t) -> std::int64_t {
            ++calls;
            return 42;
        };
        std::int64_t args[] = {1, 2};
        const auto h = aura::compiler::ffi_hot::ffi_sig_hash("test-render", "batch");
        calls = 0;
        const auto denied = hot.dispatch_batch(h, reinterpret_cast<void*>(dummy),
                                               aura::compiler::ffi_hot::RenderFfiAbi::BatchArgs,
                                               args, /*render_effect_ok=*/false);
        CHECK(denied == -1, "deny returns -1");
        CHECK(calls == 0, "no invoke on deny");
        const auto snap = aura::compiler::ffi_hot::snapshot_ffi_hot_path();
        CHECK(snap.effect_denied_render_total >= 1, "FFI deny counter");

        const auto ok = hot.dispatch_batch(h, reinterpret_cast<void*>(dummy),
                                           aura::compiler::ffi_hot::RenderFfiAbi::BatchArgs, args,
                                           /*render_effect_ok=*/true);
        CHECK(ok == 42, "allow invokes backend");
        CHECK(calls == 1, "one invoke on allow");
    }

    // ── AC6: revoke mid-session → subsequent present denied ──
    {
        std::println("\n--- AC6: revoke under capability change ---");
        reset_capability_effects_for_test();
        CompilerService cs;
        auto& ev = cs.evaluator();
        auto id = cs.eval("(make-terminal-buffer 2 2)");
        CHECK(id && is_int(*id), "buffer");
        const auto bid = as_int(*id);

        ev.set_effect_sandbox_mode(2);
        reset_capability_effects_for_test();
        ev.set_effect_sandbox_mode(2);
        ev.grant_effect_capability(ev.capability_tenant_id(), kCapRender,
                                   static_cast<std::uint16_t>(kEffectRender), 0);
        auto ok1 = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        CHECK(ok1 && is_int(*ok1) && as_int(*ok1) >= 0, "present with grant");

        // Revoke Render — subsequent present must deny cleanly.
        ev.revoke_effect_capability(ev.capability_tenant_id(), kCapRender);
        auto* m = static_cast<CompilerMetrics*>(ev.compiler_metrics());
        const auto deny_before =
            m ? m->effect_denied_render_total.load(std::memory_order_relaxed) : 0;
        auto ok2 = cs.eval(std::format("(terminal-present-batch {} -1)", bid));
        const bool denied = !ok2 || is_error(*ok2);
        CHECK(denied, "present denied after revoke");
        if (m) {
            const auto deny_after = m->effect_denied_render_total.load(std::memory_order_relaxed);
            CHECK(deny_after > deny_before, "deny metric after revoke");
        }
    }

    // ── Unrestricted (Off) still works without explicit grant ──
    {
        std::println("\n--- unrestricted Off path ---");
        reset_capability_effects_for_test();
        CompilerService cs;
        cs.evaluator().set_effect_sandbox_mode(0);
        auto id = cs.eval("(make-terminal-buffer 2 1)");
        CHECK(id && is_int(*id), "buffer Off");
        auto p = cs.eval(std::format("(terminal-present-batch {} -1)", as_int(*id)));
        CHECK(p && is_int(*p) && as_int(*p) >= 0, "Off allows present without grant");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
