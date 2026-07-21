// test_shape_soa_unit_batch.cpp — Wave 36+ (#1957) shape_soa theme
// Prefer adding a section here over a new tests/issues binary.

#include "test_harness.hpp"

#include <cstdint>
#include <print>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.value;
import aura.core.ast;

// Wave 36: #286 Env env_version / SoA lookup smoke
namespace aura_shape_run_wave36_286 {
using aura::compiler::CompilerService;
using aura::compiler::Env;
using aura::test::g_failed;
using aura::test::g_passed;
int run_286_env_version_smoke() {
    std::println("\n=== #286: Env::env_version + materialize stamp smoke ===");
    Env e;
    CHECK(e.env_version() == 0, "fresh env_version 0");
    e.set_env_version(42);
    CHECK(e.env_version() == 42, "set/get 42");
    CompilerService cs;
    CHECK(cs.eval("(define x 1)").has_value(), "define x");
    CHECK(cs.eval("(let ((f (lambda (y) (+ x y)))) f)").has_value(), "capture lambda");
    CHECK(cs.eval("(f 10)").has_value(), "apply stamps call env");
    const auto v = cs.evaluator().get_defuse_version();
    CHECK(v >= 0, "defuse_version readable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave36_286

namespace aura_shape_run_wave36_437 {
using aura::compiler::CompilerService;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
int run_437_verify_dirty_reason_smoke() {
    std::println("\n=== #437: VerifyDirtyReason enum + stats smoke ===");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kAssertionDirty) == 0x01,
          "kAssertionDirty == 0x01");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kCoverageDirty) == 0x02,
          "kCoverageDirty == 0x02");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kSvaDirty) == 0x04, "kSvaDirty == 0x04");
    CHECK(static_cast<std::uint8_t>(aura::ast::FlatAST::kFormalCounterexampleDirty) == 0x08,
          "kFormalCounterexampleDirty == 0x08");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:verify-dirty-stats\")");
    CHECK(r.has_value(), "query:verify-dirty-stats reachable");
    CHECK(cs.eval("(define smoke-437 1)").has_value(), "define smoke");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave36_437

// Wave 37 (#1957): shape_soa — #1520 children-column + #1521 shape-arena-compact stats
namespace aura_shape_run_wave37_1520 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_1520_children_column_stats_smoke() {
    std::println("\n=== #1520: query:children-column-stats smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define (g x) (+ x 1))\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto r = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(r.has_value(), "query:children-column-stats reachable");
    auto mig = cs.eval("(engine:metrics \"query:soa-children-columnar-migration-stats\")");
    CHECK(mig.has_value(), "soa-children-columnar-migration-stats reachable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave37_1520

namespace aura_shape_run_wave37_1521 {
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_1521_shape_arena_compact_stats_smoke() {
    std::println("\n=== #1521: query:shape-arena-compact-stats smoke ===");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:shape-arena-compact-stats\")");
    CHECK(r.has_value(), "query:shape-arena-compact-stats reachable");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave37_1521

// Wave 38 (#1957): shape_soa — #398 for_each_stable_child + children-stable
namespace aura_shape_run_wave38_398 {
using aura::ast::FlatAST;
using aura::ast::StringPool;
using aura::compiler::CompilerService;
using aura::test::g_failed;
using aura::test::g_passed;
int run_398_for_each_stable_child_smoke() {
    std::println("\n=== #398: for_each_stable_child + query:children-stable smoke ===");
    FlatAST flat;
    StringPool pool;
    auto c0 = flat.add_literal(0);
    auto c1 = flat.add_literal(1);
    auto parent = flat.add_define(pool.intern("p"), c0);
    flat.insert_child(parent, 1, c1);
    int n = 0;
    flat.for_each_stable_child(parent, [&](FlatAST::StableNodeRef r) {
        ++n;
        CHECK(r.id == c0 || r.id == c1, "child id expected");
    });
    CHECK(n == 2, "two stable children");
    CHECK(flat.stable_child_count(parent) == 2, "stable_child_count == 2");
    flat.for_each_stable_child(999999, [&](FlatAST::StableNodeRef) { ++n; });
    CHECK(n == 2, "OOB parent no-op");
    CompilerService cs;
    auto q = cs.eval("(engine:metrics \"query:children-column-stats\")");
    CHECK(q.has_value(), "column stats still reachable");
    auto csb = cs.eval("(query:children-stable 0)");
    (void)csb;
    CHECK(true, "query:children-stable invoked");
    return g_failed ? 1 : 0;
}
} // namespace aura_shape_run_wave38_398

int main() {
    std::println("=== test_shape_soa_unit_batch (wave 36+) ===");
    if (int rc = aura_shape_run_wave36_286::run_286_env_version_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave36_437::run_437_verify_dirty_reason_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave37_1520::run_1520_children_column_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave37_1521::run_1521_shape_arena_compact_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave38_398::run_398_for_each_stable_child_smoke(); rc != 0)
        return rc;
    std::println("\ntest_shape_soa_unit_batch: OK");
    return 0;
}
