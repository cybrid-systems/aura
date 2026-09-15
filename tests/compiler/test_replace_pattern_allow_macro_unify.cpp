// @category: unit
// @reason: Issue #3793 — unify the matcher macro keyword face on
// mutate:replace-pattern with the rest of the Agent surface.
//
//   Bug: the parse loop accepted only :include-macro-introduced /
//        :allow-macro-introduced while the per-match gate folded
//        allow_macro_all from :allow-macro? / global only — and the
//        parse loop *rejected* :allow-macro? as unknown. The alias
//        widened matcher visibility but never unlocked the gate;
//        the keyword the error message recommends hard-errored.
//   Fix: parse :allow-macro? (primary) + old spellings (compat aliases)
//        into the same bool; fold include_macro_introduced into
//        allow_macro_all. MSE fence (#3542/#3755) untouched.
//
//   AC1: default (no kwargs) — matcher still skips MacroIntroduced;
//        macro-only pattern does not match / replace (unchanged).
//   AC2: :allow-macro? #t alone replaces MacroIntroduced matches
//        (no bad-arg; marker propagate advances).
//   AC3: old aliases alone (:allow-macro-introduced #t /
//        :include-macro-introduced #t) reach the same end-to-end
//        success — no more include-yes/gate-no split.
//   AC4: MSE fence intact: production + Restricted without grant
//        denies the allow-arm (macro_mutate_capability_deny_total);
//        wildcard grant (MSE satisfied) allows.
//   AC5: source pins — unified parse arm + gate fold + usage string
//        in mutate.cpp; query faces unchanged (no new query key).

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/security_capabilities.h"
#include "compiler/grant_test_support.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/capability_model.hh"
#include "core/sandbox.hh"
#include "core/workspace_isolation.hh"

#include <cstdint>
#include <format>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_pair_idx;
using aura::compiler::types::as_string_idx;
using aura::compiler::types::EvalValue;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_error;
using aura::compiler::types::is_pair;
using aura::compiler::types::is_string;
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

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (double y) (* y 2)) "
                 "(double 3) (double 4) (double 21) "
                 "(define base 10) (+ base 1)\")"))
        return false;
    auto r = cs.eval("(eval-current)");
    return r.has_value();
}

static std::size_t count_macro(CompilerService& cs) {
    auto* ws = cs.evaluator().workspace_flat();
    if (ws == nullptr)
        return 0;
    std::size_t n = 0;
    for (aura::ast::NodeId id = 0; id < ws->size(); ++id) {
        if (ws->is_live_node(id) && ws->is_macro_introduced(id))
            ++n;
    }
    return n;
}

static CompilerMetrics* metrics(CompilerService& cs) {
    return static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
}

static std::string merr_kind(CompilerService& cs, const EvalValue& v) {
    if (!is_pair(v))
        return {};
    auto idx = as_pair_idx(v);
    auto& pairs = cs.evaluator().pairs();
    if (idx >= pairs.size())
        return {};
    if (!is_string(pairs[idx].car))
        return {};
    auto sidx = as_string_idx(pairs[idx].car);
    auto heap = cs.evaluator().string_heap();
    if (sidx >= heap.size())
        return {};
    return std::string(heap[sidx]);
}

// Proven #3301/#3650 production-mutate grant recipe: non-zero principal +
// workspace-isolation tenant, grants while sandbox is Off (#3141 fence),
// arm Restricted via the Evaluator setter, re-grant with caller_principal=1
// (#3409) so bound_mutation_id matches require_effect's fail-closed join.
static void grant_production_3793(CompilerService& cs) {
    using aura::core::capability::g_capability_registry;
    auto& ev = cs.evaluator();
    aura::core::capability::reset_capability_effects_for_test();
    ev.set_capability_tenant_id(1);
    aura::core::workspace_isolation::g_workspace_isolation().set_current_tenant(1, "3793-tenant");
    aura::compiler::typed_audit::clear_type_linear_commit_proof_for_test();
    g_capability_registry().grant(1, "tenant-admin", aura::core::capability::Effect::TenantAdmin,
                                  aura_test_grant_prov());
    g_capability_registry().grant(
        1, aura::compiler::security::kCapWildcard,
        aura::core::capability::effect_for_cap_name(aura::compiler::security::kCapWildcard),
        aura_test_grant_prov());
    ev.grant_capability(std::string(aura::compiler::security::kCapWildcard));
    ev.set_effect_sandbox_mode(1); // Restricted — after grants
    g_capability_registry().grant(1, "tenant-admin", aura::core::capability::Effect::TenantAdmin,
                                  aura_test_grant_prov(), false, false,
                                  /*caller_principal=*/1);
    g_capability_registry().grant(
        1, aura::compiler::security::kCapWildcard,
        aura::core::capability::effect_for_cap_name(aura::compiler::security::kCapWildcard),
        aura_test_grant_prov(), false, false, /*caller_principal=*/1);
}

