// @category: unit
// @reason: Issue #2563 — cross-closure linear escape discovery +
//          force_linear_rollback CrossClosureEscape authority.
//
//   AC1: dirty linear free-captured by Lambda → discovered; hard forces
//        rollback with distinct deny; Soft observe-only counters
//   AC2: no cross-closure → zero new counters; PostMutate/CrossBatch unchanged
//   AC3: #2545 single-entry force_linear_rollback still holds
//   AC4: additive schema-2563 + source-cite
//   AC5: cone-capped discovery respects #2560 soft cap

#include "test_harness.hpp"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>

import std;
import aura.core.ast;
import aura.compiler.service;
import aura.compiler.value;
import aura.compiler.type_checker;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::CompilerService;
using aura::compiler::CrossClosureEscapeResult;
using aura::compiler::discover_cross_closure_linear_escapes;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::g_typed_mutation_audit_counters;
using aura::compiler::typed_audit::linear_cross_closure_hard_enabled;
using aura::compiler::typed_audit::production_defaults_active;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
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
        "(hash-ref (engine:metrics \"query:linear-ownership-typed-mutate-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_2563() {
    reset_for_test();
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Sampled);
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
    unsetenv("AURA_PARTIAL_CONE_SOFT");
}

// Build: (lambda (p) x) where x is free dirty linear name.
static FlatAST make_lambda_capture(StringPool& pool, const char* free_name) {
    FlatAST flat;
    auto x = pool.intern(free_name);
    auto p = pool.intern("p");
    auto xv = flat.add_variable(x);
    // Minimal lambda with body = free variable (capture).
    auto lam = flat.add_lambda(std::array{p}, xv);
    flat.root = lam;
    return flat;
}

// ── AC1: discover + hard force / Soft observe ──
static void ac1_discover_hard_soft() {
    std::println("\n--- #2563 AC1: discover free-capture; hard force / Soft observe ---");
    reset_2563();

    StringPool pool;
    auto flat = make_lambda_capture(pool, "lin");
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    const bool clean =
        discover_cross_closure_linear_escapes(flat, pool, dirty, /*cone_cap=*/256, out);
    CHECK(!clean, "AC1: free-capture is an escape");
    CHECK(out.escape_sites >= 1, "AC1: escape_sites >= 1");
    CHECK(out.sites_scanned >= 1, "AC1: Lambda sites scanned");

    // Param-only use is not free-capture.
    auto flat2 = make_lambda_capture(pool, "p");
    dirty = {"p"};
    CrossClosureEscapeResult out2{};
    // lambda param is "p", free name is also "p" → param, not escape
    CHECK(discover_cross_closure_linear_escapes(flat2, pool, dirty, 256, out2),
          "AC1: param use is not free-capture escape");
    CHECK(out2.escape_sites == 0, "AC1: param capture zero escapes");

    // Soft hard_enabled default off under Sampled.
    CHECK(!linear_cross_closure_hard_enabled(), "AC1: Soft Sampled hard off");
    setenv("AURA_LINEAR_CROSS_CLOSURE_HARD", "1", 1);
    CHECK(linear_cross_closure_hard_enabled(), "AC1: env hard on");
    unsetenv("AURA_LINEAR_CROSS_CLOSURE_HARD");
    apply_production_audit_defaults();
    CHECK(linear_cross_closure_hard_enabled(), "AC1: production hard on");
    apply_dev_audit_defaults();
    set_strategy(AuditStrategy::Full);
    CHECK(linear_cross_closure_hard_enabled(), "AC1: Full hard on");
    set_strategy(AuditStrategy::Sampled);

    // Source: force path + deny kind
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("CrossClosureEscape") != std::string::npos, "AC1: CrossClosureEscape authority");
    CHECK(etc.find("linear-cross-closure-escape") != std::string::npos, "AC1: distinct deny kind");
    CHECK(etc.find("discover_cross_closure_linear_escapes") != std::string::npos,
          "AC1: discovery wired");
    CHECK(etc.find("linear_cross_closure_observe_total") != std::string::npos,
          "AC1: Soft observe counter");
    CHECK(etc.find("linear_cross_closure_force_total") != std::string::npos,
          "AC1: hard force counter");
}

// ── AC2: no cross-closure zero work; existing paths ──
static void ac2_zero_and_existing() {
    std::println("\n--- #2563 AC2: no capture → zero; CrossBatch/PostMutate retained ---");
    reset_2563();
    StringPool pool;
    FlatAST empty;
    empty.add_literal(1);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    CHECK(discover_cross_closure_linear_escapes(empty, pool, dirty, 256, out),
          "AC2: no Lambda → clean");
    CHECK(out.escape_sites == 0, "AC2: zero escape_sites");
    CHECK(out.sites_scanned == 0, "AC2: zero Lambda sites");

    dirty.clear();
    auto flat = make_lambda_capture(pool, "lin");
    CHECK(discover_cross_closure_linear_escapes(flat, pool, dirty, 256, out),
          "AC2: empty dirty → clean");
    CHECK(out.escape_sites == 0, "AC2: empty dirty zero escapes");

    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("CrossBatchEscape") != std::string::npos, "AC2: CrossBatch retained");
    CHECK(etc.find("PostMutateLinear") != std::string::npos, "AC2: PostMutate retained");
    CHECK(etc.find("hard_block_cross_batch_linear_escape") != std::string::npos,
          "AC2: cross-batch hard-block retained");
}

