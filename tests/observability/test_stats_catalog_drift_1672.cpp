// @category: unit
// @reason: Issue #1672 — kObservabilityStatsPrimitives catalog must stay
// aligned with register_stats_impl (+ residual public aliases). Diagnostic
// via (stats:get "stats:drift-check") / engine:metrics (schema 1672).
//
//   AC1: stats:drift-check resolves via stats:get
//   AC2: hash schema == 1672
//   AC3: catalog-size and impl-size > 0
//   AC4: missing-impl-count == 0 (catalog entries all resolve)
//   AC5: ok field + missing-catalog-count present (reverse drift may be >0
//        until full catalog catch-up; is_legacy_stats_name filter applies)
//   AC6: engine:metrics by-name also resolves drift-check

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::int64_t href(CompilerService& cs, std::string_view expr, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref {} \"{}\")", expr, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void ac1_resolves() {
    std::println("\n--- AC1: stats:get stats:drift-check ---");
    CompilerService cs;
    auto r = cs.eval("(stats:get \"stats:drift-check\")");
    CHECK(r.has_value(), "stats:get returns");
    if (r)
        CHECK(is_hash(*r), "drift-check is hash");
}

static void ac2_schema() {
    std::println("\n--- AC2: schema 1672 ---");
    CompilerService cs;
    CHECK(href(cs, "(stats:get \"stats:drift-check\")", "schema") == 1672, "schema == 1672");
}

static void ac3_sizes() {
    std::println("\n--- AC3: catalog/impl sizes ---");
    CompilerService cs;
    const auto cat = href(cs, "(stats:get \"stats:drift-check\")", "catalog-size");
    const auto impl = href(cs, "(stats:get \"stats:drift-check\")", "impl-size");
    CHECK(cat > 0, "catalog-size > 0");
    CHECK(impl > 0, "impl-size > 0");
    std::println("  catalog-size={} impl-size={}", cat, impl);
}

static void ac4_missing_impl_zero() {
    std::println("\n--- AC4: missing-impl-count == 0 ---");
    CompilerService cs;
    const auto mi = href(cs, "(stats:get \"stats:drift-check\")", "missing-impl-count");
    const auto mc = href(cs, "(stats:get \"stats:drift-check\")", "missing-catalog-count");
    const auto ok = href(cs, "(stats:get \"stats:drift-check\")", "ok");
    std::println("  ok={} missing-impl={} missing-catalog={}", ok, mi, mc);
    // Catalog → resolve path is the silent-failure class (#1672 primary).
    CHECK(mi == 0, "missing-impl-count == 0 (every catalog name resolves)");
    // Reverse drift (impl not catalogued) may remain >0 until a bulk catalog
    // catch-up; still exposed in the hash for agents.
    CHECK(mc >= 0, "missing-catalog-count readable");
}

static void ac5_fields() {
    std::println("\n--- AC5: count fields ---");
    CompilerService cs;
    CHECK(href(cs, "(stats:get \"stats:drift-check\")", "missing-impl-count") >= 0,
          "missing-impl-count field");
    CHECK(href(cs, "(stats:get \"stats:drift-check\")", "missing-catalog-count") >= 0,
          "missing-catalog-count field");
    CHECK(href(cs, "(stats:get \"stats:drift-check\")", "ok") == 0 ||
              href(cs, "(stats:get \"stats:drift-check\")", "ok") == 1,
          "ok is 0 or 1");
}

static void ac6_engine_metrics() {
    std::println("\n--- AC6: engine:metrics by-name ---");
    CompilerService cs;
    auto r = cs.eval("(engine:metrics \"stats:drift-check\")");
    CHECK(r && is_hash(*r), "engine:metrics stats:drift-check is hash");
    CHECK(href(cs, "(engine:metrics \"stats:drift-check\")", "schema") == 1672,
          "engine:metrics schema");
}

} // namespace

int main() {
    std::println("=== Issue #1672: stats catalog drift check ===");
    ac1_resolves();
    ac2_schema();
    ac3_sizes();
    ac4_missing_impl_zero();
    ac5_fields();
    ac6_engine_metrics();
    std::println("\n=== Results: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
