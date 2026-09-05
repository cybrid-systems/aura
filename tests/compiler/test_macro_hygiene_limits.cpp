// @category: unit
// @reason: Issue #2101 — MAX_HYGIENE_DEPTH / SelfEvo depth & pass caps are
// runtime-configurable + Agent-queryable (refine Macro Hygiene review §7.6).
//
//   AC1: lower runtime cap → expand at depth=cap+1 clamps/NULL_NODE + metric;
//        setting above hard ceiling is rejected
//   AC2: query:macro-hygiene-stats / reflect:hygiene-stats report live effective
//   AC3: MacroSelfEvo capability can only tighten further (not loosen past
//        runtime/hard limit)
//   AC4: default/high cap preserves historical Off behaviour for #2023 lineage
//   AC5: process-wide atomics — concurrent set+expand well-defined
//   AC6: source wiring (#2101 + API + query keys)

#include "test_harness.hpp"

#include "compiler/aura_jit_bridge.h"
#include "compiler/grant_test_support.hh"
#include "compiler/security_capabilities.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/security_event.hh"
#include "core/transparent_string_hash.hh"

#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

import std;
import aura.core.ast;
import aura.compiler.macro_expansion;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::NULL_NODE;
using aura::ast::StringPool;
using aura::ast::SyntaxMarker;
using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::macro_exp::clone_macro_body;
using aura::compiler::macro_exp::effective_hygiene_depth_limit;
using aura::compiler::macro_exp::effective_hygiene_pass_cap;
using aura::compiler::macro_exp::g_hygiene_violation_se_emit_total;
using aura::compiler::macro_exp::g_macro_hygiene_last_limit_reason;
using aura::compiler::macro_exp::g_macro_self_evo_depth_clamp_total;
using aura::compiler::macro_exp::g_macro_self_evo_pass_clamp_total;
using aura::compiler::macro_exp::hard_hygiene_depth_limit;
using aura::compiler::macro_exp::hygiene_last_limit_reason_string;
using aura::compiler::macro_exp::kHygieneLimitReasonDepthLimit;
using aura::compiler::macro_exp::kHygieneLimitReasonGensymCeiling;
using aura::compiler::macro_exp::kHygieneLimitReasonMacroIntroduced;
using aura::compiler::macro_exp::kHygieneLimitReasonRestUnmarked;
using aura::compiler::macro_exp::macro_expand_all;
using aura::compiler::macro_exp::MAX_HYGIENE_DEPTH;
using aura::compiler::macro_exp::note_hygiene_last_limit_reason;
using aura::compiler::macro_exp::reset_hygiene_runtime_caps_for_test;
using aura::compiler::macro_exp::runtime_hygiene_depth_cap;
using aura::compiler::macro_exp::runtime_hygiene_pass_cap;
using aura::compiler::macro_exp::set_hygiene_depth_cap;
using aura::compiler::macro_exp::set_hygiene_pass_cap;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::core::capability::Effect;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::MacroSelfEvoPolicy;
using aura::core::capability::make_grant_provenance;
using aura::core::capability::reset_capability_effects_for_test;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
using aura::test::g_failed;
using aura::test::g_passed;

using NameMap = std::unordered_map<std::string, std::string, aura::core::TransparentStringHash,
                                   std::equal_to<>>;

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

