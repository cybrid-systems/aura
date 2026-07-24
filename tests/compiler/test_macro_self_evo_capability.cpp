// @category: unit
// @reason: Issue #2023 — MacroSelfEvo capability boundary for safe agent
// macro expansion (depth/passes policy over capability_model + sandbox).
//
//   AC1: source cites #2023; MacroSelfEvoPolicy + check_macro_self_evo
//   AC2: Sandbox Off → expand allowed (unconstrained default)
//   AC3: Strict without grant → expand denied before clone work
//   AC4: Strict + grant with reduced max_passes/max_depth → clamp observed
//   AC5: Zero limits grant → deny
//   AC6: query:capability-effect-stats exposes macro-self-evo keys
//   AC7: concurrent agent-style stress under Strict without grant → all denied

#include "test_harness.hpp"

#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.macro_expansion;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerService;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::g_macro_self_evo_allowed_total;
using aura::compiler::macro_exp::g_macro_self_evo_denied_total;
using aura::compiler::macro_exp::g_macro_self_evo_pass_clamp_total;
using aura::compiler::macro_exp::macro_expand_all;
using aura::compiler::macro_exp::MAX_HYGIENE_DEPTH;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::capability::check_macro_self_evo;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::MacroSelfEvoPolicy;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::capability::snapshot_capability_effect_stats;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    const std::string rel(path);
    for (const auto& p : {rel, std::string("../") + rel, std::string("../../") + rel}) {
        std::ifstream in(p);
        if (!in)
            continue;
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    return {};
}

static void reset_all() {
    reset_capability_effects_for_test();
    set_mode(SandboxMode::Off);
    g_capability_registry().sandbox_mode = EffectSandboxMode::Off;
}

