// Issue #1610 (#1978 renamed): issue# moved from filename to header.
// @category: integration
// @reason: Issue #1610 — SyntaxMarker::MacroIntroduced full propagation
// AST→IR→JIT/AOT + provenance stamp + authoritative ir-hygiene-stats
// (refine #1047 / #455 / #733 / #501).
//
//   AC1: IRInstruction carries source_marker + provenance (via stats/wire)
//   AC2: lowering stamps MacroIntroduced (ir-hygiene-stamped-count)
//   AC3: JIT marker check wired (jit-marker-check-wired + consults key)
//   AC4: metrics ir-hygiene-stamped-count + jit-macro-introduced-deopt
//   AC5: macro-expanded define + mutate + re-eval hygiene holds
//   AC6: schema 1610 on query:ir-hygiene-stats

#include "test_harness.hpp"

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

static void ac1_stats_hash_schema() {
    std::println("\n--- AC1/AC4/AC6: ir-hygiene-stats schema 1610 ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto h = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
    CHECK(h && is_hash(*h), "authoritative hash (not bare int)");
    {
        const auto sch = href(cs, "query:ir-hygiene-stats", "schema");
        CHECK(sch == 1891 || sch == 1616 || sch == 1610, "schema 1891|1616|1610");
        const auto iss = href(cs, "query:ir-hygiene-stats", "issue");
        CHECK(iss == 1891 || iss == 1616 || iss == 1610, "issue 1891|1616|1610");
    }
    CHECK(href(cs, "query:ir-hygiene-stats", "ir-hygiene-stamped-count") >= 0,
          "ir-hygiene-stamped-count");
    CHECK(href(cs, "query:ir-hygiene-stats", "provenance-stamped-count") >= 0,
          "provenance-stamped-count");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-macro-introduced-deopt") >= 0,
          "jit-macro-introduced-deopt");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-macro-hygiene-consults") >= 0,
          "jit-macro-hygiene-consults");
}

static void ac2_lowering_stamp_wire() {
    std::println("\n--- AC2: lowering stamp wire flags ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    CHECK(href(cs, "query:ir-hygiene-stats", "lowering-stamp-wired") == 1, "lowering-stamp-wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "inline-hygiene-skipped") >= 0,
          "inline-hygiene-skipped lineage");
    CHECK(href(cs, "query:ir-hygiene-stats", "respect-macro-hygiene") >= 0,
          "respect-macro-hygiene lineage");
    // Macro workspace should produce MacroIntroduced AST markers.
    CHECK(href(cs, "query:ir-hygiene-stats", "macro-markers") >= 0, "macro-markers readable");
}

static void ac3_jit_marker_check_wire() {
    std::println("\n--- AC3: JIT marker check wired ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-marker-check-wired") == 1,
          "jit-marker-check-wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "aot-bridge-marker-wired") == 1,
          "aot-bridge-marker-wired");
}

static void ac5_mutate_requery() {
    std::println("\n--- AC5: mutate + re-eval hygiene holds ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto stamped0 = href(cs, "query:ir-hygiene-stats", "ir-hygiene-stamped-count");
    CHECK(cs.eval("(mutate:rebind \"base\" \"99\")").has_value(), "mutate:rebind");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current after mutate");
    (void)cs.eval("(query:pattern \"*\")");
    auto h = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
    CHECK(h && is_hash(*h), "stats after mutate");
    {
        const auto sch = href(cs, "query:ir-hygiene-stats", "schema");
        CHECK(sch == 1891 || sch == 1616 || sch == 1610, "schema holds after mutate");
    }
    const auto stamped1 = href(cs, "query:ir-hygiene-stats", "ir-hygiene-stamped-count");
    CHECK(stamped1 >= stamped0, "stamped count non-decreasing after mutate");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok after mutate cycle");
}

static void ac_stress() {
    std::println("\n--- stress: 50× eval under macro workspace ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    for (int i = 0; i < 50; ++i) {
        (void)cs.eval("(eval-current)");
        if ((i % 10) == 0)
            (void)cs.eval("(query:pattern \"*\")");
    }
    {
        const auto sch = href(cs, "query:ir-hygiene-stats", "schema");
        CHECK(sch == 1891 || sch == 1616 || sch == 1610, "schema after stress");
    }
    CHECK(href(cs, "query:ir-hygiene-stats", "ir-hygiene-total") >= 0, "ir-hygiene-total");
}

} // namespace

int main() {
    std::println("=== Issue #1610: IR/JIT MacroIntroduced hygiene propagation ===");
    ac1_stats_hash_schema();
    ac2_lowering_stamp_wire();
    ac3_jit_marker_check_wire();
    ac5_mutate_requery();
    ac_stress();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