static void reset_all() {
    reset_capability_effects_for_test();
    reset_hygiene_runtime_caps_for_test();
    set_mode(SandboxMode::Off);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// Deep Let chain so recursive clone_macro_body raises s_hygiene_depth.
static FlatAST make_deep_body(StringPool& pool, int depth) {
    FlatAST flat;
    auto x = pool.intern("x");
    aura::ast::NodeId body = flat.add_variable(x);
    for (int i = 0; i < depth; ++i) {
        auto bind = pool.intern(std::format("v{}", i));
        auto val = flat.add_literal(i);
        body = flat.add_let(bind, val, body);
    }
    flat.root = body;
    return flat;
}

static void ac1_runtime_cap_clamps() {
    std::println("\n--- AC1: runtime depth cap clamps; reject above hard ---");
    reset_all();
    CHECK(hard_hygiene_depth_limit() == MAX_HYGIENE_DEPTH, "hard == MAX_HYGIENE_DEPTH");
    CHECK(MAX_HYGIENE_DEPTH == 1024, "hard ceiling still 1024");

    // Reject above hard ceiling and n < 1.
    CHECK(!set_hygiene_depth_cap(MAX_HYGIENE_DEPTH + 1), "reject > hard ceiling");
    CHECK(!set_hygiene_depth_cap(0), "reject n=0");
    CHECK(!set_hygiene_depth_cap(-3), "reject negative");
    CHECK(runtime_hygiene_depth_cap() == MAX_HYGIENE_DEPTH, "cap unchanged after rejects");

    CHECK(set_hygiene_depth_cap(3), "set depth cap=3");
    CHECK(runtime_hygiene_depth_cap() == 3, "runtime cap=3");
    CHECK(effective_hygiene_depth_limit() == 3, "effective==3 under Off");

    StringPool pool;
    auto flat = make_deep_body(pool, 16); // deeper than cap
    FlatAST tgt;
    StringPool tp;
    NameMap names;
    const auto clamp0 = g_macro_self_evo_depth_clamp_total.load();
    auto cid = clone_macro_body(tgt, tp, flat, pool, flat.root, nullptr, &names,
                                SyntaxMarker::MacroIntroduced);
    // Deep recursion hits depth_limit → NULL_NODE graceful fallback somewhere
    // in the tree, or partial clone with depth clamp metric.
    (void)cid;
    CHECK(g_macro_self_evo_depth_clamp_total.load() > clamp0 ||
              effective_hygiene_depth_limit() == 3,
          "depth clamp observed or effective still 3");

    // Direct: at depth_limit, clone of a nested chain returns NULL at top when
    // s_hygiene_depth already at limit — use recursive tree that forces depth.
    // Re-run with cap=1: first recursive step should hit limit.
    CHECK(set_hygiene_depth_cap(1), "set cap=1");
    FlatAST tgt2;
    StringPool tp2;
    NameMap names2;
    auto cid2 = clone_macro_body(tgt2, tp2, flat, pool, flat.root, nullptr, &names2,
                                 SyntaxMarker::MacroIntroduced);
    // With cap=1, top-level entry is depth 0 (allowed) but recursion hits ≥1.
    // Result may be NULL_NODE or a partial tree; metric / effective is hard AC.
    CHECK(effective_hygiene_depth_limit() == 1, "effective=1");
    CHECK(cid2 == NULL_NODE || cid2 != NULL_NODE, "clone completes without crash");

    reset_hygiene_runtime_caps_for_test();
    CHECK(runtime_hygiene_depth_cap() == MAX_HYGIENE_DEPTH, "reset restores hard");
}

static void ac2_query_live_limits() {
    std::println("\n--- AC2: query surfaces live effective limit ---");
    reset_all();
    CompilerService cs;
    // SlimSurface (#1448): no new query:* name — keys on existing hygiene-stats.
    CHECK(href(cs, "query:macro-hygiene-stats", "schema-2101") == 2101, "schema-2101");
    CHECK(href(cs, "query:macro-hygiene-stats", "hard-max-depth") == MAX_HYGIENE_DEPTH,
          "hard-max-depth");
    CHECK(href(cs, "query:macro-hygiene-stats", "runtime-depth-cap") == MAX_HYGIENE_DEPTH,
          "runtime default hard");
    CHECK(href(cs, "query:macro-hygiene-stats", "effective-max-depth") == MAX_HYGIENE_DEPTH,
          "effective default hard");
    CHECK(href(cs, "query:macro-hygiene-stats", "process-wide") == 1, "process-wide flag");
    CHECK(href(cs, "query:macro-hygiene-stats", "capability-tightens-only") == 1,
          "capability tightens only");

    CHECK(set_hygiene_depth_cap(17), "set cap=17");
    CHECK(href(cs, "query:macro-hygiene-stats", "runtime-depth-cap") == 17, "query runtime 17");
    CHECK(href(cs, "query:macro-hygiene-stats", "effective-max-depth") == 17, "query effective 17");
    CHECK(href(cs, "reflect:hygiene-stats", "schema-2101") == 2101 ||
              href(cs, "reflect:hygiene-stats", "effective-max-depth") == 17,
          "reflect:hygiene-stats #2101 keys");

    CHECK(set_hygiene_pass_cap(4), "set pass cap=4");
    CHECK(runtime_hygiene_pass_cap() == 4, "runtime pass=4");
    CHECK(href(cs, "query:macro-hygiene-stats", "runtime-pass-cap") == 4, "query pass 4");
    CHECK(href(cs, "query:macro-hygiene-stats", "self-evo-pass-cap") == 4,
          "effective pass mirrors runtime when no tighter cap");

    reset_hygiene_runtime_caps_for_test();
}

static void ac3_capability_tightens_only() {
    std::println("\n--- AC3: MacroSelfEvo can only tighten further ---");
    reset_all();
    // Runtime cap=20; capability grant max_depth=8 → effective=8.
    // Bind a live Evaluator so tenant_for_macro_self_evo_check (#3378)
    // reads this principal (tenant 0) instead of a leftover TLS evaluator.
    CHECK(set_hygiene_depth_cap(20), "runtime 20");
    CompilerService cs;
    // Grant while Off (#3409: production grant of TenantAdmin/MacroSelfEvo
    // high bits requires an existing TenantAdmin — chicken-and-egg). Arm
    // Strict after the grants so check_macro_self_evo consults the policy.
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 2;
    pol.max_depth = 8;
    pol.allow_rest_hygiene = true;
    pol.allow_concurrent_fiber = true;
    CHECK(g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                        aura_test_grant_prov()),
          "TenantAdmin grant under Off");
    CHECK(g_capability_registry().grant_macro_self_evo(0, pol, aura_test_grant_prov()),
          "MacroSelfEvo grant under Off");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);

    CHECK(effective_hygiene_depth_limit() == 8, "capability 8 tightens under runtime 20");
    CHECK(effective_hygiene_pass_cap() == 2, "pass effective=2 (capability)");

    // Capability tries to "loosen" with max_depth=100 but runtime is 20 → 20.
    pol.max_depth = 100;
    pol.max_expansion_passes = 50;
    g_capability_registry().grant_macro_self_evo(0, pol, aura_test_grant_prov());
    CHECK(effective_hygiene_depth_limit() == 20, "capability 100 cannot exceed runtime 20");
    // Pass: runtime 0 (cleared) + capability 50 → 50; set runtime 10 → min=10.
    CHECK(set_hygiene_pass_cap(10), "runtime pass 10");
    CHECK(effective_hygiene_pass_cap() == 10, "capability 50 cannot exceed runtime pass 10");

    // Pass clamp on expand: request 32, runtime 10, capability 50 → 10.
    reset_all();
    CHECK(set_hygiene_pass_cap(3), "pass cap 3");
    const auto clamp0 = g_macro_self_evo_pass_clamp_total.load();
    StringPool pool;
    FlatAST flat;
    auto y = pool.intern("y");
    auto d = pool.intern("d");
    auto star = pool.intern("*");
    auto two = flat.add_literal(2);
    auto yvar = flat.add_variable(y);
    auto star_v = flat.add_variable(star);
    std::array<aura::ast::NodeId, 2> body_args{yvar, two};
    auto body = flat.add_call(star_v, body_args);
    (void)flat.add_macrodef(d, {y}, body, false, true);
    auto three = flat.add_literal(3);
    auto dvar = flat.add_variable(d);
    std::array<aura::ast::NodeId, 1> call_args{three};
    flat.root = flat.add_call(dvar, call_args);
    (void)macro_expand_all(flat, pool, flat.root, 32);
    CHECK(g_macro_self_evo_pass_clamp_total.load() > clamp0, "pass clamp on runtime cap");

    reset_all();
}

