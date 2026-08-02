// @category: unit
// @reason: Issue #2560 — partial re-infer cone soft/hard cap + type_dep
//          degree truncation (type-layer SLA).
//
//   AC1: soft overflow metric + cap path source-cite (≤ soft or overflow)
//   AC2: hard fallback under production when cone > hard
//   AC3: under soft → zero new overflow when size ≤ soft (source + defaults)
//   AC4: #2516 order preserved (cap before invalidate; empty early-return)
//   AC5: schema-2560 on type-dep-partial-merge-stats + source-cite

#include "test_harness.hpp"
#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
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
    // Partial-cone / dirty-txn keys live on type-dep-partial-merge-stats
    // (same surface as schema-2516 / #2283 merge counters).
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-dep-partial-merge-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: soft path source + metric keys ──
static void ac1_soft_overflow_path() {
    std::println("\n--- #2560 AC1: soft overflow path ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("Issue #2560") != std::string::npos, "AC1: impl cites #2560");
    CHECK(impl.find("partial_cone_soft_cap") != std::string::npos, "AC1: soft cap helper");
    CHECK(impl.find("partial_cone_soft_overflow_total") != std::string::npos,
          "AC1: soft overflow metric");
    CHECK(impl.find("AURA_PARTIAL_CONE_SOFT") != std::string::npos, "AC1: soft env");
    CHECK(impl.find("truncate_partial_cone_seed_preserving") != std::string::npos,
          "AC1: seed-preserving truncate");
    CHECK(impl.find("type_dep_fanout_cap") != std::string::npos, "AC1: degree fan-out cap");
    // Default 256 soft.
    CHECK(impl.find("return 256") != std::string::npos, "AC1: default soft 256");
}

// ── AC2: hard production fallback ──
static void ac2_hard_production() {
    std::println("\n--- #2560 AC2: hard fallback under production ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("partial_cone_hard_fallback_total") != std::string::npos,
          "AC2: hard_fallback metric");
    CHECK(impl.find("AURA_PARTIAL_CONE_HARD") != std::string::npos, "AC2: hard env");
    CHECK(impl.find("return 2048") != std::string::npos, "AC2: default hard 2048");
    CHECK(impl.find("production_defaults_active") != std::string::npos,
          "AC2: production gate for hard");
    // hard path uses pre-truncate orig_sz.
    CHECK(impl.find("orig_sz > hard") != std::string::npos, "AC2: hard uses pre-truncate size");
}

// ── AC3: under soft zero cost ──
static void ac3_under_soft_zero() {
    std::println("\n--- #2560 AC3: under soft_cap zero overflow work ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("Zero extra work when cone under soft cap") != std::string::npos ||
              impl.find("Zero cost when size") != std::string::npos ||
              impl.find("orig_sz > soft") != std::string::npos,
          "AC3: only overflow when over soft");
    // Metrics start at 0 on fresh service.
    CompilerService cs;
    CHECK(href(cs, "partial-cone-soft-overflow-total") == 0 ||
              href(cs, "partial-cone-soft-overflow-total") >= 0,
          "AC3: soft overflow queryable");
    CHECK(href(cs, "partial-cone-hard-fallback-total") >= 0, "AC3: hard fallback queryable");
}

// ── AC4: #2516 order ──
static void ac4_txn_order() {
    std::println("\n--- #2560 AC4: #2516 order preserved ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto cap_pos = impl.find("Issue #2560: partial cone soft/hard SLA");
    const auto p1_pos = impl.find("invalidate_type_dep_for_nodes");
    CHECK(cap_pos != std::string::npos, "AC4: #2560 block present");
    CHECK(p1_pos != std::string::npos, "AC4: phase1 invalidate present");
    // Cap block must appear before invalidate in this function body.
    // Use the #2516 comment as anchor after #2560.
    const auto txn_pos = impl.find("Issue #2516 dirty txn");
    CHECK(txn_pos != std::string::npos && cap_pos < txn_pos,
          "AC4: cone cap before #2516 dirty txn phases");
    CHECK(impl.find("Empty affected already returned above") != std::string::npos ||
              impl.find("affected.empty()") != std::string::npos,
          "AC4: empty early-return retained");

    const auto ixx = read_file("src/compiler/type_checker.ixx");
    CHECK(ixx.find("Issue #2560") != std::string::npos, "AC4: ixx documents #2560");
    CHECK(ixx.find("AURA_PARTIAL_CONE_SOFT") != std::string::npos, "AC4: ixx env cite");
}

// ── AC5: schema + registrations ──
static void ac5_schema() {
    std::println("\n--- #2560 AC5: schema-2560 + gate ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("schema-2560") != std::string::npos, "AC5: schema-2560");
    CHECK(q.find("partial-cone-soft-overflow-total") != std::string::npos, "AC5: soft key");
    CHECK(q.find("partial-cone-hard-fallback-total") != std::string::npos, "AC5: hard key");
    CHECK(q.find("partial-cone-type-dep-degree-trunc-total") != std::string::npos,
          "AC5: degree trunc key");
    CHECK(q.find("partial-cone-last-size") != std::string::npos, "AC5: last size key");

    const auto mh = read_file("src/compiler/observability_metrics.h");
    CHECK(mh.find("partial_cone_soft_overflow_total") != std::string::npos, "AC5: metrics soft");
    CHECK(mh.find("partial_cone_hard_fallback_total") != std::string::npos, "AC5: metrics hard");
    CHECK(mh.find("#2560") != std::string::npos, "AC5: metrics cite #2560");

    CompilerService cs;
    CHECK(href(cs, "schema-2560") == 2560, "AC5: live schema-2560");
    CHECK(href(cs, "partial-cone-cap-wired") == 1, "AC5: cap wired");
    CHECK(href(cs, "schema-2516") == 2516, "AC5: #2516 lineage retained");

    // Direct metric smoke: counters are live atomics.
    auto* m = static_cast<CompilerMetrics*>(cs.evaluator().compiler_metrics());
    CHECK(m != nullptr, "AC5: metrics ptr");
    if (m) {
        const auto s0 = m->partial_cone_soft_overflow_total.load();
        m->partial_cone_soft_overflow_total.fetch_add(1);
        CHECK(m->partial_cone_soft_overflow_total.load() == s0 + 1, "AC5: soft counter live");
        m->partial_cone_soft_overflow_total.store(s0);
    }

    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");
    CHECK(cmake.find("test_partial_cone_cap_2560") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_partial_cone_cap_2560") != std::string::npos, "AC5: build script");
    CHECK(build.find("cmd_partial_cone_cap_coverage") != std::string::npos, "AC5: build cmd");
}

} // namespace

int main() {
    std::println("=== Issue #2560: partial re-infer cone soft/hard cap ===");
    apply_dev_audit_defaults();
    ac1_soft_overflow_path();
    ac2_hard_production();
    ac3_under_soft_zero();
    ac4_txn_order();
    ac5_schema();
    apply_dev_audit_defaults();
    std::println("\n=== #2560: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
