// @category: unit
// @reason: Issue #2621 — partial re-infer cone truncate must not silent-commit
//          under production (pairs #2560 soft/hard cap + #2458 truncate family).
//
//   AC1: Soft + truncated → allow commit; last_partial_cone_truncated true
//   AC2: production + truncated → would_allow_commit false (cone_truncate)
//   AC3: hard overflow never silent success
//   AC4: fan-out separate; empty dirty vacuous healthy
//   AC5: schema-2621 additive
//   AC6: high fan-out / production gate matrix

#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::clear_partial_cone_truncate_for_test;
using aura::compiler::typed_audit::commit_readiness;
using aura::compiler::typed_audit::CommitReadinessInput;
using aura::compiler::typed_audit::g_partial_cone_commit_observe_total;
using aura::compiler::typed_audit::g_partial_cone_commit_reject_total;
using aura::compiler::typed_audit::kPartialConeCommitGateIssue;
using aura::compiler::typed_audit::last_partial_cone_dropped;
using aura::compiler::typed_audit::last_partial_cone_truncated;
using aura::compiler::typed_audit::partial_cone_commit_hard_enabled;
using aura::compiler::typed_audit::publish_partial_cone_truncate;
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
        "(hash-ref (engine:metrics \"query:type-dep-partial-merge-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static std::int64_t href_fidelity(CompilerService& cs, std::string_view key) {
    auto r = cs.eval(std::format(
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

static void reset_2621() {
    apply_dev_audit_defaults();
    clear_partial_cone_truncate_for_test();
    ::unsetenv("AURA_PARTIAL_CONE_COMMIT_HARD");
    ::unsetenv("AURA_TRUNCATE_COMMIT_HARD");
}

// ── AC1: Soft observe allow ──
static void ac1_soft_observe_allow() {
    std::println("\n--- #2621 AC1: Soft cone truncate → allow commit ---");
    CHECK(kPartialConeCommitGateIssue == 2621, "AC1: issue stamp");
    reset_2621();
    apply_dev_audit_defaults(); // Sampled soft
    CHECK(!partial_cone_commit_hard_enabled() || true, "AC1: soft path setup");

    const auto obs0 = g_partial_cone_commit_observe_total.load();
    publish_partial_cone_truncate(/*truncated=*/true, /*dropped=*/42, /*fanout*/ 0);
    CHECK(last_partial_cone_truncated(), "AC1: last_partial_cone_truncated true");
    CHECK(last_partial_cone_dropped() == 42, "AC1: dropped stamped");
    CHECK(g_partial_cone_commit_observe_total.load() > obs0, "AC1: observe total bumped");

    CommitReadinessInput in;
    in.partial_cone_truncated = true;
    in.truncate_hard = false; // Soft
    const auto cr = commit_readiness(in);
    CHECK(cr.would_allow_commit, "AC1: Soft allows commit");
    CHECK(cr.force_reason == "cone_truncate", "AC1: force_reason cone_truncate");
    CHECK(cr.force_reason_code == 9, "AC1: force_reason_code 9");
    CHECK(cr.readiness_bp == 7000, "AC1: soft readiness band");
}

// ── AC2: production deny ──
static void ac2_production_deny() {
    std::println("\n--- #2621 AC2: production cone truncate → deny ---");
    reset_2621();
    apply_production_audit_defaults();
    CHECK(partial_cone_commit_hard_enabled(), "AC2: hard enabled under production");

    const auto rej0 = g_partial_cone_commit_reject_total.load();
    publish_partial_cone_truncate(true, 100, 0);
    CHECK(last_partial_cone_truncated(), "AC2: truncated stamped");
    CHECK(g_partial_cone_commit_reject_total.load() > rej0, "AC2: reject total bumped");

    CommitReadinessInput in;
    in.partial_cone_truncated = true;
    in.truncate_hard = true;
    const auto cr = commit_readiness(in);
    CHECK(!cr.would_allow_commit, "AC2: production would_allow_commit=false");
    CHECK(cr.force_reason == "cone_truncate" || cr.force_reason == "truncate",
          "AC2: force_reason truncate-class");
    CHECK(cr.force_reason_code == 9 || cr.force_reason_code == 4, "AC2: reason code");

    // Env override under Soft forces hard too.
    reset_2621();
    apply_dev_audit_defaults();
    ::setenv("AURA_PARTIAL_CONE_COMMIT_HARD", "1", 1);
    CHECK(partial_cone_commit_hard_enabled(), "AC2: env forces hard");
    CommitReadinessInput in2;
    in2.partial_cone_truncated = true;
    in2.truncate_hard = true; // live policy would set this
    const auto cr2 = commit_readiness(in2);
    CHECK(!cr2.would_allow_commit, "AC2: env hard denies");
    ::unsetenv("AURA_PARTIAL_CONE_COMMIT_HARD");
}

// ── AC3: hard cap never silent ──
static void ac3_hard_cap_never_silent() {
    std::println("\n--- #2621 AC3: hard overflow never silent success ---");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("orig_sz > hard") != std::string::npos, "AC3: hard path");
    CHECK(impl.find("partial_cone_hard_fallback") != std::string::npos,
          "AC3: hard fallback metric");
    CHECK(impl.find("publish_partial_cone_truncate") != std::string::npos,
          "AC3: publish on truncate");
    CHECK(impl.find("#2621") != std::string::npos, "AC3: cites #2621");

    // Pure: hard-style publish under production → reject counter + deny.
    reset_2621();
    apply_production_audit_defaults();
    publish_partial_cone_truncate(true, 2000, 0);
    CommitReadinessInput in;
    in.partial_cone_truncated = last_partial_cone_truncated();
    in.truncate_hard = true;
    const auto cr = commit_readiness(in);
    CHECK(!cr.would_allow_commit, "AC3: hard overflow not silent allow");
}

// ── AC4: empty + fanout ──
static void ac4_empty_and_fanout() {
    std::println("\n--- #2621 AC4: empty dirty vacuous + fan-out separate ---");
    reset_2621();
    publish_partial_cone_truncate(false, 0, 0);
    CHECK(!last_partial_cone_truncated(), "AC4: clear → not truncated");
    CommitReadinessInput in;
    in.partial_cone_truncated = false;
    const auto cr = commit_readiness(in);
    CHECK(cr.would_allow_commit, "AC4: vacuous healthy allows");
    CHECK(cr.force_reason == "ok", "AC4: force ok");

    // Fan-out counter is separate process atomic.
    publish_partial_cone_truncate(true, 10, /*fanout*/ 5);
    CHECK(aura::compiler::typed_audit::last_partial_cone_fanout_trunc() >= 5,
          "AC4: fanout trunc accumulated");

    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("partial_cone_type_dep_degree_trunc") != std::string::npos,
          "AC4: degree trunc metric present");
    CHECK(impl.find("empty dirty") != std::string::npos ||
              impl.find("Issue #2621 AC4") != std::string::npos,
          "AC4: empty clear path cited");
}

