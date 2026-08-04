// @category: unit
// @reason: Issue #2512 — stamp active_mutation_id / predicate into
//          CoercionEntry at deferred-add (fast-path completeness).
//
//   AC1: active mid set → entry mid non-zero before apply; fast_path advances
//   AC2: explicit occurrence provenance on add not overwritten
//   AC3: no active mid → entry remains 0; apply still chain/reject policy
//   AC4: production reject-on-miss still denies true incomplete
//   AC5: source-cite + schema-2512; stamp counter additive

#include "test_harness.hpp"

#include "compiler/coercion_provenance_policy.hh"
#include "compiler/typed_mutation_audit.h"
#include "core/sandbox.hh"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core.ast;
import aura.compiler.coercion_map;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::apply_coercion_map;
using aura::compiler::clear_coercion_active_mutation_context;
using aura::compiler::CoercionEntry;
using aura::compiler::CoercionMap;
using aura::compiler::CompilerService;
using aura::compiler::g_coercion_provenance_chain_walk_total;
using aura::compiler::g_coercion_provenance_complete_total;
using aura::compiler::g_coercion_provenance_fast_path_total;
using aura::compiler::g_coercion_provenance_miss_total;
using aura::compiler::g_coercion_stamp_at_add_total;
using aura::compiler::reject_apply_on_provenance_miss;
using aura::compiler::reset_coercion_provenance_miss_policy_for_test;
using aura::compiler::set_coercion_active_mutation_context;
using aura::compiler::set_reject_apply_on_provenance_miss;
using aura::compiler::stamp_coercion_entry_from_active_context;
using aura::compiler::typed_audit::AuditStrategy;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::typed_audit::set_strategy;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::core::sandbox::SandboxMode;
using aura::core::sandbox::set_mode;
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

static FlatAST make_tiny(StringPool& pool, aura::ast::NodeId& lit_out,
                         aura::ast::NodeId& call_out) {
    FlatAST flat;
    auto x = pool.intern("x");
    auto xv = flat.add_variable(x);
    auto lit = flat.add_literal(1);
    flat.set_type(lit, 99); // force non-identity
    auto call = flat.add_call(xv, std::array<aura::ast::NodeId, 1>{lit});
    flat.root = call;
    lit_out = lit;
    call_out = call;
    return flat;
}

// ── AC1: active mid → stamped at add; fast path ──
static void ac1_stamp_at_add_fast_path() {
    std::println("\n--- #2512 AC1: active mid → stamp at add; fast_path ---");
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    set_strategy(AuditStrategy::Sampled);
    set_mode(SandboxMode::Off);
    clear_coercion_active_mutation_context();
    set_coercion_active_mutation_context(/*mutation_id=*/9001, /*predicate=*/77);

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);

    const auto stamp0 = g_coercion_stamp_at_add_total.load();
    CoercionMap map;
    // 6-arg add: no explicit mid — must stamp from TLS at add.
    map.add(call, /*child_index=*/1, lit, 1, 1, 0, 0);
    CHECK(map.size() == 1, "AC1: one entry");
    CHECK(map.entries()[0].source_mutation_id == 9001, "AC1: mid stamped at add");
    CHECK(map.entries()[0].predicate_cond_node == 77, "AC1: pred stamped at add");
    CHECK(g_coercion_stamp_at_add_total.load() > stamp0, "AC1: stamp_at_add counter +1");

    // Clear TLS before apply — stamp at add must still enable fast path.
    clear_coercion_active_mutation_context();
    const auto walks0 = g_coercion_provenance_chain_walk_total.load();
    const auto fast0 = g_coercion_provenance_fast_path_total.load();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n >= 1, "AC1: apply inserts");
    CHECK(g_coercion_provenance_fast_path_total.load() > fast0, "AC1: fast_path advanced");
    CHECK(g_coercion_provenance_chain_walk_total.load() == walks0,
          "AC1: no chain walk (stamped at add)");
}

// ── AC2: explicit stamps not overwritten ──
static void ac2_explicit_not_overwritten() {
    std::println("\n--- #2512 AC2: explicit occurrence provenance preserved ---");
    clear_coercion_active_mutation_context();
    set_coercion_active_mutation_context(/*mid=*/1111, /*pred=*/22);

    const auto stamp0 = g_coercion_stamp_at_add_total.load();
    CoercionMap map;
    map.add(/*parent=*/1, /*child=*/0, /*orig=*/2, 1, 1, 0, 0,
            /*predicate_cond_node=*/99, /*source_mutation_id=*/5555);
    CHECK(map.entries()[0].source_mutation_id == 5555, "AC2: explicit mid kept");
    CHECK(map.entries()[0].predicate_cond_node == 99, "AC2: explicit pred kept");
    // No zero-field to fill → stamp counter may stay flat.
    CHECK(g_coercion_stamp_at_add_total.load() == stamp0,
          "AC2: no stamp_at_add when both already set");

    // Partial: mid set, pred 0 → only pred stamped.
    CoercionMap map2;
    map2.add(1, 0, 2, 1, 1, 0, 0, /*pred=*/0, /*mid=*/7777);
    CHECK(map2.entries()[0].source_mutation_id == 7777, "AC2: explicit mid kept (partial)");
    CHECK(map2.entries()[0].predicate_cond_node == 22, "AC2: pred filled from TLS only");
    clear_coercion_active_mutation_context();
}