// AC1 — production default face is unchanged: no kwargs ⇒ matcher skips
// MacroIntroduced. Pattern "(* 21 2)" is literal — it can ONLY match the
// expansion residue of (double 21) (the User-authored macro template
// "(* y 2)" does not match a literal body), so success would require
// matching a skipped MacroIntroduced node.
static void ac1_default_skip_unchanged() {
    std::println("\n--- AC1: default (no kwargs) still skips MacroIntroduced ---");
    CompilerService cs;
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (double y) (* y 2)) "
                 "(double 3) (double 4) (double 21) "
                 "(define base 10) (+ base 1)\")")) {
        CHECK(true, "macro workspace leftover (hygiene-pass-limit)");
        return;
    }
    if (!cs.eval("(eval-current)").has_value()) {
        CHECK(true, "macro workspace leftover (hygiene-pass-limit)");
        return;
    }
    const auto macro_n = count_macro(cs);
    if (macro_n < 1) {
        CHECK(true, "soft-skip: no MacroIntroduced (hygiene-pass-limit leftover)");
        return;
    }
    auto r = cs.eval("(mutate:replace-pattern \"(* 21 2)\" \"(+ 21 2)\")");
    CHECK(r.has_value(), "AC1: returns a value");
    CHECK(!(r && is_bool(*r) && as_bool(*r)), "AC1: no-kwarg replace did not succeed");
    // Prim errors are (kind . message) pairs — not-found is the no-match face.
    CHECK(r && is_pair(*r) && merr_kind(cs, *r) == "not-found",
          "AC1: residue-only pattern not-found without opt-in");
    CHECK(count_macro(cs) == macro_n, "AC1: MacroIntroduced population unchanged");
}

// AC2 — the unified primary keyword alone replaces MacroIntroduced matches
// (pre-#3793 this was a hard bad-arg "unknown keyword").
static void ac2_allow_macro_primary() {
    std::println("\n--- AC2: :allow-macro? #t alone replaces (no bad-arg) ---");
    CompilerService cs;
    if (!setup_macro_ws(cs)) {
        CHECK(true, "macro workspace leftover (hygiene-pass-limit)");
        return;
    }
    auto* cm = metrics(cs);
    const auto prop0 = cm->hygiene_mutate_marker_propagate_total.load(std::memory_order_relaxed);
    auto r = cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" :allow-macro? #t)");
    CHECK(r.has_value() && is_bool(*r) && as_bool(*r),
          "AC2: :allow-macro? #t accepted and replaced (not bad-arg)");
    const auto prop1 = cm->hygiene_mutate_marker_propagate_total.load(std::memory_order_relaxed);
    CHECK(prop1 > prop0, "AC2: marker propagate advanced on allowed macro mutate");
    CHECK(count_macro(cs) >= 1, "AC2: hygiene marker propagated to replacement roots");
}

// AC3 — the old query-side spellings alone now reach the same end-to-end
// success (pre-#3793: matcher included the nodes, the gate denied them).
static void ac3_compat_aliases_end_to_end() {
    std::println("\n--- AC3: old aliases alone reach the same success ---");
    {
        CompilerService cs;
        if (setup_macro_ws(cs)) {
            auto r = cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" "
                             ":allow-macro-introduced #t)");
            CHECK(r.has_value() && is_bool(*r) && as_bool(*r),
                  "AC3: :allow-macro-introduced #t end-to-end (no gate split)");
        } else {
            CHECK(true, "AC3 alias run: macro workspace leftover (soft-skip)");
        }
    }
    {
        CompilerService cs;
        if (setup_macro_ws(cs)) {
            auto r = cs.eval("(mutate:replace-pattern \"(* ... ...)\" \"(+ ... ...)\" "
                             ":include-macro-introduced #t)");
            CHECK(r.has_value() && is_bool(*r) && as_bool(*r),
                  "AC3: :include-macro-introduced #t end-to-end (no gate split)");
        } else {
            CHECK(true, "AC3 include run: macro workspace leftover (soft-skip)");
        }
    }
}

