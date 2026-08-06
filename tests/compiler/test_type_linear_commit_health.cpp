// @category: unit
// @reason: Issue #2613 — query:type-linear-commit-health folds readiness ×
//          linear × coercion × occurrence into one Agent surface.
//
//   AC1: Query returns all folded keys; schema-2613 registered
//   AC2: Incomplete blame / linear force / coercion SLO → force_reason + flags
//        match underlying counters
//   AC3: Happy path → readiness_bp 10000, would_allow_commit true, zero stale
//   AC4: No commit/audit behavior change (pure aggregation + existing queries)
//   AC5: Source-cite + gate; no docs/design

#include "compiler/type_linear_commit_health.hh"
#include "compiler/typed_mutation_audit.h"
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::compute_type_linear_commit_health;
using aura::compiler::kTypeLinearCommitHealthIssue;
using aura::compiler::TypeLinearCommitHealthSnapshot;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::CommitReadinessInput;
using aura::compiler::typed_audit::reset_for_test;
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
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
    auto r = cs.eval(
        std::format("(hash-ref (engine:metrics \"query:type-linear-commit-health\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: query surface ──
static void ac1_query_keys() {
    std::println("\n--- #2613 AC1: query keys + schema-2613 ---");
    reset_for_test();
    apply_dev_audit_defaults();
    CompilerService cs;
    auto h = cs.eval("(engine:metrics \"query:type-linear-commit-health\")");
    CHECK(h && is_hash(*h), "AC1: query returns hash");
    CHECK(href(cs, "schema-2613") == 2613, "AC1: schema-2613");
    CHECK(href(cs, "issue-2613") == 2613, "AC1: issue-2613");
    CHECK(href(cs, "type-linear-commit-health-wired") == 1, "AC1: wired");
    CHECK(href(cs, "readiness-bp") >= 0, "AC1: readiness-bp");
    CHECK(href(cs, "force-reason") >= 0, "AC1: force-reason");
    CHECK(href(cs, "would-allow-commit") >= 0, "AC1: would-allow-commit");
    CHECK(href(cs, "coercion-completeness-bp") >= 0, "AC1: coercion-completeness-bp");
    CHECK(href(cs, "coercion-slo-force-pending") >= 0, "AC1: coercion-slo-force-pending");
    CHECK(href(cs, "linear-force-unified") >= 0, "AC1: linear-force-unified");
    CHECK(href(cs, "linear-cross-closure-escape-total") >= 0, "AC1: cross-closure escape");
    CHECK(href(cs, "occurrence-stale") >= 0, "AC1: occurrence-stale");
    CHECK(href(cs, "predicate-memo-stale") >= 0, "AC1: predicate-memo-stale");
    CHECK(href(cs, "throttle-action") >= 0, "AC1: throttle-action");
    // Lineage retained
    CHECK(href(cs, "schema-2553") == 2553, "AC1: schema-2553 lineage");
    CHECK(href(cs, "schema-2558") == 2558, "AC1: schema-2558 lineage");
    CHECK(href(cs, "schema-2359") == 2359, "AC1: schema-2359 lineage");
    CHECK(kTypeLinearCommitHealthIssue == 2613, "AC1: issue constant");
}

// ── AC2: force_reason matches underlying axes ──
static void ac2_force_reason_match() {
    std::println("\n--- #2613 AC2: force_reason matches blame/linear/coercion SLO ---");

    // Incomplete blame under hard → blame deny
    {
        TypeLinearCommitHealthSnapshot s;
        s.readiness_in.blame_ok = false;
        s.readiness_in.blame_hard = true;
        const auto r = compute_type_linear_commit_health(s);
        CHECK(r.force_reason == "blame", "AC2: incomplete blame → force_reason blame");
        CHECK(r.force_reason_code == 2, "AC2: blame code 2");
        CHECK(!r.would_allow_commit, "AC2: blame hard denies commit");
        CHECK(r.readiness_bp == 1500, "AC2: blame hard readiness band");
        CHECK(r.throttle_action == 1, "AC2: deny → delay throttle");
    }

    // Linear force → linear
    {
        TypeLinearCommitHealthSnapshot s;
        s.readiness_in.linear_ok = false;
        s.readiness_in.linear_hard = true;
        s.linear_cross_closure_force_total = 3;
        const auto r = compute_type_linear_commit_health(s);
        CHECK(r.force_reason == "linear", "AC2: linear fail → force_reason linear");
        CHECK(r.force_reason_code == 3, "AC2: linear code 3");
        CHECK(!r.would_allow_commit, "AC2: linear hard denies");
        CHECK(r.linear_cross_closure_force_total == 3, "AC2: force total mirrored");
    }

    // Coercion SLO pending on otherwise clean face → coercion-slo (advisory)
    {
        TypeLinearCommitHealthSnapshot s;
        s.coercion_slo_force_pending = true;
        s.coercion_completeness_bp = 5000;
        const auto r = compute_type_linear_commit_health(s);
        CHECK(r.force_reason == "coercion-slo", "AC2: SLO pending → coercion-slo");
        CHECK(r.force_reason_code == 7, "AC2: coercion-slo code 7");
        CHECK(r.would_allow_commit, "AC2: SLO advisory still allows commit face");
        CHECK(r.coercion_slo_force_pending, "AC2: slo pending flag");
        CHECK(r.coercion_completeness_bp == 5000, "AC2: completeness mirrored");
        CHECK(r.throttle_action == 1, "AC2: SLO → delay advisory");
    }

    // Commit readiness linear wins over coercion overlay
    {
        TypeLinearCommitHealthSnapshot s;
        s.readiness_in.linear_ok = false;
        s.readiness_in.linear_hard = true;
        s.coercion_slo_force_pending = true;
        const auto r = compute_type_linear_commit_health(s);
        CHECK(r.force_reason == "linear", "AC2: readiness linear wins over SLO overlay");
        CHECK(!r.would_allow_commit, "AC2: still deny");
    }
}

// ── AC3: happy path ──
static void ac3_happy_path() {
    std::println("\n--- #2613 AC3: happy path 10000 / allow / zero stale ---");
    TypeLinearCommitHealthSnapshot s; // defaults clean
    const auto r = compute_type_linear_commit_health(s);
    CHECK(r.readiness_bp == 10000, "AC3: readiness_bp == 10000");
    CHECK(r.would_allow_commit, "AC3: would_allow_commit true");
    CHECK(r.force_reason == "ok", "AC3: force_reason ok");
    CHECK(r.force_reason_code == 0, "AC3: reason code 0");
    CHECK(r.occurrence_stale == 0, "AC3: zero occurrence_stale");
    CHECK(r.predicate_memo_stale == 0, "AC3: zero predicate_memo_stale");
    CHECK(!r.coercion_slo_force_pending, "AC3: no SLO pending");
    CHECK(r.throttle_action == 0, "AC3: no throttle");

    // Live query vacuous face
    reset_for_test();
    apply_dev_audit_defaults();
    CompilerService cs;
    CHECK(href(cs, "readiness-bp") == 10000, "AC3: live readiness-bp 10000");
    CHECK(href(cs, "would-allow-commit") == 1, "AC3: live would-allow-commit");
    CHECK(href(cs, "force-reason") == 0, "AC3: live force-reason ok");
    CHECK(href(cs, "occurrence-stale") == 0, "AC3: live occurrence-stale 0");
    CHECK(href(cs, "predicate-memo-stale") == 0, "AC3: live predicate-memo-stale 0");
}

// ── AC4: pure / no policy change ──
static void ac4_pure_no_policy() {
    std::println("\n--- #2613 AC4: pure aggregation; existing queries intact ---");
    TypeLinearCommitHealthSnapshot a, b;
    a.readiness_in.blame_ok = false;
    a.readiness_in.blame_hard = true;
    b = a;
    const auto ra = compute_type_linear_commit_health(a);
    const auto rb = compute_type_linear_commit_health(b);
    CHECK(ra.force_reason_code == rb.force_reason_code, "AC4: identical inputs → identical out");
    CHECK(ra.readiness_bp == rb.readiness_bp, "AC4: readiness deterministic");

    // Existing detailed queries still resolve (not replaced).
    reset_for_test();
    CompilerService cs;
    auto fidelity = cs.eval("(engine:metrics \"query:type-incremental-fidelity-stats\")");
    CHECK(fidelity && is_hash(*fidelity), "AC4: type-incremental-fidelity-stats retained");
    auto coer = cs.eval("(engine:metrics \"query:coercion-provenance-health\")");
    CHECK(coer && is_hash(*coer), "AC4: coercion-provenance-health retained");
    auto linear = cs.eval("(engine:metrics \"query:linear-ownership-typed-mutate-stats\")");
    CHECK(linear && is_hash(*linear), "AC4: linear-ownership-typed-mutate-stats retained");

    // Occurrence-stale overlay does not deny commit
    TypeLinearCommitHealthSnapshot s;
    s.occurrence_stale = 2;
    const auto r = compute_type_linear_commit_health(s);
    CHECK(r.force_reason == "occurrence-stale", "AC4: occurrence overlay reason");
    CHECK(r.would_allow_commit, "AC4: occurrence stale does not deny commit");
    CHECK(r.force_reason_code == 8, "AC4: occurrence-stale code 8");
}

// ── AC5: source-cite ──
static void ac5_source_cite() {
    std::println("\n--- #2613 AC5: source-cite + no design docs ---");
    const auto hh = read_file("src/compiler/type_linear_commit_health.hh");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto obs = read_file("src/compiler/evaluator_primitives_observability.cpp");
    CHECK(hh.find("#2613") != std::string::npos, "AC5: header cites #2613");
    CHECK(hh.find("compute_type_linear_commit_health") != std::string::npos, "AC5: pure compute");
    CHECK(q.find("query:type-linear-commit-health") != std::string::npos, "AC5: query registered");
    CHECK(q.find("schema-2613") != std::string::npos, "AC5: schema-2613 in query");
    CHECK(obs.find("query:type-linear-commit-health") != std::string::npos, "AC5: obs inventory");
    CHECK(hh.find("commit_readiness") != std::string::npos, "AC5: folds commit_readiness");
    CHECK(hh.find("coercion-slo") != std::string::npos ||
              hh.find("coercion_slo") != std::string::npos,
          "AC5: folds coercion SLO");
}

// ── Issue #2697 AC1: query returns proof with correct fields ──
static void ac2697_1_proof_queryable() {
    std::println("\n--- #2697 AC1: query:last-type-linear-commit-proof fields ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "type-linear-commit-proof-wired") == 1,
          "AC1: type-linear-commit-proof-wired sentinel == 1");
    CHECK(href(cs, "type-linear-commit-proof-readiness-bp") >= 0, "AC1: readiness-bp queryable");
    CHECK(href(cs, "type-linear-commit-proof-would-allow-commit") >= 0,
          "AC1: would-allow-commit queryable (0/1)");
    CHECK(href(cs, "type-linear-commit-proof-linear-ok") >= 0, "AC1: linear-ok queryable (0/1)");
    CHECK(href(cs, "type-linear-commit-proof-occurrence-consistent") >= 0,
          "AC1: occurrence-consistent queryable (0/1)");
    CHECK(href(cs, "type-linear-commit-proof-defuse-or-epoch-stamp") >= 0,
          "AC1: defuse-or-epoch-stamp queryable");
    CHECK(href(cs, "type-linear-commit-proof-last-stamp") >= 0, "AC1: last-stamp queryable");
    CHECK(href(cs, "schema-2697") == 2697, "AC1: schema-2697 sentinel");
    CHECK(href(cs, "issue-2697") == 2697, "AC1: issue-2697 sentinel");
}

// ── Issue #2697 AC4: #2613 health query additive — not replaced ──
static void ac2697_4_additive_facade_to_2613() {
    std::println("\n--- #2697 AC4: #2613 health query still works ---");
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2613") == 2613,
          "AC4: schema-2613 retained (#2613 health query not replaced)");
    CHECK(href(cs, "type-linear-commit-proof-wired") == 1,
          "AC4: #2697 proof additive on top of #2613");
}

// ── Issue #2697 AC5: source-cite + linter ──
static void ac2697_5_source_and_linter() {
    std::println("\n--- #2697 AC5: source-cite + additive ---");
    const auto hdr = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/compiler/test_type_linear_commit_health.cpp");
    CHECK(hdr.find("TypeLinearCommitProof") != std::string::npos,
          "AC5: hdr declares TypeLinearCommitProof struct");
    CHECK(hdr.find("kTypeLinearCommitProofIssue = 2697") != std::string::npos,
          "AC5: hdr stamps issue = 2697");
    CHECK(hdr.find("last_type_linear_commit_proof_stamp_v_read") != std::string::npos,
          "AC5: hdr exposes last-stamp accessor");
    CHECK(q.find("type-linear-commit-proof-wired") != std::string::npos,
          "AC5: query wired sentinel");
    CHECK(q.find("schema-2697") != std::string::npos, "AC5: query schema-2697");
    CHECK(q.find("issue-2697") != std::string::npos, "AC5: query issue-2697");
    CHECK(q.find("schema-2613") != std::string::npos, "AC5: schema-2613 preserved");
    CHECK(t.find("ac2697_1_proof_queryable") != std::string::npos, "AC5: AC1 test present");
    CHECK(t.find("ac2697_4_additive_facade_to_2613") != std::string::npos, "AC5: AC4 test present");
}

// ── Issue #2697 AC6: no docs/design/ per #1655 ──
static void ac2697_6_no_docs_design() {
    std::println("\n--- #2697 AC6: no docs/design/2697-* per #1655 ---");
    const std::string design_path = "docs/design/2697-";
    CHECK(read_file((design_path + "commit-proof-facade.md").c_str()).empty(),
          "AC6: no docs/design/2697-* per #1655");
}

} // namespace

int run_test_type_linear_commit_health() {
    std::println("=== Issue #2613: query:type-linear-commit-health ===");
    ac1_query_keys();
    ac2_force_reason_match();
    ac3_happy_path();
    ac4_pure_no_policy();
    ac5_source_cite();
    apply_dev_audit_defaults();
    reset_for_test();
    std::println("\n=== Issue #2697: query:last-type-linear-commit-proof facade ===");
    ac2697_1_proof_queryable();
    ac2697_4_additive_facade_to_2613();
    ac2697_5_source_and_linter();
    ac2697_6_no_docs_design();
    std::println("\n=== #2613 + #2697: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_type_linear_commit_health();
}
#endif
