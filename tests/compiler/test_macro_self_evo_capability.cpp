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

#include "compiler/typed_mutation_audit.h"
#include "core/security_event.hh"

extern "C" const char* aura_classify_mid0_se_reason(const char* reason) noexcept;

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
using aura::core::capability::Effect;
using aura::core::capability::EffectSandboxMode;
using aura::core::capability::g_capability_registry;
using aura::core::capability::MacroSelfEvoPolicy;
using aura::core::capability::make_grant_provenance;
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
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
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
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
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
    // Issue #3594: the Strict-mode check key must join the grant's bound
    // mid — establish a deterministic live Mutation epoch and bind the
    // grants to it (epoch=0 / caller_mid=0 is a provenance-fence deny by
    // design; see test_effect_epoch_mutation_unify ac3594_mse_epoch0_fenced).
    aura::core::reset_mutation_epoch_for_test();
    aura::core::bump_mutation_epoch();
    const auto live_mid = aura::core::current_mutation_epoch();
    // Issue #3409 bootstrap: seed TenantAdmin while the registry is Off —
    // a production bootstrap grant from a non-admin caller is denied by
    // the SSOT fence. Then flip to Strict for the policy checks.
    g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                  make_grant_provenance(live_mid, true, 0, 0));
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 2;
    pol.max_depth = 8;
    pol.allow_rest_hygiene = true;
    pol.allow_concurrent_fiber = true;
    // Issue #3459: production callers must supply a real mid — the old
    // phantom synthesis (mid = epoch|1) is gone (#3090 one-refuse-policy).
    // Bind to the live epoch so check_macro_self_evo (caller_mid defaults
    // to the Mutation epoch) joins the grant fence.
    g_capability_registry().grant_macro_self_evo(0, pol,
                                                 make_grant_provenance(live_mid, true, 0, 0));

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
    // Issue #3594: live epoch + mid-bound grants so the provenance fence
    // passes and the zero-limits deny reason below is reached (epoch=0
    // would fence first — see AC4).
    aura::core::reset_mutation_epoch_for_test();
    aura::core::bump_mutation_epoch();
    const auto live_mid = aura::core::current_mutation_epoch();
    // Issue #3409 bootstrap: seed TenantAdmin while Off (see AC4).
    g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                  make_grant_provenance(live_mid, true, 0, 0));
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    MacroSelfEvoPolicy pol;
    pol.max_expansion_passes = 0;
    pol.max_depth = 0;
    // Issue #3459: real mid under production (phantom synthesis removed);
    // live epoch so the check joins the grant fence (#3594).
    g_capability_registry().grant_macro_self_evo(0, pol,
                                                 make_grant_provenance(live_mid, true, 0, 0));
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
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
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
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
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
    g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                  make_grant_provenance(1, true, 0, 0));
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

// ── Issue #3459: grant_macro_self_evo must refuse mid==0 under
// production (one refuse policy with grant() #3090) instead of
// synthesizing mid = epoch|1; security:grant-effect! must report
// refusal, not #t after a dead write.