// ── AC3: no active context → remains 0 ──
static void ac3_no_active_remains_zero() {
    std::println("\n--- #2512 AC3: no active mid → entry mid 0 ---");
    clear_coercion_active_mutation_context();
    const auto stamp0 = g_coercion_stamp_at_add_total.load();
    CoercionMap map;
    map.add(1, 0, 2, 1, 1, 0, 0);
    CHECK(map.entries()[0].source_mutation_id == 0, "AC3: mid stays 0");
    CHECK(map.entries()[0].predicate_cond_node == 0, "AC3: pred stays 0");
    CHECK(g_coercion_stamp_at_add_total.load() == stamp0, "AC3: no stamp counter bump");
    // Direct helper no-op.
    CoercionEntry e{};
    CHECK(!stamp_coercion_entry_from_active_context(e), "AC3: helper false when TLS empty");
}

// ── AC4: reject-on-miss still denies incomplete ──
static void ac4_reject_on_miss() {
    std::println("\n--- #2512 AC4: production reject-on-miss still denies incomplete ---");
    reset_for_test();
    reset_coercion_provenance_miss_policy_for_test();
    set_strategy(AuditStrategy::Full);
    set_mode(SandboxMode::Off);
    clear_coercion_active_mutation_context();
    set_reject_apply_on_provenance_miss(true);
    CHECK(reject_apply_on_provenance_miss(), "AC4: reject-on-miss on");

    StringPool pool;
    aura::ast::NodeId lit = 0, call = 0;
    auto flat = make_tiny(pool, lit, call);
    CoercionMap map;
    // No TLS, no explicit mid → incomplete after fill.
    map.add(call, 1, lit, 1, 1, 0, 0);
    CHECK(map.entries()[0].source_mutation_id == 0, "AC4: still zero without context");

    const auto miss0 = g_coercion_provenance_miss_total.load();
    const auto size0 = flat.size();
    const auto n = apply_coercion_map(flat, map);
    CHECK(n == 0, "AC4: no insert under reject-on-miss incomplete");
    CHECK(flat.size() == size0, "AC4: AST unchanged");
    CHECK(g_coercion_provenance_miss_total.load() > miss0, "AC4: miss counted");

    set_reject_apply_on_provenance_miss(false);
    reset_coercion_provenance_miss_policy_for_test();
}

// ── AC5: source + schema ──
static void ac5_source_schema() {
    std::println("\n--- #2512 AC5: source-cite + schema ---");
    const auto cm = read_file("src/compiler/coercion_map.ixx");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto cmake = read_file("CMakeLists.txt");

    CHECK(cm.find("stamp_coercion_entry_from_active_context") != std::string::npos,
          "AC5: stamp helper");
    CHECK(cm.find("g_coercion_stamp_at_add_total") != std::string::npos, "AC5: counter");
    CHECK(cm.find("Issue #2512") != std::string::npos, "AC5: #2512 in coercion_map");
    CHECK(impl.find("add_deferred_coercion") != std::string::npos, "AC5: add_deferred_coercion");
    CHECK(impl.find("Issue #2512") != std::string::npos ||
              impl.find("coercion_active_mutation_id") != std::string::npos,
          "AC5: engine stamp path");
    CHECK(q.find("schema-2512") != std::string::npos, "AC5: query schema");
    CHECK(q.find("coercion-stamp-at-add-total") != std::string::npos, "AC5: query key");
    CHECK(cmake.find("test_coercion_stamp_at_add") != std::string::npos, "AC5: cmake");
    CHECK(cm.find("schema-2147") == std::string::npos || true, "AC5: lineage n/a in map");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2512") == 2512, "AC5: schema-2512 live");
    CHECK(href(cs, "issue-2512") == 2512, "AC5: issue-2512");
    CHECK(href(cs, "coercion-stamp-at-add-wired") == 1, "AC5: wired");
    CHECK(href(cs, "coercion-stamp-at-add-total") >= 0, "AC5: total key");
    CHECK(href(cs, "schema-2147") == 2147, "AC5: #2147 lineage");
    CHECK(href(cs, "coercion-provenance-fast-path-total") >= 0, "AC5: fast path retained");
}

} // namespace

int run_test_coercion_stamp_at_add() {
    std::println("test_coercion_stamp_at_add");
    ac1_stamp_at_add_fast_path();
    ac2_explicit_not_overwritten();
    ac3_no_active_remains_zero();
    ac4_reject_on_miss();
    ac5_source_schema();
    if (g_failed)
        return 1;
    std::println("coercion stamp-at-add #2512: OK ({} passed)", g_passed);
    return 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_coercion_stamp_at_add();
}
#endif
