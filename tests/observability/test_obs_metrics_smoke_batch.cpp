// test_obs_metrics_smoke_batch.cpp — consolidated observability schema smokes
// Wave 26+ (#1957): fold thin tests/issues/*_observability and metrics probes
// Prefer adding a section here over a new tests/issues binary.

#include "test_harness.hpp"

#include <cstdint>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_int;
using aura::compiler::types::is_error;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::compiler::types::is_pair;
using aura::test::g_failed;
using aura::test::g_passed;

std::int64_t hash_int(CompilerService& cs, std::string_view query, std::string_view key) {
    auto r = cs.eval(std::format("(hash-ref (engine:metrics \"{}\") '{}')", query, key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

// ═══════════════════════════════════════════════════════════════
// Wave 26 (#1957): observability — #478 #584 #583 #591
// ═══════════════════════════════════════════════════════════════

namespace aura_obs_run_wave26_478 {
int run_478_primitive_error_stats_smoke() {
    std::println("\n=== #478: primitive-error-stats smoke ===");
    CompilerService cs;
    auto& ev = cs.evaluator();
    CHECK(ev.get_primitive_error_count() == 0, "error count 0 fresh");
    CHECK(ev.get_primitive_error_values_size() == 0, "error_values empty fresh");
    auto r = cs.eval("(engine:metrics \"query:primitive-error-stats\")");
    CHECK(r && is_pair(*r), "primitive-error-stats pair");
    const auto before = ev.get_primitive_error_count();
    auto err = cs.eval("(modulo 1 0)");
    CHECK(err && is_error(*err), "modulo 1 0 → error");
    CHECK(ev.get_primitive_error_count() == before + 1, "error count +1");
    auto happy = cs.eval("(+ 1 2)");
    CHECK(happy && is_int(*happy) && as_int(*happy) == 3, "happy path +");
    return g_failed ? 1 : 0;
}
} // namespace aura_obs_run_wave26_478

namespace aura_obs_run_wave26_584 {
int run_584_primitives_hotpath_stats_smoke() {
    std::println("\n=== #584: primitives-hotpath-stats schema smoke ===");
    CompilerService cs;
    auto stats = cs.eval("(engine:metrics \"query:primitives-hotpath-stats\")");
    CHECK(stats && is_hash(*stats), "hotpath-stats hash");
    CHECK(hash_int(cs, "query:primitives-hotpath-stats", "primitive-call-total") >= 0,
          "primitive-call-total");
    CHECK(hash_int(cs, "query:primitives-hotpath-stats", "pair-alloc-total") >= 0,
          "pair-alloc-total");
    CHECK(hash_int(cs, "query:primitives-hotpath-stats", "hotpath-schema") == 584,
          "hotpath-schema == 584");
    return g_failed ? 1 : 0;
}
} // namespace aura_obs_run_wave26_584

namespace aura_obs_run_wave26_583 {
int run_583_registry_core_stats_smoke() {
    std::println("\n=== #583: primitives-registry-core-stats schema smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define acc 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto stats = cs.eval("(engine:metrics \"query:primitives-registry-core-stats\")");
    CHECK(stats && is_hash(*stats), "registry-core-stats hash");
    CHECK(hash_int(cs, "query:primitives-registry-core-stats", "registry-slots") > 0,
          "registry-slots > 0");
    CHECK(hash_int(cs, "query:primitives-registry-core-stats", "registry-core-schema") == 583,
          "registry-core-schema == 583");
    return g_failed ? 1 : 0;
}
} // namespace aura_obs_run_wave26_583

namespace aura_obs_run_wave26_591 {
int run_591_scheduler_mutation_coord_stats_smoke() {
    std::println("\n=== #591: scheduler-mutation-coord-stats schema smoke ===");
    CompilerService cs;
    CHECK(cs.eval("(set-code \"(define acc 0)\")").has_value(), "set-code");
    CHECK(cs.eval("(eval-current)").has_value(), "eval-current");
    auto stats = cs.eval("(engine:metrics \"query:scheduler-mutation-coord-stats\")");
    CHECK(stats && is_hash(*stats), "scheduler-mutation-coord-stats hash");
    CHECK(hash_int(cs, "query:scheduler-mutation-coord-stats",
                   "gc-pauses-attributed-to-mutation") >= 0,
          "gc-pauses-attributed-to-mutation");
    CHECK(hash_int(cs, "query:scheduler-mutation-coord-stats", "mutation-boundary-depth") >= 0,
          "mutation-boundary-depth");
    CHECK(hash_int(cs, "query:scheduler-mutation-coord-stats", "schema") == 618,
          "schema == 618 (#618 lineage)");
    return g_failed ? 1 : 0;
}
} // namespace aura_obs_run_wave26_591

int main() {
    std::println("=== test_obs_metrics_smoke_batch (wave 26) ===");
    if (int rc = aura_obs_run_wave26_478::run_478_primitive_error_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_obs_run_wave26_584::run_584_primitives_hotpath_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_obs_run_wave26_583::run_583_registry_core_stats_smoke(); rc != 0)
        return rc;
    ::aura::test::g_failed = 0;
    ::aura::test::g_passed = 0;
    if (int rc = aura_obs_run_wave26_591::run_591_scheduler_mutation_coord_stats_smoke(); rc != 0)
        return rc;
    std::println("\ntest_obs_metrics_smoke_batch: OK");
    return 0;
}