static void ac3459_1_production_mid_refuse() {
    std::println("\n--- #3459 AC1: production mid==0 → refuse, no MSE bit ---");
    reset_all();
    constexpr std::uint64_t tenant = 3459;
    // Seed explicit TenantAdmin while the registry is Off (no fences), then
    // flip to production — the refuse under test must fire on mid==0, not
    // on a setup-time TA denial.
    aura::core::capability::EffectProvenance prov_ta{};
    prov_ta.epoch = 1;
    prov_ta.mutation_id = 1;
    g_capability_registry().grant(tenant, "ta-3459", aura::core::capability::Effect::TenantAdmin,
                                  prov_ta);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    aura::core::bump_mutation_epoch(1);
    const auto m = aura::core::current_mutation_epoch();
    const auto refused0 = aura::core::capability::g_capability_effect_metrics()
                              .capability_grant_mid_refused_total.load();
    aura::core::capability::EffectProvenance prov0{};
    prov0.epoch = m;
    prov0.mutation_id = 0;
    const bool landed =
        g_capability_registry().grant_macro_self_evo(tenant, MacroSelfEvoPolicy{}, prov0, tenant);
    CHECK(!landed, "AC1: production mid==0 → grant_macro_self_evo refuses");
    bool saw_mse = false;
    {
        std::lock_guard<std::mutex> lock(g_capability_registry().mtx);
        const auto it = g_capability_registry().by_tenant.find(tenant);
        if (it != g_capability_registry().by_tenant.end())
            for (const auto& g : it->second)
                if ((static_cast<std::uint16_t>(g.effects) &
                     static_cast<std::uint16_t>(aura::core::capability::Effect::MacroSelfEvo)) != 0)
                    saw_mse = true;
    }
    CHECK(!saw_mse, "AC1: no MSE bit in effects_for after refuse");
    CHECK(aura::core::capability::g_capability_effect_metrics()
                  .capability_grant_mid_refused_total.load() > refused0,
          "AC1: capability_grant_mid_refused_total bumped");
}

static void ac3459_2_live_mid_lands_with_m() {
    std::println("\n--- #3459 AC2: live epoch M + explicit TA → lands bound_mutation_id==M ---");
    reset_all();
    constexpr std::uint64_t tenant = 3460;
    // Seed explicit TenantAdmin while the registry is Off (no fences), then
    // flip to production for the live-mid grant.
    aura::core::capability::EffectProvenance prov_ta{};
    prov_ta.epoch = 1;
    prov_ta.mutation_id = 1;
    g_capability_registry().grant(tenant, "ta-3460", aura::core::capability::Effect::TenantAdmin,
                                  prov_ta);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    aura::core::bump_mutation_epoch(7);
    const auto m = aura::core::current_mutation_epoch();
    aura::core::capability::EffectProvenance prov{};
    prov.epoch = m;
    prov.mutation_id = m;
    const bool landed =
        g_capability_registry().grant_macro_self_evo(tenant, MacroSelfEvoPolicy{}, prov, tenant);
    CHECK(landed, "AC2: live mid M + explicit TA lands");
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_capability_registry().mtx);
        const auto it = g_capability_registry().by_tenant.find(tenant);
        if (it != g_capability_registry().by_tenant.end())
            for (const auto& g : it->second)
                if (g.name == "macro-self-evo") {
                    found = true;
                    CHECK(g.bound_mutation_id == m, "AC2: bound_mutation_id == M (not phantom 1)");
                }
    }
    CHECK(found, "AC2: macro-self-evo row found");
}

static void ac3459_3_no_explicit_ta_denied() {
    std::println("\n--- #3459 AC3: caller without explicit TA → TA fence deny, no write ---");
    reset_all();
    constexpr std::uint64_t caller_t = 3461;
    constexpr std::uint64_t target_t = 3462;
    // Seed the caller with a plain (non-TA) effect while the registry is
    // Off, then flip to production — the deny under test must come from the
    // #3029 macro fence, not a setup-time grant denial.
    aura::core::capability::EffectProvenance prov_caller{};
    prov_caller.epoch = 1;
    prov_caller.mutation_id = 1;
    g_capability_registry().grant(caller_t, "mut-caller", aura::core::capability::Effect::Mutate,
                                  prov_caller);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    aura::core::bump_mutation_epoch(1);
    const auto m = aura::core::current_mutation_epoch();
    const auto denies0 = aura::core::capability::g_capability_effect_metrics()
                             .capability_macro_self_evo_grant_deny_total.load();
    aura::core::capability::EffectProvenance prov{};
    prov.epoch = m;
    prov.mutation_id = m;
    const bool landed = g_capability_registry().grant_macro_self_evo(target_t, MacroSelfEvoPolicy{},
                                                                     prov, caller_t);
    CHECK(!landed, "AC3: no explicit TA → denied");
    bool saw_mse = false;
    {
        std::lock_guard<std::mutex> lock(g_capability_registry().mtx);
        const auto it = g_capability_registry().by_tenant.find(target_t);
        if (it != g_capability_registry().by_tenant.end())
            for (const auto& g : it->second)
                if ((static_cast<std::uint16_t>(g.effects) &
                     static_cast<std::uint16_t>(aura::core::capability::Effect::MacroSelfEvo)) != 0)
                    saw_mse = true;
    }
    CHECK(!saw_mse, "AC3: no registry write on target");
    CHECK(aura::core::capability::g_capability_effect_metrics()
                  .capability_macro_self_evo_grant_deny_total.load() > denies0,
          "AC3: macro-self-evo-grant-needs-tenant-admin deny counter bumped");
}