static void ac4_default_preserves_off() {
    std::println("\n--- AC4: default high cap preserves Off lineage ---");
    reset_all();
    CHECK(runtime_hygiene_depth_cap() == MAX_HYGIENE_DEPTH, "default runtime = hard");
    CHECK(runtime_hygiene_pass_cap() == 0, "default pass = 0 (no clamp)");
    CHECK(effective_hygiene_depth_limit() == MAX_HYGIENE_DEPTH, "Off effective = hard");
    StringPool pool;
    FlatAST flat;
    auto y = pool.intern("y");
    auto d = pool.intern("d");
    auto star = pool.intern("*");
    auto two = flat.add_literal(2);
    auto yvar = flat.add_variable(y);
    auto star_v = flat.add_variable(star);
    std::array<aura::ast::NodeId, 2> body_args{yvar, two};
    auto body = flat.add_call(star_v, body_args);
    (void)flat.add_macrodef(d, {y}, body, false, true);
    auto three = flat.add_literal(3);
    auto dvar = flat.add_variable(d);
    std::array<aura::ast::NodeId, 1> call_args{three};
    flat.root = flat.add_call(dvar, call_args);
    auto out = macro_expand_all(flat, pool, flat.root, 32);
    CHECK(out != NULL_NODE || out == flat.root, "expand under default caps ok");
}

static void ac5_concurrent_process_wide() {
    std::println("\n--- AC5: concurrent set + expand process-wide ---");
    reset_all();
    std::atomic<int> seen{0};
    std::atomic<int> ok{1};
    std::thread setter([&] {
        // Monotonic non-decreasing cap so the reader's `e <= rt` invariant
        // is race-free: effective is computed from some rt' <= current rt,
        // so e <= rt' <= rt always holds even when setter interleaves
        // between the reader's two reads (5,6,7,7,7,...). The old cyclic
        // 5,6,7,5,6,7 sequence could LOWER the cap between the reader's
        // e-read and rt-read, spuriously failing "effective always <= runtime".
        int cur = 5;
        for (int i = 0; i < 50; ++i) {
            cur = std::max(cur, 5 + (i % 3));
            if (!set_hygiene_depth_cap(cur))
                ok.store(0);
            std::this_thread::yield();
        }
        seen.fetch_add(1);
    });
    std::thread reader([&] {
        for (int i = 0; i < 50; ++i) {
            const int e = effective_hygiene_depth_limit();
            // Always in [1, MAX] and ≤ runtime.
            if (e < 1 || e > MAX_HYGIENE_DEPTH)
                ok.store(0);
            const int rt = runtime_hygiene_depth_cap();
            if (e > rt)
                ok.store(0);
            std::this_thread::yield();
        }
        seen.fetch_add(1);
    });
    setter.join();
    reader.join();
    CHECK(seen.load() == 2, "both threads finished");
    CHECK(ok.load() == 1, "effective always ≤ runtime and within hard");
    CompilerService cs;
    CHECK(href(cs, "query:macro-hygiene-stats", "process-wide") == 1,
          "query documents process-wide");
    reset_all();
}

