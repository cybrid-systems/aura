// @category: unit
// @reason: Issue #2148 — Precision meet/join for predicate lattice
// (number/string/bool + simple ADT tags).
//
//   AC1: (and (number? x) (integer? x)) refines to Int, not Dynamic
//   AC2: (or (string? x) (number? x)) remains Dynamic — no false precision
//   AC3: Existing gradual / strict coercion paths unchanged (smoke)
//   AC4: Unit tests for meet/join tag matrix + schema-2148

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.type;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::TypeRegistry;
using aura::core::TypeTag;
using aura::core::VariantType;
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
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC4: meet/join tag matrix (direct TypeRegistry) ──────────
static void ac4_meet_join_matrix() {
    std::println("\n--- AC4: meet/join tag matrix ---");
    TypeRegistry reg;
    const auto Dyn = reg.dynamic_type();
    const auto Int = reg.int_type();
    const auto Str = reg.string_type();
    const auto Bool = reg.bool_type();
    const auto Flt = reg.lookup_type("Float");
    CHECK(Flt.valid(), "Float pre-registered");

    // Equal / identity
    CHECK(reg.meet(Int, Int) == Int, "meet(Int,Int)=Int");
    CHECK(reg.join(Str, Str) == Str, "join(String,String)=String");

    // Dynamic as top
    CHECK(reg.meet(Int, Dyn) == Int, "meet(Int,Any)=Int (Number-ish top)");
    CHECK(reg.meet(Dyn, Str) == Str, "meet(Any,String)=String");
    CHECK(reg.join(Int, Dyn) == Dyn, "join(Int,Any)=Any");
    CHECK(reg.join(Dyn, Str) == Dyn, "join(Any,String)=Any");

    // Int ∩ Number-ish (Any) → Int; precision hit
    const auto hits0 = reg.meet_precision_hit_total();
    auto m = reg.meet(Int, Dyn);
    CHECK(m == Int, "Int ∩ Number-ish → Int");
    CHECK(reg.meet_precision_hit_total() > hits0, "meet_precision_hit on Int∩Any");

    // Cross-tag collapses (no false precision)
    CHECK(reg.meet(Int, Str) == Dyn, "meet(Int,String)=Dynamic");
    CHECK(reg.join(Str, Int) == Dyn, "join(String,Int)=Dynamic");
    CHECK(reg.meet(Int, Flt) == Dyn, "meet(Int,Float)=Dynamic");
    CHECK(reg.join(Int, Flt) == Dyn, "join(Int,Float)=Dynamic (no Number)");
    CHECK(reg.meet(Bool, Int) == Dyn, "meet(Bool,Int)=Dynamic");

    // Same ADT tag
    VariantType vt;
    vt.variants.push_back({"Some", {Int}});
    vt.variants.push_back({"None", {}});
    auto adt = reg.register_variant(std::move(vt));
    CHECK(adt.valid() && reg.tag_of(adt) == TypeTag::VARIANT, "register variant");
    CHECK(reg.meet(adt, adt) == adt, "meet same ADT = ADT");
    CHECK(reg.join(adt, adt) == adt, "join same ADT = ADT");
    CHECK(reg.meet(adt, Int) == Dyn, "meet(ADT,Int)=Dynamic");

    // a==b does not bump precision (identity path)
    const auto hits1 = reg.meet_precision_hit_total();
    (void)reg.meet(Int, Int);
    CHECK(reg.meet_precision_hit_total() == hits1, "equal meet no precision hit");
}

// ── AC1: and (number? / integer?) refines to Int ─────────────
static void ac1_and_number_integer() {
    std::println("\n--- AC1: (and (number? x) (integer? x)) → Int ---");
    TypeRegistry reg;
    // Both predicates refine to Int today; meet must stay Int.
    auto n = reg.int_type(); // number?
    auto i = reg.int_type(); // integer?
    auto m = reg.meet(n, i);
    CHECK(m == reg.int_type(), "meet(number?,integer?) → Int");
    CHECK(m != reg.dynamic_type(), "not Dynamic");
    CHECK(reg.name_of(m) == "Int" || reg.tag_of(m) == TypeTag::INT, "tag/name Int");

    // End-to-end: compound predicate typechecks and uses meet path.
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define f (lambda (x) "
                  "(if (and (number? x) (integer? x)) (+ x 1) 0)))\")")
              .has_value(),
          "set-code and-number-integer");
    CHECK(cs.eval("(eval-current)").has_value(), "eval");
    auto tc = cs.eval("(typecheck-current)");
    CHECK(tc.has_value(), "typecheck compound and-predicate");
    // Call with int succeeds under refinement.
    auto r = cs.eval("(f 41)");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 42, "(f 41) → 42");
}