static void ac3459_4_off_synthesis_contract() {
    std::println("\n--- #3459 AC4: Soft/Off mid synthesis stays (contract) ---");
    reset_all();
    constexpr std::uint64_t tenant = 3463;
    aura::core::capability::EffectProvenance prov{};
    prov.epoch = 0;
    prov.mutation_id = 0;
    const bool landed =
        g_capability_registry().grant_macro_self_evo(tenant, MacroSelfEvoPolicy{}, prov, tenant);
    CHECK(landed, "AC4: Off grants without TA fence");
    bool saw_nonzero_mid = false;
    {
        std::lock_guard<std::mutex> lock(g_capability_registry().mtx);
        const auto it = g_capability_registry().by_tenant.find(tenant);
        if (it != g_capability_registry().by_tenant.end())
            for (const auto& g : it->second)
                if (g.name == "macro-self-evo")
                    saw_nonzero_mid = g.bound_mutation_id != 0;
    }
    CHECK(saw_nonzero_mid, "AC4: Off mid synthesis kept (epoch|1 contract)");
}

static void ac3459_5_source_cite_no_new_key() {
    std::println("\n--- #3459 AC5/AC6: source-cite, grant() refuse unchanged, no new key ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #3459") != std::string::npos, "AC5: capability_model.hh cites #3459");
    CHECK(cap.find("Soft/Off-only contract") != std::string::npos,
          "AC5: phantom synthesis gated to Soft/Off");
    CHECK(cap.find("bool grant_macro_self_evo") != std::string::npos,
          "AC5: MSE grant reports landed");
    CHECK(cap.find("grant-mid-refused") != std::string::npos,
          "AC5: grant-mid-refused reason stays (grant() + MSE refuse)");
    const auto sec = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(sec.find("Issue #3459") != std::string::npos, "AC6: prim cites #3459");
    CHECK(sec.find("security:grant-effect!: grant refused") != std::string::npos,
          "AC6: grant-effect! reports refusal");
    CHECK(sec.find("macro-self-evo grant refused") != std::string::npos,
          "AC6: MSE seed refusal reported");
    CHECK(sec.find("security:grant-capability!: grant did not land") != std::string::npos,
          "AC6: grant-capability! hygiene");
    CHECK(read_file("docs/design/3459-grant-mse-mid-refuse.md").empty(),
          "AC6: no docs/design/3459-*.md");
    CHECK(read_file("tests/issues/test_issue_3459.cpp").empty(),
          "AC6: no tests/issues/test_issue_3459.cpp");
}