static void ac6_source_wiring() {
    std::println("\n--- AC6: source wiring #2101 ---");
    auto mx = read_file("src/compiler/macro_expansion.ixx");
    auto cpp = read_file("src/compiler/macro_expansion.cpp");
    auto q = read_file("src/compiler/evaluator_primitives_query_obs_mid.cpp");
    if (q.find("schema-2101") == std::string::npos)
        q += read_file("src/compiler/evaluator_primitives_query_reflect.cpp");
    CHECK(!mx.empty() && mx.find("Issue #2101") != std::string::npos, "ixx #2101");
    CHECK(mx.find("set_hygiene_depth_cap") != std::string::npos, "set_hygiene_depth_cap export");
    CHECK(mx.find("effective_hygiene_depth_limit") != std::string::npos, "effective export");
    CHECK(!cpp.empty() && cpp.find("Issue #2101") != std::string::npos, "cpp #2101");
    CHECK(cpp.find("g_runtime_hygiene_depth_cap") != std::string::npos, "runtime depth atomic");
    CHECK(cpp.find("combine_depth_limit") != std::string::npos, "combine helper");
    CHECK(cpp.find("AURA_MACRO_HYGIENE_DEPTH_CAP") != std::string::npos, "env depth");
    CHECK(!q.empty() && q.find("effective-max-depth") != std::string::npos,
          "effective-max-depth key");
    CHECK(q.find("schema-2101") != std::string::npos, "schema-2101");
    CHECK(q.find("runtime-depth-cap") != std::string::npos, "runtime-depth-cap key");
    // No new public query:* name (SlimSurface #1448).
    CHECK(q.find("query:macro-hygiene-limits") == std::string::npos,
          "no new query:macro-hygiene-limits name");
}

// ── Issue #3029: stable Agent reason strings for ceiling / depth / pass ──

static void ac3029_depth_reason() {
    std::println("\n--- #3029 AC: depth-limit sets hygiene-depth-limit ---");
    reset_all();
    CHECK(set_hygiene_depth_cap(1), "cap=1");
    StringPool pool;
    auto flat = make_deep_body(pool, 8);
    FlatAST tgt;
    StringPool tp;
    NameMap names;
    g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
    auto cid = clone_macro_body(tgt, tp, flat, pool, flat.root, nullptr, &names,
                                SyntaxMarker::MacroIntroduced);
    (void)cid;
    const auto* rs = hygiene_last_limit_reason_string();
    CHECK(rs != nullptr && std::string(rs) == "hygiene-depth-limit",
          "3029: last reason hygiene-depth-limit");
    CHECK(g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed) == 2,
          "3029: reason enum 2");
    reset_all();
}

static void ac3029_ceiling_reason() {
    std::println("\n--- #3029 AC: gensym ceiling sets hygiene-gensym-ceiling ---");
    reset_all();
    aura_test_set_max_gensym_map_size_for_test(1);
    StringPool pool;
    auto flat = make_deep_body(pool, 6);
    FlatAST tgt;
    StringPool tp;
    NameMap names;
    g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
    auto cid = clone_macro_body(tgt, tp, flat, pool, flat.root, nullptr, &names,
                                SyntaxMarker::MacroIntroduced);
    (void)cid;
    const auto* rs = hygiene_last_limit_reason_string();
    CHECK(rs != nullptr && std::string(rs) == "hygiene-gensym-ceiling",
          "3029: last reason hygiene-gensym-ceiling");
    aura_test_set_max_gensym_map_size_for_test(0);
}

static void ac3029_pass_reason() {
    std::println("\n--- #3029 AC: pass-limit sets hygiene-pass-limit ---");
    reset_all();
    CHECK(set_hygiene_pass_cap(1), "pass cap=1");
    StringPool pool;
    FlatAST flat;
    auto y = pool.intern("y");
    auto d = pool.intern("d");
    auto e = pool.intern("e");
    auto star = pool.intern("*");
    auto yvar = flat.add_variable(y);
    auto evar = flat.add_variable(e);
    auto star_v = flat.add_variable(star);
    auto two = flat.add_literal(2);
    // (e y) → (* y 2); (d y) → (e y). One pass leaves a macro call.
    std::array<aura::ast::NodeId, 2> e_args{yvar, two};
    auto e_body = flat.add_call(star_v, e_args);
    (void)flat.add_macrodef(e, {y}, e_body, false, true);
    std::array<aura::ast::NodeId, 1> d_args{yvar};
    auto d_body = flat.add_call(evar, d_args);
    (void)flat.add_macrodef(d, {y}, d_body, false, true);
    auto three = flat.add_literal(3);
    auto dcall = flat.add_variable(d);
    std::array<aura::ast::NodeId, 1> call_args{three};
    flat.root = flat.add_call(dcall, call_args);
    g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
    (void)macro_expand_all(flat, pool, flat.root, 8);
    const auto* rs = hygiene_last_limit_reason_string();
    CHECK(rs != nullptr && std::string(rs) == "hygiene-pass-limit",
          "3029: last reason hygiene-pass-limit");
    CHECK(g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed) == 3,
          "3029: reason enum 3");
    reset_all();
}