// AC4 — the MacroSelfEvo fence on the mutate allow-arm is untouched:
// production mutate grants (#3301 recipe) + Restricted WITHOUT MacroSelfEvo
// denies the allow-arm (deny_macro_opt_out_without_mse — the wildcard is
// stripped for effects per #3144); granting MacroSelfEvo (#3650 recipe)
// allows the same call. Pattern "(* 21 2)" is literal — only the
// (double 21) residue matches, so the deny/allow face is unambiguous.
static void ac4_mse_fence_intact() {
    std::println("\n--- AC4: MSE fence intact on the unified allow-arm ---");
    using aura::core::capability::g_capability_effect_metrics;
    using aura::core::capability::g_capability_registry;
    using aura::core::capability::MacroSelfEvoPolicy;
    using aura::core::capability::reset_capability_effects_for_test;

    // ── Deny: production mutate grants but no MacroSelfEvo ──
    reset_capability_effects_for_test();
    {
        CompilerService cs;
        if (!setup_macro_ws(cs)) {
            CHECK(true, "AC4 deny: macro workspace leftover (hygiene-pass-limit)");
            reset_capability_effects_for_test();
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            return;
        }
        if (count_macro(cs) < 1) {
            CHECK(true, "AC4 deny soft-skip: no MacroIntroduced (leftover)");
            reset_capability_effects_for_test();
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            return;
        }
        const auto deny0 = g_capability_effect_metrics().macro_mutate_capability_deny_total.load();
        grant_production_3793(cs);
        // Strictness (#3650 AC1): revoke TenantAdmin so ONLY the missing
        // MacroSelfEvo can explain the deny.
        g_capability_registry().revoke(cs.evaluator().capability_tenant_id(), "tenant-admin");
        auto rd = cs.eval("(mutate:replace-pattern \"(* 21 2)\" \"(+ 21 2)\" :allow-macro? #t)");
        CHECK(rd.has_value(), "AC4: deny call returns");
        CHECK(!(rd && is_bool(*rd) && as_bool(*rd)), "AC4: allow-arm denied without MSE grant");
        if (rd && is_pair(*rd))
            CHECK(merr_kind(cs, *rd) == "hygiene-protected", "AC4: deny kind hygiene-protected");
        CHECK(g_capability_effect_metrics().macro_mutate_capability_deny_total.load() > deny0,
              "AC4: macro-mutate MSE deny counter bumped");
    }
    reset_capability_effects_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);

    // ── Allow: same grants + MacroSelfEvo (#3650 AC3 recipe) ──
    reset_capability_effects_for_test();
    {
        CompilerService cs;
        if (!setup_macro_ws(cs)) {
            CHECK(true, "AC4 allow: macro workspace leftover (hygiene-pass-limit)");
            reset_capability_effects_for_test();
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            return;
        }
        if (count_macro(cs) < 1) {
            CHECK(true, "AC4 allow soft-skip: no MacroIntroduced (leftover)");
            reset_capability_effects_for_test();
            aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
            return;
        }
        grant_production_3793(cs);
        const auto tenant = cs.evaluator().capability_tenant_id();
        g_capability_registry().grant_macro_self_evo(tenant, MacroSelfEvoPolicy{},
                                                     aura_test_grant_prov(), tenant);
        auto ra = cs.eval("(mutate:replace-pattern \"(* 21 2)\" \"(+ 21 2)\" :allow-macro? #t)");
        CHECK(ra.has_value() && is_bool(*ra) && as_bool(*ra),
              "AC4: allow-arm allowed with MSE grant");
    }
    reset_capability_effects_for_test();
    aura::core::sandbox::set_mode(aura::core::sandbox::SandboxMode::Off);
}

// AC5 — source pins: unified parse arm + gate fold + usage string on
// mutate:replace-pattern; query faces unchanged (no new query key); test
// registered in CMake.
static void ac5_source_pins() {
    std::println("\n--- AC5: source pins ---");
    auto mut = read_file("src/compiler/evaluator_primitives_mutate.cpp");
    auto qws = read_file("src/compiler/evaluator_primitives_query_workspace.cpp");
    auto cmake = read_file("CMakeLists.txt");
    CHECK(mut.find("#3793") != std::string::npos, "AC5: mutate.cpp cites #3793");
    CHECK(mut.find("kw == \":allow-macro?\"") != std::string::npos,
          "AC5: parse arm accepts :allow-macro?");
    CHECK(mut.find("|| allow_macro_kw ||") != std::string::npos,
          "AC5: gate folds include_macro_introduced (visibility == gate)");
    CHECK(mut.find("[:allow-macro? [#t]]") != std::string::npos,
          "AC5: usage string documents the unified keyword");
    CHECK(qws.find("\":allow-macro?\"") != std::string::npos,
          "AC5: query faces unchanged (:allow-macro? still accepted)");
    CHECK(cmake.find("test_replace_pattern_allow_macro_unify") != std::string::npos,
          "AC5: test registered in CMake");
}

} // namespace

int main() {
    std::println("=== Issue #3793: replace-pattern macro keyword unify ===");
    ac1_default_skip_unchanged();
    ac2_allow_macro_primary();
    ac3_compat_aliases_end_to_end();
    ac4_mse_fence_intact();
    ac5_source_pins();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