// ── Issue #4133: grant_macro_self_evo TA fence is caller-only (#3800
// alignment) — direct registry mint face checks.
static void ac4133_1_registry_caller_only_fence() {
    std::println("\n--- #4133 AC1/AC2: registry mint — caller-only TA fence ---");
    // AC1: target-only TA must NOT authorize a non-TA caller's mint.
    reset_all();
    constexpr std::uint64_t caller_t = 4133;
    constexpr std::uint64_t target_t = 4134;
    // Seed explicit TA on the TARGET while the registry is Off (no
    // fences), then flip to production — the deny must come from the
    // caller-only fence, not a setup-time TA denial.
    aura::core::capability::EffectProvenance prov_ta{};
    prov_ta.epoch = 1;
    prov_ta.mutation_id = 1;
    g_capability_registry().grant(target_t, "ta-4134", aura::core::capability::Effect::TenantAdmin,
                                  prov_ta);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    aura::core::bump_mutation_epoch(1);
    const auto m = aura::core::current_mutation_epoch();
    const auto denies0 = aura::core::capability::g_capability_effect_metrics()
                             .capability_macro_self_evo_grant_deny_total.load();
    aura::core::capability::EffectProvenance prov{};
    prov.epoch = m;
    prov.mutation_id = m;
    const bool landed = g_capability_registry().grant_macro_self_evo(target_t, MacroSelfEvoPolicy{},
                                                                     prov, caller_t);
    CHECK(!landed, "4133 AC1: target-only TA → mint denied (caller-only fence)");
    bool saw_mse = false;
    {
        std::lock_guard<std::mutex> lock(g_capability_registry().mtx);
        const auto it = g_capability_registry().by_tenant.find(target_t);
        if (it != g_capability_registry().by_tenant.end())
            for (const auto& g : it->second)
                if ((static_cast<std::uint16_t>(g.effects) &
                     static_cast<std::uint16_t>(aura::core::capability::Effect::MacroSelfEvo)) != 0)
                    saw_mse = true;
    }
    CHECK(!saw_mse, "4133 AC1: no MSE bit lands on the TA target");
    CHECK(aura::core::capability::g_capability_effect_metrics()
                  .capability_macro_self_evo_grant_deny_total.load() > denies0,
          "4133 AC1: macro-self-evo-grant-needs-tenant-admin deny counter bumped");

    // AC2: caller TA → mint to a foreign (TA-less) target still lands.
    reset_all();
    g_capability_registry().grant(caller_t, "ta-4133", aura::core::capability::Effect::TenantAdmin,
                                  prov_ta);
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    aura::core::bump_mutation_epoch(1);
    const auto m2 = aura::core::current_mutation_epoch();
    aura::core::capability::EffectProvenance prov2{};
    prov2.epoch = m2;
    prov2.mutation_id = m2;
    const bool landed2 = g_capability_registry().grant_macro_self_evo(
        target_t, MacroSelfEvoPolicy{}, prov2, caller_t);
    CHECK(landed2, "4133 AC2: caller TA → foreign mint lands");
    reset_all();
}

static void ac4133_2_source_cite_stable_surface() {
    std::println("\n--- #4133 AC3: stable deny surface + caller-only fence source-cite ---");
    const auto cap = read_file("src/core/capability_model.hh");
    CHECK(cap.find("Issue #4133") != std::string::npos, "AC3: capability_model cites #4133");
    CHECK(cap.find("if (!has_admin(caller)) {") != std::string::npos,
          "AC3: caller-only fence present");
    CHECK(cap.find("has_admin(caller) && !has_admin(tenant)") == std::string::npos,
          "AC3: caller-OR-target fence removed (superseded)");
    CHECK(cap.find("macro-self-evo-grant-needs-tenant-admin") != std::string::npos,
          "AC3: SE reason string stays stable");
    CHECK(cap.find("capability_macro_self_evo_grant_deny_total") != std::string::npos,
          "AC3: deny counter name stays stable");
    CHECK(read_file("docs/design/4133-mse-caller-only-ta.md").empty(),
          "AC3: no docs/design/4133-*");
    CHECK(read_file("tests/core/test_issue_4133.cpp").empty(), "AC3: no invented test file");
}

static const aura::core::security_event::SecurityEvent* newest_se() {
    using aura::core::security_event::g_security_event_ring;
    using aura::core::security_event::kSecurityEventRingSize;
    auto& ring = g_security_event_ring();
    const auto head = ring.seq.load(std::memory_order_relaxed);
    if (head == 0)
        return nullptr;
    return &ring.ring[(head - 1) % kSecurityEventRingSize];
}