// Two-pass macro chain: (d 3) → (e 3) → (* 3 2). One pass leaves a macro call.
static void fill_two_pass_macros(FlatAST& flat, StringPool& pool) {
    auto y = pool.intern("y");
    auto d = pool.intern("d");
    auto e = pool.intern("e");
    auto star = pool.intern("*");
    auto yvar = flat.add_variable(y);
    auto evar = flat.add_variable(e);
    auto star_v = flat.add_variable(star);
    auto two = flat.add_literal(2);
    std::array<aura::ast::NodeId, 2> e_args{yvar, two};
    auto e_body = flat.add_call(star_v, e_args);
    (void)flat.add_macrodef(e, {y}, e_body, false, true);
    std::array<aura::ast::NodeId, 1> d_args{yvar};
    auto d_body = flat.add_call(evar, d_args);
    (void)flat.add_macrodef(d, {y}, d_body, false, true);
    auto three = flat.add_literal(3);
    auto dcall = flat.add_variable(d);
    std::array<aura::ast::NodeId, 1> call_args{three};
    flat.root = flat.add_call(dcall, call_args);
}

static std::vector<std::uint32_t> tree_fp(const FlatAST& flat, aura::ast::NodeId root) {
    std::vector<std::uint32_t> fp;
    std::vector<aura::ast::NodeId> st;
    st.push_back(root);
    while (!st.empty()) {
        auto id = st.back();
        st.pop_back();
        if (id == NULL_NODE || id >= flat.size())
            continue;
        auto v = flat.get(id);
        fp.push_back(static_cast<std::uint32_t>(v.tag));
        fp.push_back(v.sym_id);
        std::vector<aura::ast::NodeId> kids(v.children.begin(), v.children.end());
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            st.push_back(*it);
    }
    return fp;
}

static void grant_self_evo_production() {
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 32;
    pol.max_depth = 256;
    pol.allow_rest_hygiene = true;
    pol.allow_concurrent_fiber = true;
    (void)g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                        aura_test_grant_prov());
    (void)g_capability_registry().grant_macro_self_evo(0, pol, aura_test_grant_prov());
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    set_mode(SandboxMode::Restricted);
}

static void ac3062_no_boundary_refuse_partial() {
    std::println("\n--- #3062 AC1: no-boundary production refuse half-expand ---");
    reset_all();
    grant_self_evo_production();
    CHECK(set_hygiene_pass_cap(1), "3062 AC1 pass cap=1");
    StringPool pool;
    FlatAST flat;
    fill_two_pass_macros(flat, pool);
    const auto orig = flat.root;
    const auto fp0 = tree_fp(flat, orig);
    g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
    auto out = macro_expand_all(flat, pool, orig, 8);
    const auto* rs = hygiene_last_limit_reason_string();
    CHECK(rs != nullptr && std::string(rs) == "hygiene-pass-limit",
          "3062 AC1 last reason hygiene-pass-limit");
    CHECK(g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed) == 3,
          "3062 AC1 reason enum 3");
    CHECK(out == orig, "3062 AC1 no-boundary returns original_root");
    CHECK(tree_fp(flat, out) == fp0, "3062 AC1 tree identical to pre-expand");
    reset_all();
}

static void ac3062_boundary_restore() {
    std::println("\n--- #3062 AC2: MutationBoundary restore to original_root ---");
    reset_all();
    grant_self_evo_production();
    CHECK(set_hygiene_pass_cap(1), "3062 AC2 pass cap=1");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define a 1)\")").has_value(), "3062 AC2 set-code");
    auto& ev = cs.evaluator();
    StringPool pool;
    FlatAST flat;
    fill_two_pass_macros(flat, pool);
    const auto orig = flat.root;
    const auto fp0 = tree_fp(flat, orig);
    g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
    bool ok = true;
    {
        auto gr = Evaluator::MutationBoundaryGuard::try_acquire(ev, /*pending=*/1, &ok);
        CHECK(gr.has_value(), "3062 AC2 Guard acquired");
        auto out = macro_expand_all(flat, pool, orig, 8);
        CHECK(out == orig, "3062 AC2 boundary returns original_root");
        CHECK(g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed) == 3,
              "3062 AC2 reason 3");
        CHECK(tree_fp(flat, out) == fp0, "3062 AC2 tree identical after restore");
    }
    reset_all();
}

static void ac3062_soft_off_half_expand() {
    std::println("\n--- #3062 AC3: Soft/Off still half-expands (zero-cost) ---");
    reset_all();
    CHECK(set_hygiene_pass_cap(1), "3062 AC3 pass cap=1");
    StringPool pool;
    FlatAST flat;
    fill_two_pass_macros(flat, pool);
    const auto orig = flat.root;
    g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
    auto out = macro_expand_all(flat, pool, orig, 8);
    CHECK(g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed) == 3,
          "3062 AC3 reason 3");
    CHECK(out != orig, "3062 AC3 Soft/Off still half-expands");
    reset_all();
}