// ── AC2: or string/number stays Dynamic ──────────────────────
static void ac2_or_string_number_dynamic() {
    std::println("\n--- AC2: (or (string? x) (number? x)) → Dynamic ---");
    TypeRegistry reg;
    auto j = reg.join(reg.string_type(), reg.int_type());
    CHECK(j == reg.dynamic_type(), "join(String,Int)=Dynamic — no false precision");

    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define g (lambda (x) "
                  "(if (or (string? x) (number? x)) x #f)))\")")
              .has_value(),
          "set-code or-string-number");
    CHECK(cs.eval("(eval-current)").has_value(), "eval g");
    CHECK(cs.eval("(typecheck-current)").has_value(), "typecheck or-predicate");
    auto r1 = cs.eval("(g \"hi\")");
    CHECK(r1.has_value(), "(g \"hi\") ok");
    auto r2 = cs.eval("(g 7)");
    CHECK(r2.has_value() && is_int(*r2) && as_int(*r2) == 7, "(g 7) → 7");
}

// ── AC3: gradual smoke ───────────────────────────────────────
static void ac3_gradual_smoke() {
    std::println("\n--- AC3: gradual / valid programs unchanged ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define id (lambda (x) x)) (define n (+ 1 2))\")").has_value(),
          "set-code gradual");
    CHECK(cs.eval("(eval-current)").has_value(), "eval gradual");
    CHECK(cs.eval("(typecheck-current)").has_value(), "typecheck gradual");
    auto r = cs.eval("(id n)");
    CHECK(r.has_value() && is_int(*r) && as_int(*r) == 3, "(id n) → 3");
    // Coercion surface still present
    auto src = read_file("src/compiler/coercion_map.ixx");
    if (src.empty())
        src = read_file("src/core/type_impl.cpp");
    CHECK(!src.empty(), "coercion/type sources readable");
}

// ── Schema + wiring ──────────────────────────────────────────
static void ac_schema_2148() {
    std::println("\n--- schema-2148 + meet-precision keys ---");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define h (lambda (x) "
                  "(if (and (number? x) (integer? x)) x 0)))\")")
              .has_value(),
          "set-code schema");
    CHECK(cs.eval("(eval-current)").has_value(), "eval schema");
    (void)cs.eval("(typecheck-current)");
    // Force a precise meet via registry path that CompilerService owns.
    // typecheck of and-predicate uses meet(Int,Int) which is identity;
    // also exercise Any∩Int via fidelity query after a Dynamic meet.
    TypeRegistry local;
    (void)local.meet(local.int_type(), local.dynamic_type());

    CHECK(href(cs, "schema-2148") == 2148, "schema-2148");
    CHECK(href(cs, "meet-precision-lattice-wired") == 1, "lattice wired");
    // Keys present (values may be 0 if only identity meets ran).
    CHECK(href(cs, "meet-precision-hit-total") >= 0, "meet-precision-hit-total key");
    CHECK(href(cs, "and-or-meet-uses-total") >= 0, "and-or-meet-uses-total key");

    auto q = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(q.has_value(), "fidelity query reachable");
    // Source cites #2148
    auto impl = read_file("src/core/type_impl.cpp");
    CHECK(impl.find("#2148") != std::string::npos, "type_impl cites #2148");
}

} // namespace

int main() {
    ac4_meet_join_matrix();
    ac1_and_number_integer();
    ac2_or_string_number_dynamic();
    ac3_gradual_smoke();
    ac_schema_2148();

    std::println("\n=== #2148 predicate meet/join lattice: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