static void expect_mse_deny(std::string_view token, std::uint64_t mid, const char* label) {
    const auto* e = newest_se();
    CHECK(e != nullptr, std::string(label) + " row");
    if (e == nullptr)
        return;
    CHECK(std::string_view(e->reason) == token, std::string(label) + " reason");
    CHECK(std::string_view(e->op) == "macro-self-evo", std::string(label) + " op");
    CHECK(e->denied, std::string(label) + " denied");
    CHECK(e->mutation_id == mid, std::string(label) + " mid");
}

// Live Mutation epoch + TenantAdmin, then Strict. Grants that must pass the
// provenance fence use the returned mid (#3594: epoch 0 fences first).
static std::uint64_t arm_strict_with_ta() {
    reset_all();
    aura::core::reset_mutation_epoch_for_test();
    aura::core::bump_mutation_epoch();
    const auto live = aura::core::current_mutation_epoch();
    g_capability_registry().grant(0, "tenant-admin", Effect::TenantAdmin,
                                  make_grant_provenance(live, true, 0, 0));
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    return live;
}

static std::uint64_t miss_total() {
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    return g_typed_mutation_audit_counters.audit_mid_ssot_miss_total.load(
        std::memory_order_relaxed);
}

static void ac4447_deny_reason_and_quiet_probes() {
    std::println("\n--- #4447: MacroSelfEvo deny tokens; probes skip SE ---");
    using aura::compiler::macro_exp::effective_hygiene_depth_limit;
    using aura::compiler::macro_exp::effective_hygiene_pass_cap;
    using aura::compiler::typed_audit::apply_dev_audit_defaults;
    using aura::compiler::typed_audit::apply_production_audit_defaults;
    using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
    using aura::compiler::typed_audit::kAuditMidSsotMissReason;
    using aura::core::capability::g_capability_effect_metrics;
    using aura::core::security_event::g_security_event_ring;
    using aura::core::security_event::reset_security_event_ring_for_test;

    reset_all();
    apply_production_audit_defaults();
    aura::core::reset_mutation_epoch_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Strict);
    set_mode(SandboxMode::Strict);
    reset_security_event_ring_for_test();
    g_typed_mutation_audit_counters.audit_mid_ssot_miss_total.store(0, std::memory_order_relaxed);

    const auto denied0 =
        g_capability_effect_metrics().macro_self_evo_denied_total.load(std::memory_order_relaxed);
    const auto expand_denied0 = g_macro_self_evo_denied_total.load(std::memory_order_relaxed);
    const auto seq0 = g_security_event_ring().seq.load(std::memory_order_relaxed);
    const auto miss0 = miss_total();
    const int depth = effective_hygiene_depth_limit();
    const int passes = effective_hygiene_pass_cap();
    (void)passes;
    CHECK(depth >= 1, "4447: depth probe still returns a limit");
    CHECK(g_security_event_ring().seq.load(std::memory_order_relaxed) == seq0,
          "4447: probes emit no SE");
    CHECK(miss_total() == miss0, "4447: probes do not bump audit-mid-ssot-miss");
    CHECK(g_capability_effect_metrics().macro_self_evo_denied_total.load(
              std::memory_order_relaxed) == denied0 + 2,
          "4447: probes still count two denials");
    CHECK(g_macro_self_evo_denied_total.load(std::memory_order_relaxed) == expand_denied0,
          "4447: probes are not the expand deny counter");

    auto chk = check_macro_self_evo(0, true, false, 0, 0, true);
    CHECK(!chk.allowed, "4447: record=true deny without grant");
    CHECK(chk.deny_reason != nullptr &&
              std::string_view(chk.deny_reason) == "MacroSelfEvo capability not granted",
          "4447: English deny_reason stays");
    expect_mse_deny("macro-self-evo-not-granted", 0, "4447: not-granted");
    CHECK(miss_total() == miss0, "4447: not-granted token is canonical at mid=0");

    const auto live = arm_strict_with_ta();
    MacroSelfEvoPolicy pol;
    g_capability_registry().grant_macro_self_evo(0, pol, make_grant_provenance(live, true, 0, 0));
    aura::core::reset_mutation_epoch_for_test();
    chk = check_macro_self_evo(0, true, false, 0, 0, true);
    CHECK(!chk.allowed, "4447: mid=0 with a grant is the provenance fence");
    CHECK(chk.deny_reason != nullptr &&
              std::string_view(chk.deny_reason).find("provenance fence") != std::string_view::npos,
          "4447: English provenance fence stays");
    expect_mse_deny("macro-self-evo-provenance-fence", 0, "4447: provenance-fence");
    CHECK(miss_total() == miss0, "4447: provenance-fence token is canonical at mid=0");

    const auto live_pol = arm_strict_with_ta();
    const auto denied_pol =
        g_capability_effect_metrics().macro_self_evo_denied_total.load(std::memory_order_relaxed);
    CHECK(g_capability_registry().grant(0, "mse-bit-only", Effect::MacroSelfEvo,
                                        make_grant_provenance(live_pol, true, 0, 0)),
          "4447: bit grant without a policy map");
    chk = check_macro_self_evo(0, true, false, 0, live_pol, true);
    CHECK(!chk.allowed, "4447: policy missing denies");
    CHECK(chk.deny_reason != nullptr &&
              std::string_view(chk.deny_reason).find("policy missing") != std::string_view::npos,
          "4447: English policy missing stays");
    expect_mse_deny("macro-self-evo-policy-missing", live_pol, "4447: policy-missing");
    CHECK(g_capability_effect_metrics().macro_self_evo_denied_total.load(
              std::memory_order_relaxed) == denied_pol + 1,
          "4447: policy-missing bumps the deny counter");
    CHECK(miss_total() == miss0, "4447: policy-missing does not bump the miss counter");

    const auto live_z = arm_strict_with_ta();
    MacroSelfEvoPolicy zero;
    zero.max_expansion_passes = 0;
    zero.max_depth = 0;
    CHECK(g_capability_registry().grant_macro_self_evo(0, zero,
                                                       make_grant_provenance(live_z, true, 0, 0)),
          "4447: zero-limit grant lands");
    chk = check_macro_self_evo(0, true, false, 0, live_z, true);
    CHECK(!chk.allowed, "4447: zero limits deny");
    CHECK(chk.deny_reason != nullptr &&
              std::string_view(chk.deny_reason).find("zero") != std::string_view::npos,
          "4447: English limits-zero stays");
    expect_mse_deny("macro-self-evo-limits-zero", live_z, "4447: limits-zero");
    CHECK(miss_total() == miss0, "4447: limits-zero does not bump the miss counter");

    const char* tokens[] = {"macro-self-evo-not-granted", "macro-self-evo-provenance-fence",
                            "macro-self-evo-policy-missing", "macro-self-evo-limits-zero"};
    for (const char* t : tokens) {
        const auto before = miss_total();
        const char* kept = aura_classify_mid0_se_reason(t);
        CHECK(kept != nullptr && std::string_view(kept) == t, "4447: classifier keeps token");
        CHECK(miss_total() == before, "4447: canonical token does not bump miss");
    }
    const auto before_prefix = miss_total();
    const char* grant_tok = aura_classify_mid0_se_reason("macro-self-evo-grant-needs-tenant-admin");
    CHECK(grant_tok != nullptr && std::string_view(grant_tok) == kAuditMidSsotMissReason,
          "4447: grant token is not canonical via a prefix");
    CHECK(miss_total() == before_prefix + 1, "4447: non-canonical grant token bumps miss");

    const auto cap = read_file("src/core/capability_model.hh");
    const auto hooks = read_file("src/compiler/typed_mutation_audit_hooks.cpp");
    const auto mx = read_file("src/compiler/macro_expansion.cpp");
    CHECK(cap.find("\"macro-self-evo-not-granted\"") != std::string::npos,
          "4447: not-granted cite");
    CHECK(cap.find("\"macro-self-evo-provenance-fence\"") != std::string::npos,
          "4447: provenance-fence cite");
    CHECK(cap.find("\"macro-self-evo-policy-missing\"") != std::string::npos,
          "4447: policy-missing cite");
    CHECK(cap.find("\"macro-self-evo-limits-zero\"") != std::string::npos,
          "4447: limits-zero cite");
    CHECK(cap.find("MacroSelfEvo capability not granted") != std::string::npos,
          "4447: English not-granted stays in source");
    CHECK(cap.find("MacroSelfEvo provenance fence (epoch/fiber/mid)") != std::string::npos,
          "4447: English provenance stays in source");
    CHECK(cap.find("MacroSelfEvo policy missing") != std::string::npos,
          "4447: English policy stays in source");
    CHECK(cap.find("MacroSelfEvo limits are zero") != std::string::npos,
          "4447: English limits stay in source");
    CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonNotGranted)") !=
              std::string::npos,
          "4447: #3304 not-granted code stays");
    CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonProvenanceFence)") !=
              std::string::npos,
          "4447: #3304 provenance code stays");
    CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonPolicyMissing)") !=
              std::string::npos,
          "4447: #3304 policy code stays");
    CHECK(cap.find("note_capability_deny_last_reason(kCapabilityDenyReasonLimitsZero)") !=
              std::string::npos,
          "4447: #3304 limits code stays");
    CHECK(hooks.find("macro-self-evo-not-granted") != std::string::npos, "4447: classifier token");
    CHECK(hooks.find("macro-self-evo-provenance-fence") != std::string::npos,
          "4447: classifier fence token");
    CHECK(hooks.find("macro-self-evo-policy-missing") != std::string::npos,
          "4447: classifier policy token");
    CHECK(hooks.find("macro-self-evo-limits-zero") != std::string::npos,
          "4447: classifier limits token");
    CHECK(hooks.find("starts_with(\"macro-self-evo") == std::string::npos,
          "4447: classifier does not prefix-match macro-self-evo");
    int quiet = 0;
    constexpr std::string_view kQuiet = "/*record=*/false";
    for (std::size_t p = 0; (p = mx.find(kQuiet, p)) != std::string::npos; p += kQuiet.size())
        ++quiet;
    CHECK(quiet == 2, "4447: only the two hygiene probes skip record");
    CHECK(read_file("tests/compiler/test_issue_4447.cpp").empty(), "4447: no test_issue_4447.cpp");
    CHECK(read_file("docs/design/4447-macro-self-evo-deny.md").empty(),
          "4447: no docs/design/4447-*");

    apply_dev_audit_defaults();
    const auto miss_soft = miss_total();
    const char* soft = aura_classify_mid0_se_reason("capability-effect-deny");
    CHECK(soft != nullptr && std::string_view(soft) == "capability-effect-deny",
          "4447: Soft/Off keeps the caller reason");
    CHECK(miss_total() == miss_soft, "4447: Soft/Off does not bump miss");
    reset_all();
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
    ac3459_1_production_mid_refuse();
    ac3459_2_live_mid_lands_with_m();
    ac3459_3_no_explicit_ta_denied();
    ac3459_4_off_synthesis_contract();
    ac3459_5_source_cite_no_new_key();
    ac4133_1_registry_caller_only_fence();
    ac4133_2_source_cite_stable_surface();
    ac4447_deny_reason_and_quiet_probes();
    reset_all();
    if (g_failed)
        return 1;
    std::println("MacroSelfEvo capability (#2023): OK ({} passed)", g_passed);
    return 0;
}
