// @category: unit
// @reason: Issue #3378 \u2014 check_macro_self_evo at clone/expand-all was keyed
// by process default_tenant, not the live Evaluator's
// capability_tenant_id(). #3145 landed per-Evaluator caller_principal on
// the grant path; the check path (effective_hygiene_depth_limit /
// effective_hygiene_pass_cap / TopLevelMacroCapGuard / macro_expand_all)
// still consulted default_tenant so:
//   1. Evaluator A granted MacroSelfEvo on tenant 5; default_tenant is
//      0/1. clone / macro_expand_all on A consulted the wrong tenant
//      and denied even though A was granted.
//   2. default_tenant held a live grant; Evaluator B on tenant 6
//      with no grant passed TopLevelMacroCapGuard / macro_expand_all
//      because the check never looked at B's principal.
// Fix: add tenant_for_macro_self_evo_check() that resolves the live
// Evaluator via aura_evaluator_resolve_current_for_macro (steal-side
// provenance bridge at #2810) and reads capability_tenant_id(),
// falling back to default_tenant when no Evaluator (tests / CLI).
// Non-duplicative to #3145 (grant path), #3132 (choke-point coverage),
// #3029 (TenantAdmin fence), #2810 (steal repin bridge), #3362 (write order).
//
//   AC1: source cites the new helper tenant_for_macro_self_evo_check
//        + the 4 sites that use it (not default_tenant)
//   AC2: source cites the Evaluator forward-decl + capability_tenant_id
//        resolution + default_tenant fallback path
//   AC3: source cites the bridge reuse \u2014 aura_evaluator_resolve_current_for_macro
//        was already used for steal-side provenance (line 2313)
//   AC4: source cites wildcard_ok=false on the 4 sites (unchanged)
//   AC5: no docs/design/3378-*; no test_issue_3378.cpp per #1655 / #81967

#include "test_harness.hpp"

#include <cstring>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace {

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

static bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int run_test_macro_self_evo_check_evaluator_principal() {
    std::println(
        "=== Issue #3378: check_macro_self_evo at clone/expand-all uses Evaluator principal ===");
    CHECK(true, "3378: issue stamp");

    // \u2500\u2500 AC1: new helper + 4 sites use it (not default_tenant) \u2500\u2500
    {
        std::println("\n--- AC1: new helper + 4 sites ---");
        const auto me = read_file("src/compiler/macro_expansion.cpp");
        // New helper defined in macro_expansion.cpp.
        CHECK(contains(me, "tenant_for_macro_self_evo_check"),
              "AC1: new helper defined in macro_expansion.cpp");
        // Helper cites #3378 to anchor the regression contract.
        const auto helper_pos = me.find("tenant_for_macro_self_evo_check");
        const auto me_after =
            (helper_pos == std::string::npos) ? std::string{} : me.substr(helper_pos);
        CHECK(contains(me_after, "#3378"), "AC1: helper cites #3378");
        // Helper definition: resolves live Evaluator, reads capability_tenant_id,
        // falls back to default_tenant.
        CHECK(contains(me_after, "aura_evaluator_resolve_current_for_macro"),
              "AC1: helper uses existing resolve-current-Evaluator bridge");
        CHECK(contains(me_after, "capability_tenant_id()"),
              "AC1: helper reads capability_tenant_id()");
        CHECK(contains(me_after, "g_capability_registry().default_tenant.load()"),
              "AC1: helper falls back to default_tenant when no Evaluator");
        // 4 sites use the helper (not default_tenant directly).
        const auto helper_call_count = [](const std::string& s) -> std::size_t {
            std::size_t c = 0, pos = 0;
            while ((pos = s.find("tenant_for_macro_self_evo_check()", pos)) != std::string::npos) {
                ++c;
                pos += std::strlen("tenant_for_macro_self_evo_check()");
            }
            return c;
        };
        const std::size_t n = helper_call_count(me);
        CHECK(n >= 4, "AC1: helper called >= 4 times (effective_hygiene_depth_limit, "
                      "effective_hygiene_pass_cap, TopLevelMacroCapGuard, macro_expand_all)");
        // The 4 sites must NOT use default_tenant directly anymore.
        // (Helper itself uses default_tenant as fallback, so we check
        // that the call sites no longer load default_tenant before the
        // helper. The 4 sites are: lines 380, 395, 1505, 2743 in the
        // pre-fix code. The helper uses default_tenant as fallback \u2014
        // one occurrence. Pre-fix had 4 default_tenant occurrences at
        // the 4 sites. After fix, 1 occurrence (in the helper).)
        const auto default_tenant_count = [](const std::string& s) -> std::size_t {
            std::size_t c = 0, pos = 0;
            while ((pos = s.find("g_capability_registry().default_tenant.load()", pos)) !=
                   std::string::npos) {
                ++c;
                pos += std::strlen("g_capability_registry().default_tenant.load()");
            }
            return c;
        };
        const std::size_t dt = default_tenant_count(me);
        CHECK(dt == 1, "AC1: default_tenant.load() used exactly once (helper fallback only); "
                       "4 call sites now use the helper instead of direct default_tenant");
    }

    // \u2500\u2500 AC2: Evaluator forward-decl + capability_tenant_id resolution + default_tenant
    // fallback \u2500\u2500
    {
        std::println("\n--- AC2: Evaluator forward-decl + fallback ---");
        const auto me = read_file("src/compiler/macro_expansion.cpp");
        // Evaluator class forward-declared in aura::compiler namespace.
        CHECK(contains(me, "namespace aura::compiler {"), "AC2: aura::compiler namespace used");
        CHECK(contains(me, "class Evaluator;"), "AC2: Evaluator class forward-declared");
        // Helper casts void* to aura::compiler::Evaluator*.
        const auto helper_pos = me.find("tenant_for_macro_self_evo_check");
        const auto me_after_helper =
            (helper_pos == std::string::npos) ? std::string{} : me.substr(helper_pos);
        CHECK(contains(me_after_helper, "static_cast<aura::compiler::Evaluator*>(ev)"),
              "AC2: helper casts void* to aura::compiler::Evaluator*");
        CHECK(contains(me_after_helper, "tid != 0"), "AC2: helper checks for non-zero tenant id");
        // default_tenant fallback present.
        CHECK(contains(me_after_helper, "if (tid != 0)\n            return tid;"),
              "AC2: non-zero tenant id returned directly");
        CHECK(contains(me_after_helper, "return g_capability_registry().default_tenant.load();"),
              "AC2: default_tenant fallback when no Evaluator / zero tenant id");
    }

    // \u2500\u2500 AC3: bridge reuse \u2014 aura_evaluator_resolve_current_for_macro already used
    // \u2500\u2500
    {
        std::println("\n--- AC3: bridge reuse ---");
        const auto me = read_file("src/compiler/macro_expansion.cpp");
        // aura_evaluator_resolve_current_for_macro was already declared
        // as extern (steal-side provenance bridge at #2810).
        CHECK(contains(me, "extern \"C\" void* aura_evaluator_resolve_current_for_macro"),
              "AC3: resolve_current_for_macro extern declaration present");
        // Already used for steal-side provenance (aura_macro_provenance_repin_on_steal
        // at #2810). Both call sites use the same bridge.
        const auto steal_pos = me.find("aura_macro_provenance_repin_on_steal");
        const auto me_after_steal =
            (steal_pos == std::string::npos) ? std::string{} : me.substr(steal_pos);
        CHECK(contains(me_after_steal, "aura_evaluator_resolve_current_for_macro()"),
              "AC3: steal-side provenance site uses the same resolve bridge");
        // And the new helper also uses the same bridge.
        CHECK(contains(me, "tenant_for_macro_self_evo_check"),
              "AC3: new tenant helper present (uses the same resolve bridge)");
    }

    // \u2500\u2500 AC4: wildcard_ok=false on the 4 sites (unchanged) \u2500\u2500
    {
        std::println("\n--- AC4: wildcard_ok=false on the 4 sites ---");
        const auto me = read_file("src/compiler/macro_expansion.cpp");
        // Count wildcard_ok=false occurrences inside check_macro_self_evo
        // calls. Expected: 4 (one per call site).
        const auto wildcard_count = [](const std::string& s) -> std::size_t {
            std::size_t c = 0, pos = 0;
            while ((pos = s.find("wildcard_ok=*/false", pos)) != std::string::npos) {
                ++c;
                pos += std::strlen("wildcard_ok=*/false");
            }
            return c;
        };
        const std::size_t wn = wildcard_count(me);
        CHECK(wn >= 4,
              "AC4: wildcard_ok=false on >= 4 check_macro_self_evo call sites (unchanged)");
    }

    // \u2500\u2500 AC5: no docs/design/3378-*; no test_issue_3378.cpp \u2500\u2500
    {
        std::println("\n--- AC5: no docs/design/3378-*; no test_issue_3378.cpp ---");
        CHECK(read_file("docs/design/3378-macro-self-evo-check-evaluator-principal.md").empty(),
              "AC5: no docs/design/3378-* per #1655");
        CHECK(read_file("tests/compiler/test_issue_3378.cpp").empty(),
              "AC5: no test_issue_3378.cpp per #81967");
        CHECK(read_file("tests/issues/test_issue_3378.cpp").empty(),
              "AC5: no tests/issues/test_issue_3378.cpp (R1 abandoned scheme)");
    }

    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_macro_self_evo_check_evaluator_principal();
}
#endif