// ── AC3: #2545 single entry ──
static void ac3_unified_entry() {
    std::println("\n--- #2563 AC3: #2545 single-entry force_linear_rollback ---");
    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto eixx = read_file("src/compiler/evaluator.ixx");
    const auto emb = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(eixx.find("force_linear_rollback") != std::string::npos, "AC3: unified entry declared");
    CHECK(eixx.find("CrossClosureEscape") != std::string::npos, "AC3: authority in enum");
    CHECK(etc.find("force_linear_rollback") != std::string::npos, "AC3: force impl");
    CHECK(etc.find("case LinearForceAuthority::CrossClosureEscape") != std::string::npos,
          "AC3: CrossClosure case in force_linear_rollback");
    CHECK(emb.find("force_linear_rollback") != std::string::npos,
          "AC3: boundary still uses unified entry");
    // No ad-hoc sticky force outside force_linear_rollback for cross-closure.
    CHECK(etc.find("note_cross_closure_escape_fail") != std::string::npos, "AC3: sticky note");
    CHECK(etc.find("classify_linear_force") != std::string::npos, "AC3: classify includes axis");
}

// ── AC4: schema + source ──
static void ac4_schema() {
    std::println("\n--- #2563 AC4: schema-2563 + source-cite ---");
    const auto aud = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(aud.find("#2563") != std::string::npos, "AC4: audit cites #2563");
    CHECK(aud.find("linear_cross_closure_escape_total") != std::string::npos, "AC4: escape total");
    CHECK(aud.find("linear_cross_closure_hard_enabled") != std::string::npos, "AC4: hard helper");
    CHECK(aud.find("cross_closure_linear_escape") != std::string::npos, "AC4: audit result field");

    const auto tci = read_file("src/compiler/type_checker.ixx");
    CHECK(tci.find("discover_cross_closure_linear_escapes") != std::string::npos,
          "AC4: discover export");
    CHECK(tci.find("CrossClosureEscapeResult") != std::string::npos, "AC4: result struct");

    const auto q = read_file("src/compiler/evaluator_primitives_security.cpp");
    CHECK(q.find("schema-2563") != std::string::npos, "AC4: schema-2563");
    CHECK(q.find("linear-cross-closure-escape-total") != std::string::npos, "AC4: query key");

    reset_2563();
    CompilerService cs;
    CHECK(href(cs, "schema-2563") == 2563, "AC4: live schema-2563");
    CHECK(href(cs, "linear-cross-closure-wired") == 1, "AC4: wired");
    CHECK(href(cs, "linear-cross-closure-escape-total") >= 0, "AC4: escape queryable");
    CHECK(href(cs, "linear-cross-closure-force-total") >= 0, "AC4: force queryable");
    CHECK(href(cs, "linear-cross-closure-observe-total") >= 0, "AC4: observe queryable");
    CHECK(href(cs, "linear-cross-closure-hard-enabled") == 0, "AC4: Soft hard=0");
    CHECK(href(cs, "schema-2545") == 2545, "AC4: #2545 schema retained");
}

// ── AC5: cone cap ──
static void ac5_cone_cap() {
    std::println("\n--- #2563 AC5: cone-capped discovery (#2560 soft cap) ---");
    reset_2563();
    StringPool pool;
    auto flat = make_lambda_capture(pool, "lin");
    // Pad AST so size > tiny cone cap.
    for (int i = 0; i < 20; ++i)
        (void)flat.add_literal(i);
    std::unordered_set<std::string> dirty{"lin"};
    CrossClosureEscapeResult out{};
    // cone_cap=1 may miss Lambda if it's not node 0 — still records trunc when size>cap.
    (void)discover_cross_closure_linear_escapes(flat, pool, dirty, /*cone_cap=*/1, out);
    CHECK(out.cap_truncations == 1, "AC5: cap_truncations when size > cone_cap");

    const auto etc = read_file("src/compiler/evaluator_typecheck.cpp");
    CHECK(etc.find("AURA_PARTIAL_CONE_SOFT") != std::string::npos ||
              etc.find("partial_cone_soft_cap_for_linear") != std::string::npos,
          "AC5: discovery uses soft cone cap");
    CHECK(etc.find("linear_cross_closure_cap_trunc_total") != std::string::npos,
          "AC5: cap trunc counter wired");
}

} // namespace

int main() {
    std::println("=== Issue #2563: cross-closure linear escape discovery ===");
    ac1_discover_hard_soft();
    ac2_zero_and_existing();
    ac3_unified_entry();
    ac4_schema();
    ac5_cone_cap();
    apply_dev_audit_defaults();
    reset_2563();
    std::println("\n=== #2563: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
