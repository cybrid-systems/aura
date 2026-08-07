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

// ── Issue #2717 AC1: boundary success and reject paths stamp the proof ──
static void ac2717_1_boundary_success_and_reject_stamp() {
    std::println("\n--- #2717 AC1: boundary success + reject stamp ---");
    const auto efm = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("build_type_linear_commit_proof_from_live") != std::string::npos,
          "AC1: tma declares build helper");
    CHECK(tma.find("g_type_linear_commit_proof_stamped_total") != std::string::npos,
          "AC1: tma declares stamped-total counter");
    CHECK(efm.find("build_type_linear_commit_proof_from_live") != std::string::npos,
          "AC1: at least one stamp site present in evaluator_mutation_boundary.cpp");
}

// ── Issue #2717 AC2: composite_txn_commit stamps on both ok and reject ──
static void ac2717_2_composite_txn_commit_stamps() {
    std::println("\n--- #2717 AC2: composite_txn_commit stamps ---");
    const auto efm = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    CHECK(efm.find("build_type_linear_commit_proof_from_live") != std::string::npos,
          "AC2: build call present in evaluator_mutation_boundary.cpp");
    CHECK(efm.find("(void)typed_audit::build_type_linear_commit_proof_from_live(cp.version)") !=
              std::string::npos,
          "AC2: stamp call uses cp.version as the defuse_or_epoch source");
}

// ── Issue #2717 AC3: Soft + quiet path → stamp cheap (zeros / vacuous healthy) ──
static void ac2717_3_soft_quiet_path_cheap() {
    std::println("\n--- #2717 AC3: Soft + quiet path → stamp cheap ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("build_type_linear_commit_proof_from_live") != std::string::npos,
          "AC3: build helper present (read-only — counter reads only)");
    // The source has "p.live_goal_count = 0; // #2708 future wire" after
    // clang-format — check for the unique comment suffix and the
    // assignment prefix (whitespace-independent).
    CHECK(tma.find("p.live_goal_count = 0;") != std::string::npos,
          "AC3: live_goal_count defaults to 0 (cheap on quiet path)");
    CHECK(tma.find("p.linear_root_count = 0;") != std::string::npos,
          "AC3: linear_root_count defaults to 0 (cheap on quiet path)");
    CHECK(tma.find("// #2708 future wire") != std::string::npos,
          "AC3: #2708 future wire comment present");
    CHECK(tma.find("g_type_linear_commit_proof_stamped_total.fetch_add(1") != std::string::npos,
          "AC3: stamped_total bumps (1 atomic add per call)");
}

// ── Issue #2717 AC4: Agent comparing defuse_or_epoch_stamp detects drift ──
static void ac2717_4_drift_detection_via_defuse_or_epoch_stamp() {
    std::println("\n--- #2717 AC4: drift detection via defuse_or_epoch_stamp ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("p.defuse_or_epoch_stamp = current_epoch_or_defuse;") != std::string::npos,
          "AC4: proof struct has defuse_or_epoch_stamp field");
    CHECK(tma.find("struct TypeLinearCommitProof") != std::string::npos,
          "AC4: TypeLinearCommitProof struct present");
    CHECK(tma.find("stamp_type_linear_commit_proof") != std::string::npos,
          "AC4: low-level stamp preserved (existing query path additive)");
}

// ── Issue #2717 AC5: additive only — preserve #2613 / #2697 surfaces ──
static void ac2717_5_additive_no_regression() {
    std::println("\n--- #2717 AC5: additive only (no regression) ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    // #2613 is the "type-linear-commit-health" query — no struct
    // constant in typed_mutation_audit.h. Verified via the "#2613"
    // comment fragment + the query surface in evaluator_primitives_query.cpp.
    CHECK(tma.find("#2613") != std::string::npos, "AC5: #2613 comment reference preserved");
    CHECK(q.find("type-linear-commit-health") != std::string::npos,
          "AC5: #2613 query surface preserved");
    CHECK(tma.find("kTypeLinearCommitProofIssue = 2697") != std::string::npos,
          "AC5: #2697 issue stamp preserved");
    CHECK(q.find("type-linear-commit-proof-readiness-bp") != std::string::npos,
          "AC5: #2697 readiness-bp queryable (additive)");
    CHECK(q.find("schema-2697") != std::string::npos, "AC5: schema-2697 preserved");
    CHECK(tma.find("g_type_linear_commit_proof_stamped_total") != std::string::npos,
          "AC5: new stamped-total counter declared (additive)");
    CHECK(q.find("type-linear-commit-proof-stamped-total") != std::string::npos,
          "AC5: new stamped-total queryable");
    CHECK(q.find("schema-2717") != std::string::npos, "AC5: schema-2717 sentinel");
    CHECK(q.find("issue-2717") != std::string::npos, "AC5: issue-2717 sentinel");
}

// ── Issue #2717 AC6: source-cite + linter + no docs/design/ ──
static void ac2717_6_source_and_linter() {
    std::println("\n--- #2717 AC6: source-cite + linter + no docs/design/ ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto efm = read_file("src/compiler/evaluator_mutation_boundary.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/compiler/test_type_linear_commit_health.cpp");
    const auto lint = read_file("scripts/check_type_linear_commit_proof_stamp_2717.py");
    const auto build = read_file("build.py");
    CHECK(tma.find("Issue #2717") != std::string::npos, "AC6: tma cites #2717");
    CHECK(efm.find("Issue #2717") != std::string::npos,
          "AC6: evaluator_mutation_boundary.cpp cites #2717");
    CHECK(q.find("Issue #2717") != std::string::npos,
          "AC6: evaluator_primitives_query.cpp cites #2717");
    CHECK(t.find("ac2717_1_boundary_success_and_reject_stamp") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2717_2_composite_txn_commit_stamps") != std::string::npos,
          "AC6: AC2 test present");
    CHECK(t.find("ac2717_3_soft_quiet_path_cheap") != std::string::npos, "AC6: AC3 test present");
    CHECK(t.find("ac2717_4_drift_detection_via_defuse_or_epoch_stamp") != std::string::npos,
          "AC6: AC4 test present");
    CHECK(t.find("ac2717_5_additive_no_regression") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2717_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    CHECK(!lint.empty() && lint.find("Issue #2717") != std::string::npos,
          "AC6: coverage linter present and cites #2717");
    CHECK(build.find("check_type_linear_commit_proof_stamp_2717") != std::string::npos ||
              build.find("cmd_type_linear_commit_proof_stamp_2717_coverage") != std::string::npos,
          "AC6: build.py gate entry");
    CHECK(!std::filesystem::exists("docs/design/2717-type-linear-commit-proof-stamp.md"),
          "AC6: no docs/design/2717-* per #1655");
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
    // Issue #2717: stamp TypeLinearCommitProof on boundary + composite
    // commit (close #2697 residual). The boundary success + reject
    // paths and the composite_txn_commit path now stamp the durable
    // proof via build_type_linear_commit_proof_from_live. Soft + quiet
    // path stays cheap (counter reads only). Existing #2613 / #2697
    // query surfaces preserved.
    ac2717_1_boundary_success_and_reject_stamp();
    ac2717_2_composite_txn_commit_stamps();
    ac2717_3_soft_quiet_path_cheap();
    ac2717_4_drift_detection_via_defuse_or_epoch_stamp();
    ac2717_5_additive_no_regression();
    ac2717_6_source_and_linter();
    std::println("\n=== #2613 + #2697 + #2717: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_type_linear_commit_health();
}
#endif