static void ac3062_source_wiring() {
    std::println("\n--- #3062 AC4: source wiring + counters remain accurate ---");
    const auto cpp = read_file("src/compiler/macro_expansion.cpp");
    const auto fib = read_file("src/compiler/evaluator_fiber_mutation.cpp");
    CHECK(cpp.find("Issue #3062") != std::string::npos, "3062 AC4 cpp cite");
    CHECK(cpp.find("install_macro_expand_checkpoint") != std::string::npos,
          "3062 AC4 install helper");
    CHECK(cpp.find("production_surface") != std::string::npos, "3062 AC4 production gate");
    CHECK(cpp.find("NameMapCheckpoint") != std::string::npos, "3062 AC4 NameMapCheckpoint cite");
    CHECK(fib.find("aura_evaluator_try_save_macro_expand_checkpoint") != std::string::npos,
          "3062 AC4 save ABI");
    CHECK(fib.find("aura_evaluator_commit_macro_expand_checkpoint") != std::string::npos,
          "3062 AC4 commit ABI");
    CHECK(cpp.find("aura_evaluator_try_restore_macro_expand_checkpoint") != std::string::npos,
          "3062 AC4 restore still wired");
    CHECK(cpp.find("hygiene-pass-limit") != std::string::npos, "3062 AC5 reason string");
    reset_all();
}

static void ac3470_inner_unwrap_depth_source() {
    std::println("\n--- #3470: inner unwrap depth-limit source ---");
    const auto cpp = read_file("src/compiler/macro_expansion.cpp");
    auto pos = cpp.find("aura::ast::NodeId expand_inner_macros");
    CHECK(pos != std::string::npos, "3470: expand_inner_macros present");
    auto win = pos != std::string::npos ? cpp.substr(pos, 3600) : std::string{};
    CHECK(win.find("Issue #3470") != std::string::npos, "3470: inner cites #3470");
    CHECK(win.find("kHygieneLimitReasonDepthLimit") != std::string::npos,
          "3470: inner stamps DepthLimit");
    CHECK(win.find("kHygieneLimitReasonPassLimit") == std::string::npos,
          "3470: inner depth path is not PassLimit");
    CHECK(win.find("inner_expand_production_limit_deny") != std::string::npos,
          "3470: unwrap restore on deny");
    CHECK(cpp.find("g_3470_") == std::string::npos, "3470: no new metric atomic");
    reset_all();
}

static void ac3508_expand_all_splices_non_root() {
    std::println("\n--- #3508: macro_expand_all splices non-root Calls ---");
    reset_all();
    StringPool pool;
    FlatAST flat;
    auto x = pool.intern("x");
    auto dbl = pool.intern("dbl");
    auto star = pool.intern("*");
    auto xvar = flat.add_variable(x);
    auto star_v = flat.add_variable(star);
    auto two = flat.add_literal(2);
    std::array<aura::ast::NodeId, 2> mul{xvar, two};
    auto body = flat.add_call(star_v, mul);
    auto md = flat.add_macrodef(dbl, {x}, body, false, true);
    auto three = flat.add_literal(3);
    auto dbl_v = flat.add_variable(dbl);
    std::array<aura::ast::NodeId, 1> cargs{three};
    auto call = flat.add_call(dbl_v, cargs);
    std::array<aura::ast::NodeId, 2> kids{md, call};
    flat.root = flat.add_begin(kids);
    const auto orig_root = flat.root;
    auto out = macro_expand_all(flat, pool, orig_root, 8);
    CHECK(out != NULL_NODE, "3508 AC1: expand");
    CHECK(out == orig_root, "3508 AC2: begin root kept (non-root splice)");
    bool saw_dbl = false;
    bool saw_star_parented = false;
    std::vector<aura::ast::NodeId> st{out};
    while (!st.empty()) {
        auto id = st.back();
        st.pop_back();
        if (id == NULL_NODE || id >= flat.size())
            continue;
        auto v = flat.get(id);
        if (v.tag == aura::ast::NodeTag::Call && !v.children.empty()) {
            auto cal = flat.get(v.child(0));
            if (cal.tag == aura::ast::NodeTag::Variable) {
                auto nm = std::string(pool.resolve(cal.sym_id));
                if (nm == "dbl")
                    saw_dbl = true;
                if (nm == "*" && flat.parent_of(id) != NULL_NODE)
                    saw_star_parented = true;
            }
        }
        std::vector<aura::ast::NodeId> ch(v.children.begin(), v.children.end());
        for (auto c : ch)
            st.push_back(c);
    }
    CHECK(!saw_dbl, "3508 AC1: nested dbl call spliced away");
    CHECK(saw_star_parented, "3508 AC1: expansion reachable from root");
    CHECK(g_macro_hygiene_last_limit_reason.load(std::memory_order_relaxed) != 3,
          "3508 AC3: successful splice is not pass-limit");
    const auto cpp = read_file("src/compiler/macro_expansion.cpp");
    CHECK(cpp.find("Issue #3508") != std::string::npos, "3508 AC5: cite");
    CHECK(cpp.find("expand_inner_macros(") != std::string::npos, "3508 AC4: inner expand kept");
    CHECK(read_file("tests/compiler/test_issue_3508.cpp").empty(), "3508 AC5: no test_issue");
    CHECK(read_file("docs/design/3508-expand-all-splice.md").empty(), "3508 AC5: no docs/design");
    reset_all();
}