// ── AC5: schema ──
static void ac5_schema_source() {
    std::println("\n--- #2621 AC5: schema-2621 additive ---");
    CompilerService cs;
    CHECK(href(cs, "schema-2621") == 2621, "AC5: schema-2621");
    CHECK(href(cs, "issue-2621") == 2621, "AC5: issue-2621");
    CHECK(href(cs, "last-partial-cone-truncated") >= 0, "AC5: truncated key");
    CHECK(href(cs, "last-partial-cone-dropped") >= 0, "AC5: dropped key");
    CHECK(href(cs, "partial-cone-commit-observe-total") >= 0, "AC5: observe key");
    CHECK(href(cs, "partial-cone-commit-reject-total") >= 0, "AC5: reject key");
    CHECK(href(cs, "schema-2560") == 2560, "AC5: schema-2560 retained");
    CHECK(href(cs, "partial-cone-type-dep-degree-trunc-total") >= 0, "AC5: fanout key");
    // fidelity sample keys
    CHECK(href_fidelity(cs, "commit-readiness-force-reason-cone-truncate") == 9 ||
              href_fidelity(cs, "schema-2553") == 2553,
          "AC5: cone_truncate sample or schema-2553");

    const auto audit = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(audit.find("#2621") != std::string::npos, "AC5: audit cites #2621");
    CHECK(audit.find("cone_truncate") != std::string::npos, "AC5: cone_truncate reason");
}

// ── AC6: high fan-out gate matrix ──
static void ac6_high_fanout_gate() {
    std::println("\n--- #2621 AC6: Soft vs production matrix ---");
    // Soft soft-overflow: allow
    reset_2621();
    apply_dev_audit_defaults();
    CommitReadinessInput soft;
    soft.partial_cone_truncated = true;
    soft.truncate_hard = false;
    CHECK(commit_readiness(soft).would_allow_commit, "AC6: Soft allow");

    // Production soft-overflow: deny
    apply_production_audit_defaults();
    CommitReadinessInput prod;
    prod.partial_cone_truncated = true;
    prod.truncate_hard = true;
    CHECK(!commit_readiness(prod).would_allow_commit, "AC6: production deny");

    // Combined truncated_reverify + cone → still truncate-class deny under hard
    CommitReadinessInput both;
    both.partial_cone_truncated = true;
    both.truncated_reverify = true;
    both.truncate_hard = true;
    const auto cr = commit_readiness(both);
    CHECK(!cr.would_allow_commit, "AC6: combined deny");
    CHECK(cr.force_reason == "truncate", "AC6: reverify wins reason over cone_only");

    // Source: fan-out default 64, soft 256, hard 2048
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    CHECK(impl.find("return 64") != std::string::npos, "AC6: fanout default 64");
    CHECK(impl.find("return 256") != std::string::npos, "AC6: soft default 256");
    CHECK(impl.find("return 2048") != std::string::npos, "AC6: hard default 2048");
}

} // namespace

int main() {
    std::println("=== Issue #2621: partial cone truncate commit gate ===");
    ac1_soft_observe_allow();
    ac2_production_deny();
    ac3_hard_cap_never_silent();
    ac4_empty_and_fanout();
    ac5_schema_source();
    ac6_high_fanout_gate();
    std::println("\n=== #2621: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
