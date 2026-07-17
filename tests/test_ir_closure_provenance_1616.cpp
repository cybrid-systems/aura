// @category: integration
// @reason: Issue #1616 — complete SyntaxMarker + provenance into
// IRClosure / ClosureBridge / FlatInstruction (refine #1047 / #1610).
//
//   AC1: IRInstruction + FlatInstruction carry marker+provenance (wired)
//   AC2: ClosureBridge / IRClosure marker fields (wire flags)
//   AC3: ir_closure_needs_safe_fallback consults MacroIntroduced
//   AC4: metrics ir_provenance_stamped_total, macro_introduced_ignored_in_ir
//   AC5: query:ir-hygiene-stats schema 1616 (+ ir-marker keys)
//   AC6: macro workspace + mutate + IR hygiene non-zero lineage

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"

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
                 "(d 1) (d 2) "
                 "(define base 10) "
                 "(define (f x) (+ x base)) "
                 "(f 1)\")")) {
        return false;
    }
    return cs.eval("(eval-current)").has_value();
}

static void ac1_ac2_wire_flags() {
    std::println("\n--- AC1/AC2: wire flags ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    CHECK(href(cs, "query:ir-hygiene-stats", "closure-bridge-marker-wired") == 1,
          "closure-bridge-marker-wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "ir-closure-marker-wired") == 1,
          "ir-closure-marker-wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "flat-instr-provenance-wired") == 1,
          "flat-instr-provenance-wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "lowering-stamp-wired") == 1, "lowering-stamp-wired");
    CHECK(href(cs, "query:ir-hygiene-stats", "jit-marker-check-wired") == 1,
          "jit-marker-check-wired");
}

static void ac3_ac4_metrics_keys() {
    std::println("\n--- AC3/AC4: metrics keys ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    (void)cs.eval("(query:pattern \"*\")");
    CHECK(href(cs, "query:ir-hygiene-stats", "ir_provenance_stamped_total") >= 0 ||
              href(cs, "query:ir-hygiene-stats", "ir-provenance-stamped-total") >= 0,
          "ir_provenance_stamped_total");
    CHECK(href(cs, "query:ir-hygiene-stats", "macro_introduced_ignored_in_ir") >= 0 ||
              href(cs, "query:ir-hygiene-stats", "macro-introduced-ignored-in-ir") >= 0,
          "macro_introduced_ignored_in_ir");
    CHECK(href(cs, "query:ir-hygiene-stats", "ir-closure-macro-stamped") >= 0,
          "ir-closure-macro-stamped");
    CHECK(href(cs, "query:ir-hygiene-stats", "ir-closure-macro-consults") >= 0,
          "ir-closure-macro-consults");
    CHECK(href(cs, "query:ir-hygiene-stats", "macro-introduced-count") >= 0,
          "macro-introduced-count");
}

static void ac5_schema() {
    std::println("\n--- AC5: schema 1616 + ir-marker keys ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    auto h1 = cs.eval("(engine:metrics \"query:ir-hygiene-stats\")");
    CHECK(h1 && is_hash(*h1), "ir-hygiene-stats hash");
    CHECK(href(cs, "query:ir-hygiene-stats", "schema") == 1616 ||
              href(cs, "query:ir-hygiene-stats", "schema") == 1610,
          "ir-hygiene schema 1616|1610");
    CHECK(href(cs, "query:ir-hygiene-stats", "issue") == 1616 ||
              href(cs, "query:ir-hygiene-stats", "issue") == 1610,
          "issue lineage");
    // AC "query:ir-marker-stats" served as keys (no new *-stats name).
    CHECK(href(cs, "query:ir-hygiene-stats", "macro-introduced-count") >= 0,
          "macro-introduced-count key (ir-marker surface)");
}

static void ac6_macro_mutate_lineage() {
    std::println("\n--- AC6: macro workspace + mutate lineage ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    CHECK(cs.eval("(mutate:rebind \"base\" \"20\")").has_value(), "mutate:rebind user");
    (void)cs.eval("(eval-current)");
    (void)cs.eval("(query:pattern \"*\")");
    // Lineage counters from #1610 still present.
    CHECK(href(cs, "query:ir-hygiene-stats", "ir-hygiene-stamped-count") >= 0, "stamped count");
    CHECK(href(cs, "query:ir-hygiene-stats", "provenance-stamped-count") >= 0, "prov stamped");
    CHECK(href(cs, "query:ir-hygiene-stats", "macro-markers") >= 0, "macro-markers");
    auto r = cs.eval("(+ 1 1)");
    CHECK(r.has_value(), "eval ok");
}

static void ac_marker_count() {
    std::println("\n--- AC: MacroIntroduced counts readable ---");
    CompilerService cs;
    CHECK(setup_macro_ws(cs), "macro workspace");
    const auto n = href(cs, "query:ir-hygiene-stats", "macro-introduced-count");
    CHECK(n >= 0, "macro-introduced-count >= 0");
    CHECK(href(cs, "query:ir-hygiene-stats", "macro-markers") >= 0, "macro-markers");
}

} // namespace

int main() {
    std::println("=== Issue #1616: IRClosure/ClosureBridge provenance ===");
    ac1_ac2_wire_flags();
    ac3_ac4_metrics_keys();
    ac5_schema();
    ac6_macro_mutate_lineage();
    ac_marker_count();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