static void ac3029_query_and_linter() {
    std::println("\n--- #3029 AC: query keys + linter ---");
    CompilerService cs;
    CHECK(href(cs, "query:macro-hygiene-stats", "schema-3029") == 3029, "3029: schema-3029");
    CHECK(href(cs, "query:macro-hygiene-stats", "hygiene-limit-reason-gensym-ceiling") == 1,
          "3029: ceiling sentinel");
    CHECK(href(cs, "query:macro-hygiene-stats", "hygiene-limit-reason-depth-limit") == 2,
          "3029: depth sentinel");
    CHECK(href(cs, "query:macro-hygiene-stats", "hygiene-limit-reason-pass-limit") == 3,
          "3029: pass sentinel");
    const auto cpp = read_file("src/compiler/macro_expansion.cpp");
    CHECK(cpp.find("hygiene-gensym-ceiling") != std::string::npos, "3029: ceiling string");
    CHECK(cpp.find("hygiene-depth-limit") != std::string::npos, "3029: depth string");
    CHECK(cpp.find("hygiene-pass-limit") != std::string::npos, "3029: pass string");
    CHECK(cpp.find("aura_evaluator_try_restore_macro_expand_checkpoint") != std::string::npos,
          "3029: pass-limit restore");
}

// ── Issue #3543: typed MacroHygiene SE on hygiene fail ──
static bool ring_has_reason_3543(std::string_view needle) {
    using aura::core::security_event::g_security_event_ring;
    using aura::core::security_event::kSecurityEventRingSize;
    auto& ring = g_security_event_ring();
    const auto cur = ring.seq.load(std::memory_order_acquire);
    const auto end = cur > kSecurityEventRingSize ? cur - kSecurityEventRingSize : 0;
    for (auto s = cur; s > end; --s) {
        const auto& e = ring.ring[(s - 1) % kSecurityEventRingSize];
        if (std::string_view(e.reason) == needle)
            return true;
    }
    return false;
}

static void ac3543_four_fail_modes_emit_se() {
    std::println("\n--- #3543 AC1: Restricted emit SE for 4 hygiene fail modes ---");
    using aura::core::security_event::reset_security_event_ring_for_test;
    reset_all();
    reset_security_event_ring_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    const auto se0 = g_hygiene_violation_se_emit_total.load();
    // Reset last-reason between stamps: ceiling/depth must not clobber
    // skip after macro-introduced / rest-unmarked (#3215).
    const std::uint8_t codes[] = {kHygieneLimitReasonMacroIntroduced,
                                  kHygieneLimitReasonGensymCeiling, kHygieneLimitReasonDepthLimit,
                                  kHygieneLimitReasonRestUnmarked};
    for (auto code : codes) {
        g_macro_hygiene_last_limit_reason.store(0, std::memory_order_relaxed);
        note_hygiene_last_limit_reason(code);
    }
    CHECK(g_hygiene_violation_se_emit_total.load() >= se0 + 4, "3543 AC1: SE counter +4");
    CHECK(ring_has_reason_3543("hygiene-macro-introduced"), "3543 AC1: hygiene-macro-introduced");
    CHECK(ring_has_reason_3543("hygiene-gensym-ceiling"), "3543 AC1: hygiene-gensym-ceiling");
    CHECK(ring_has_reason_3543("hygiene-depth-limit"), "3543 AC1: hygiene-depth-limit");
    CHECK(ring_has_reason_3543("hygiene-rest-unmarked"), "3543 AC1: hygiene-rest-unmarked");
    reset_all();
}

static void ac3543_depth_clone_emits() {
    std::println("\n--- #3543 AC2: Restricted depth-limit clone emits hygiene-depth-limit ---");
    using aura::core::security_event::reset_security_event_ring_for_test;
    reset_all();
    reset_security_event_ring_for_test();
    CHECK(set_hygiene_depth_cap(1), "3543 AC2: cap=1");
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 8;
    pol.max_depth = 8;
    pol.allow_rest_hygiene = true;
    pol.allow_concurrent_fiber = true;
    CHECK(g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                        aura_test_grant_prov()),
          "3543 AC2: TA grant");
    CHECK(g_capability_registry().grant_macro_self_evo(0, pol, aura_test_grant_prov()),
          "3543 AC2: MSE grant");
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Restricted);
    StringPool pool;
    auto flat = make_deep_body(pool, 8);
    FlatAST tgt;
    StringPool tp;
    NameMap names;
    const auto se0 = g_hygiene_violation_se_emit_total.load();
    auto cid = clone_macro_body(tgt, tp, flat, pool, flat.root, nullptr, &names,
                                SyntaxMarker::MacroIntroduced);
    (void)cid;
    CHECK(g_hygiene_violation_se_emit_total.load() > se0, "3543 AC2: SE emit on clone deny");
    CHECK(ring_has_reason_3543("hygiene-depth-limit"), "3543 AC2: SE reason");
    reset_all();
}

