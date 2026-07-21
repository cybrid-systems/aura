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

int main() {
    std::println("=== test_shape_soa_unit_batch (wave 36) ===");
    if (int rc = aura_shape_run_wave36_286::run_286_env_version_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_shape_run_wave36_437::run_437_verify_dirty_reason_smoke(); rc != 0)
        return rc;
    std::println("\ntest_shape_soa_unit_batch: OK");
    return 0;
}