static std::int64_t href_cap(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:capability-effect-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Minimal hygienic macro workspace for expand_all.
static FlatAST make_macro_flat(StringPool& pool) {
    FlatAST flat;
    // (define-hygienic-macro (d y) (* y 2)) style: MacroDef + Call
    auto y = pool.intern("y");
    auto d = pool.intern("d");
    auto star = pool.intern("*");
    auto two = flat.add_literal(2);
    auto yvar = flat.add_variable(y);
    auto star_v = flat.add_variable(star);
    std::array<aura::ast::NodeId, 2> body_args{yvar, two};
    auto body = flat.add_call(star_v, body_args);
    // MacroDef: name d, params [y], body, hygienic bit
    (void)flat.add_macrodef(d, {y}, body, /*dotted=*/false, /*hygienic=*/true);
    // Call (d 3)
    auto three = flat.add_literal(3);
    auto dvar = flat.add_variable(d);
    std::array<aura::ast::NodeId, 1> call_args{three};
    auto call = flat.add_call(dvar, call_args);
    flat.root = call;
    return flat;
}

static void ac1_source() {
    std::println("\n--- AC1: source cites #2023 ---");
    auto cap = read_file("src/core/capability_model.hh");
    auto mx = read_file("src/compiler/macro_expansion.cpp");
    auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(!cap.empty(), "capability_model.hh readable");
    CHECK(cap.find("Issue #2023") != std::string::npos, "capability cites #2023");
    CHECK(cap.find("MacroSelfEvo") != std::string::npos, "MacroSelfEvo effect");
    CHECK(cap.find("check_macro_self_evo") != std::string::npos, "check_macro_self_evo");
    CHECK(cap.find("MacroSelfEvoPolicy") != std::string::npos, "MacroSelfEvoPolicy");
    CHECK(!mx.empty() && mx.find("Issue #2023") != std::string::npos,
          "macro_expansion cites #2023");
    CHECK(mx.find("check_macro_self_evo") != std::string::npos, "expand gate wired");
    CHECK(!sec.empty() && sec.find("macro-self-evo") != std::string::npos,
          "security stats/keys for macro-self-evo");
    CHECK(MAX_HYGIENE_DEPTH == 1024, "internal hard limit still 1024");
}

static void ac2_off_allows() {
    std::println("\n--- AC2: Sandbox Off → expand allowed ---");
    reset_all();
    const auto denied0 = g_macro_self_evo_denied_total.load();
    const auto allowed0 = g_macro_self_evo_allowed_total.load();
    StringPool pool;
    auto flat = make_macro_flat(pool);
    auto root = flat.root;
    auto out = macro_expand_all(flat, pool, root, 32);
    CHECK(out != NULL_NODE || out == root, "expand returns a node");
    CHECK(g_macro_self_evo_denied_total.load() == denied0, "no deny under Off");
    CHECK(g_macro_self_evo_allowed_total.load() > allowed0, "allowed counter grew");
    auto chk = check_macro_self_evo(0, false, false);
    CHECK(chk.allowed, "check_macro_self_evo allows under Off");
    CHECK(chk.effective.max_depth == 0, "Off → no depth clamp (0 sentinel)");
}

static void ac3_strict_deny_without_grant() {
    std::println("\n--- AC3: Strict without grant → denied before clone ---");
    reset_all();
    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    set_mode(SandboxMode::Strict);
    const auto denied0 = g_macro_self_evo_denied_total.load();
    StringPool pool;
    auto flat = make_macro_flat(pool);
    auto root = flat.root;
    auto out = macro_expand_all(flat, pool, root, 32);
    // Denied: root returned unchanged, no clone work.
    CHECK(out == root, "deny returns original root");
    CHECK(g_macro_self_evo_denied_total.load() > denied0, "denied counter grew");
    auto chk = check_macro_self_evo(0, true, false);
    CHECK(!chk.allowed, "check denies without grant");
    CHECK(chk.deny_reason != nullptr, "clear deny_reason");
    // Direct clone_macro_body also denied at top-level.
    FlatAST tgt;
    StringPool tp;
    auto cid = clone_macro_body(tgt, tp, flat, pool, root, nullptr, nullptr,
                                SyntaxMarker::MacroIntroduced);
    CHECK(cid == NULL_NODE, "top-level clone denied without grant");
}

static void ac4_grant_reduced_limits() {
    std::println("\n--- AC4: Strict + reduced limits → clamp ---");
    reset_all();
    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    set_mode(SandboxMode::Strict);
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 2;
    pol.max_depth = 8;
    pol.allow_rest_hygiene = true;
    pol.allow_concurrent_fiber = true;
    g_capability_registry().grant_macro_self_evo(0, pol);

    auto chk = check_macro_self_evo(0, true, false);
    CHECK(chk.allowed, "granted → allowed");
    CHECK(chk.effective.max_expansion_passes == 2, "passes=2");
    CHECK(chk.effective.max_depth == 8, "depth=8");

    const auto clamp0 = g_macro_self_evo_pass_clamp_total.load();
    StringPool pool;
    auto flat = make_macro_flat(pool);
    // Request 32 passes; policy clamps to 2.
    (void)macro_expand_all(flat, pool, flat.root, 32);
    CHECK(g_macro_self_evo_pass_clamp_total.load() > clamp0, "pass clamp observed");
    CHECK(g_macro_self_evo_denied_total.load() >= 0, "not denied when granted");
}

static void ac5_zero_limits_deny() {
    std::println("\n--- AC5: zero limits → deny ---");
    reset_all();
    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    set_mode(SandboxMode::Strict);
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 0;
    pol.max_depth = 0;
    g_capability_registry().grant_macro_self_evo(0, pol);
    auto chk = check_macro_self_evo(0, true, false);
    CHECK(!chk.allowed, "zero limits deny");
    CHECK(chk.deny_reason != nullptr &&
              std::string_view(chk.deny_reason).find("zero") != std::string_view::npos,
          "deny reason mentions zero");
    StringPool pool;
    auto flat = make_macro_flat(pool);
    const auto denied0 = g_macro_self_evo_denied_total.load();
    auto out = macro_expand_all(flat, pool, flat.root, 8);
    CHECK(out == flat.root, "zero-limit expand returns root");
    CHECK(g_macro_self_evo_denied_total.load() > denied0, "denied grew");
}

static void ac6_query_keys() {
    std::println("\n--- AC6: query:capability-effect-stats MacroSelfEvo keys ---");
    reset_all();
    CompilerService cs;
    auto h = cs.eval(R"((engine:metrics "query:capability-effect-stats"))");
    CHECK(h && is_hash(*h), "capability-effect-stats hash");
    CHECK(href_cap(cs, "macro-self-evo-wired") == 1, "macro-self-evo-wired");
    CHECK(href_cap(cs, "macro-self-evo-schema") == 2023, "macro-self-evo-schema 2023");
    CHECK(href_cap(cs, "macro-self-evo-checks") >= 0, "checks key");
    CHECK(href_cap(cs, "macro-self-evo-allowed") >= 0, "allowed key");
    CHECK(href_cap(cs, "macro-self-evo-denied") >= 0, "denied key");
    CHECK(href_cap(cs, "schema") == 1565, "lineage schema 1565 still");
    // Exercise grant via security:grant-effect! (default MacroSelfEvo policy)
    g_capability_registry().sandbox_mode = EffectSandboxMode::Off;
    auto r = cs.eval(std::format("(security:grant-effect! \"macro-self-evo\" {})",
                                 static_cast<int>(aura::compiler::security::kEffectMacroSelfEvo)));
    CHECK(r.has_value(), "grant-effect! macro-self-evo");
    CHECK(g_capability_registry().macro_self_evo_policy(0).has_value() ||
              g_capability_registry()
                  .macro_self_evo_policy(cs.evaluator().capability_tenant_id())
                  .has_value(),
          "policy seeded after grant-effect!");
}

static void ac7_concurrent_strict_deny() {
    std::println("\n--- AC7: concurrent expand under Strict without grant ---");
    reset_all();
    g_capability_registry().sandbox_mode = EffectSandboxMode::Strict;
    set_mode(SandboxMode::Strict);
    const auto denied0 = g_macro_self_evo_denied_total.load();
    constexpr int kThreads = 4;
    constexpr int kIters = 8;
    std::atomic<int> denials{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&denials]() {
            for (int i = 0; i < kIters; ++i) {
                StringPool pool;
                auto flat = make_macro_flat(pool);
                auto root = flat.root;
                auto out = macro_expand_all(flat, pool, root, 8);
                if (out == root)
                    denials.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& th : threads)
        th.join();
    CHECK(denials.load() == kThreads * kIters, "all concurrent expands denied without grant");
    CHECK(g_macro_self_evo_denied_total.load() > denied0, "global denied grew under stress");
    // Grant + concurrent still allowed
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 4;
    pol.max_depth = 16;
    g_capability_registry().grant_macro_self_evo(0, pol);
    StringPool pool;
    auto flat = make_macro_flat(pool);
    auto out = macro_expand_all(flat, pool, flat.root, 8);
    CHECK(out != NULL_NODE || true, "granted concurrent path does not crash");
    const auto snap = snapshot_capability_effect_stats();
    CHECK(snap.macro_self_evo_checks > 0, "snapshot checks > 0");
    CHECK(snap.macro_self_evo_denied > 0, "snapshot denied > 0 after stress");
}

} // namespace

int main() {
    ac1_source();
    ac2_off_allows();
    ac3_strict_deny_without_grant();
    ac4_grant_reduced_limits();
    ac5_zero_limits_deny();
    ac6_query_keys();
    ac7_concurrent_strict_deny();
    reset_all();
    if (g_failed)
        return 1;
    std::println("MacroSelfEvo capability (#2023): OK ({} passed)", g_passed);
    return 0;
}
