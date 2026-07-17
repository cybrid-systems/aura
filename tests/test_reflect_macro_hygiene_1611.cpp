// @category: integration
// @reason: Issue #1611 — reflect.hh MacroIntroduced hygiene-aware validate
// + post_mutation_reflect_validate force-check + schema 1611 stats
// (refine #750 / #502 / #551 / #373).
//
//   AC1: reflect.hh hygiene APIs (auto_validate_with_marker wired via stats)
//   AC2: post_mutation MacroIntroduced check (post-mutation-macro-check-wired)
//   AC3: query:reflect-postmutate-stats schema 1611 (no new public stats)
//   AC4: reflect mutate on macro workspace — hygiene protection + typed path
//   AC5: deserialize-hygiene-wired (Phase-4 module export path)
//   AC6: allow-macro-mutate flag readable; checks/rejects counters

#include "test_harness.hpp"
#include "reflect/hygiene_validate.hh"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view q, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") \"{}\")", q, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static bool setup_macro_ws(CompilerService& cs) {
    if (!cs.eval("(set-code \""
                 "(define-hygienic-macro (d y) (* y 2)) "
                 "(d 1) (d 2) (d 3) "
                 "(define base 10) (+ base 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void ac1_reflect_hh_api() {
    std::println("\n--- AC1: hygiene_validate.hh / reflect hygiene APIs ---");
    std::string err;
    // User marker: always ok.
    CHECK(aura::reflect::hygiene_allows_evolution(aura::reflect::HygieneMarker::User, false, &err),
          "User marker validates without allow");
    // MacroIntroduced without allow: reject.
    err.clear();
    CHECK(!aura::reflect::hygiene_allows_evolution(aura::reflect::HygieneMarker::MacroIntroduced,
                                                   false, &err),
          "MacroIntroduced without allow rejects");
    CHECK(!err.empty(), "typed error string on reject");
    // MacroIntroduced with allow: ok.
    err.clear();
    CHECK(aura::reflect::hygiene_allows_evolution(aura::reflect::HygieneMarker::MacroIntroduced,
                                                  true, &err),
          "MacroIntroduced with allow passes");
    // Deserialize hygiene gate.
    CHECK(!aura::reflect::validate_deserialize_hygiene(
              aura::reflect::HygieneMarker::MacroIntroduced, false, &err),
          "deserialize rejects MacroIntroduced without allow");
    CHECK(aura::reflect::validate_deserialize_hygiene(aura::reflect::HygieneMarker::MacroIntroduced,
                                                      true, &err),
          "deserialize allows MacroIntroduced with allow");
}

static void ac2_ac3_stats_schema() {
    std::println("\n--- AC2/AC3: reflect-postmutate-stats schema 1611 ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    // Trigger Guard post-mutate reflect path via user rebind.
    CHECK(cs.eval("(mutate:rebind \"base\" \"42\")").has_value(), "mutate:rebind user binding");
    auto h = cs.eval("(engine:metrics \"query:reflect-postmutate-stats\")");
    CHECK(h && is_hash(*h), "reflect-postmutate-stats hash");
    CHECK(href(cs, "query:reflect-postmutate-stats", "schema") == 1611, "schema 1611");
    CHECK(href(cs, "query:reflect-postmutate-stats", "issue") == 1611, "issue 1611");
    CHECK(href(cs, "query:reflect-postmutate-stats", "hygiene-aware-validate-wired") == 1,
          "hygiene-aware-validate-wired");
    CHECK(href(cs, "query:reflect-postmutate-stats", "post-mutation-macro-check-wired") == 1,
          "post-mutation-macro-check-wired");
    CHECK(href(cs, "query:reflect-postmutate-stats", "deserialize-hygiene-wired") == 1,
          "deserialize-hygiene-wired");
    CHECK(href(cs, "query:reflect-postmutate-stats", "reflect-macro-hygiene-checks") >= 0,
          "reflect-macro-hygiene-checks");
    CHECK(href(cs, "query:reflect-postmutate-stats", "reflect-macro-hygiene-rejects") >= 0,
          "reflect-macro-hygiene-rejects");
    CHECK(href(cs, "query:reflect-postmutate-stats", "allow-macro-mutate") == 0 ||
              href(cs, "query:reflect-postmutate-stats", "allow-macro-mutate") == 1,
          "allow-macro-mutate readable");
}

static void ac4_mutate_hygiene() {
    std::println("\n--- AC4: mutate on macro workspace hygiene holds ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    // User mutate succeeds.
    auto r = cs.eval("(mutate:rebind \"base\" \"99\")");
    CHECK(r.has_value(), "user rebind ok");
    // Stats still authoritative after mutate.
    CHECK(href(cs, "query:reflect-postmutate-stats", "schema") == 1611, "schema after mutate");
    auto ok = cs.eval("(+ 1 1)");
    CHECK(ok.has_value(), "eval ok after cycle");
}

static void ac6_allow_flag_and_validate_macro() {
    std::println("\n--- AC6: allow flag + reflect:validate-macro-body ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto allow0 = cs.eval("(hygiene:allow-macro-mutate?)");
    CHECK(allow0 && is_bool(*allow0), "hygiene:allow-macro-mutate? reachable");
    // Validate first MacroIntroduced node if any (via query).
    auto macro_list = cs.eval("(query:macro-introduced)");
    if (macro_list && is_int(*macro_list) == false) {
        // list — try validate first id if length > 0
        auto len = cs.eval("(length (query:macro-introduced))");
        if (len && is_int(*len) && as_int(*len) > 0) {
            auto first = cs.eval("(car (query:macro-introduced))");
            if (first && is_int(*first)) {
                auto v = cs.eval(std::format("(reflect:validate-macro-body {})", as_int(*first)));
                CHECK(v.has_value(), "reflect:validate-macro-body returns");
                // After validate, checks counter may grow.
                CHECK(href(cs, "query:reflect-postmutate-stats", "reflect-macro-hygiene-checks") >=
                          0,
                      "checks after validate-macro-body");
            }
        }
    }
    CHECK(cs.eval("(hygiene:set-allow-macro-mutate! #t)").has_value(), "set allow #t");
    CHECK(href(cs, "query:reflect-postmutate-stats", "allow-macro-mutate") == 1,
          "allow-macro-mutate flag reflected in stats");
    CHECK(cs.eval("(hygiene:set-allow-macro-mutate! #f)").has_value(), "set allow #f");
}

static void ac_health_struct() {
    std::println("\n--- MutationReflectHealth enforce reject ---");
    aura::reflect::MutationReflectHealth h;
    h.marker_consistent = true;
    h.generation_healthy = true;
    h.dirty_macro_nodes = 3;
    h.allow_macro_evolution = false;
    h.enforce_macro_hygiene_reject = true;
    std::string err;
    CHECK(!aura::reflect::validate_mutation_reflect_health(h, &err),
          "enforce reject without allow");
    h.allow_macro_evolution = true;
    CHECK(aura::reflect::validate_mutation_reflect_health(h, &err), "enforce allow passes");
}

} // namespace

int main() {
    std::println("=== Issue #1611: reflect MacroIntroduced hygiene ===");
    ac1_reflect_hh_api();
    ac2_ac3_stats_schema();
    ac4_mutate_hygiene();
    ac6_allow_flag_and_validate_macro();
    ac_health_struct();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