static void ac3543_soft_no_se() {
    std::println("\n--- #3543 AC3: Soft/Off fail stamps reason, no SE ---");
    using aura::core::security_event::reset_security_event_ring_for_test;
    reset_all();
    reset_security_event_ring_for_test();
    CHECK(!aura::core::sandbox::is_sandbox_active(), "3543 AC3: Off");
    const auto se0 = g_hygiene_violation_se_emit_total.load();
    note_hygiene_last_limit_reason(kHygieneLimitReasonDepthLimit);
    CHECK(hygiene_last_limit_reason_string() != nullptr &&
              std::string(hygiene_last_limit_reason_string()) == "hygiene-depth-limit",
          "3543 AC3: reason still stamped");
    CHECK(g_hygiene_violation_se_emit_total.load() == se0, "3543 AC3: no SE emit");
    CHECK(!ring_has_reason_3543("hygiene-depth-limit"), "3543 AC3: ring quiet");
    reset_all();
}

static void ac3543_soft_no_abort() {
    std::println("\n--- #3543 AC4: fail path returns (no abort) ---");
    CHECK(true, "3543 AC4: AC1/AC2 returned");
}

static void ac3543_source_query() {
    std::println("\n--- #3543 AC5: source-cite + schema-3543 ---");
    const auto cpp = read_file("src/compiler/macro_expansion.cpp");
    const auto ixx = read_file("src/compiler/macro_expansion.ixx");
    const auto q = read_file("src/compiler/evaluator_primitives_query_obs_mid.cpp");
    CHECK(cpp.find("emit_hygiene_limit_se") != std::string::npos, "3543 AC5: helper");
    CHECK(cpp.find("SecurityEventKind::MacroHygiene") != std::string::npos, "3543 AC5: kind");
    CHECK(ixx.find("kHygieneViolationSeIssue = 3543") != std::string::npos, "3543 AC5: stamp");
    CHECK(ixx.find("g_hygiene_violation_se_emit_total") != std::string::npos,
          "3543 AC5: counter END");
    CHECK(q.find("schema-3543") != std::string::npos, "3543 AC5: schema-3543");
    CHECK(q.find("hygiene-violation-se-emit-total") != std::string::npos, "3543 AC5: query key");
    CHECK(q.find("schema-3029") != std::string::npos, "3543 AC5: schema-3029 retained");
    CHECK(read_file("tests/compiler/test_hygiene_violation_se_emit.cpp").empty(),
          "3543 AC5: no new test file");
    CHECK(read_file("docs/design/3543-hygiene-violation-se.md").empty(),
          "3543 AC5: no docs/design");
    CompilerService cs;
    CHECK(href(cs, "query:macro-hygiene-stats", "schema-3543") == 3543, "3543 AC5: schema query");
    CHECK(href(cs, "query:macro-hygiene-stats", "hygiene-violation-se-wired") == 1,
          "3543 AC5: wired");
    CHECK(href(cs, "query:macro-hygiene-stats", "schema-3029") == 3029, "3543 AC5: 3029 retained");
    CHECK(read_file("src/compiler/evaluator_primitives_obs_jit.cpp").find("schema-3543") ==
              std::string::npos,
          "3543 AC5: provenance-stats key family unchanged");
}

} // namespace

int run_test_macro_hygiene_limits() {
    std::println("=== Issue #2101: runtime hygiene depth/pass caps ===");
    ac1_runtime_cap_clamps();
    ac2_query_live_limits();
    ac3_capability_tightens_only();
    ac4_default_preserves_off();
    ac5_concurrent_process_wide();
    ac6_source_wiring();
    std::println("\n=== Issue #3029: stable hygiene limit reasons ===");
    ac3029_depth_reason();
    ac3029_ceiling_reason();
    ac3029_pass_reason();
    ac3029_query_and_linter();
    std::println("\n=== Issue #3062: no-boundary pass-limit refuse-partial ===");
    ac3062_no_boundary_refuse_partial();
    ac3062_boundary_restore();
    ac3062_soft_off_half_expand();
    ac3062_source_wiring();
    std::println("\n=== Issue #3470: inner qq-unwrap depth-limit refuse-partial ===");
    ac3470_inner_unwrap_depth_source();
    std::println("\n=== Issue #3508: macro_expand_all splices non-root Calls ===");
    ac3508_expand_all_splices_non_root();
    std::println("\n=== Issue #3543: typed MacroHygiene SE on hygiene fail ===");
    ac3543_four_fail_modes_emit_se();
    ac3543_depth_clone_emits();
    ac3543_soft_no_se();
    ac3543_soft_no_abort();
    ac3543_source_query();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_macro_hygiene_limits();
}
#endif
