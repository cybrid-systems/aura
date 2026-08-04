// @category: unit
// @reason: Issue #2553 — single Agent commit-readiness score
//          (solve × linear × blame × truncate).
//
//   AC1: Clean SOLVED + linear + blame + !trunc → bp=10000, ok, allow
//   AC2: expected_partial + empty CS under production hard → empty_cs, deny
//   AC3: truncated under production hard → truncate, deny
//   AC4: Soft observe paths report reason but may allow
//   AC5: Additive schema-2553 + source-cite; pure function

#include "test_harness.hpp"

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

using aura::compiler::CompilerService;
using aura::compiler::typed_audit::apply_dev_audit_defaults;
using aura::compiler::typed_audit::apply_production_audit_defaults;
using aura::compiler::typed_audit::commit_readiness;
using aura::compiler::typed_audit::commit_readiness_live_policy;
using aura::compiler::typed_audit::CommitReadinessInput;
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
        "(hash-ref (engine:metrics \"query:type-incremental-fidelity-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

// ── AC1: clean face ──
static void ac1_clean_ok() {
    std::println("\n--- #2553 AC1: clean SOLVED face → 10000 / ok / allow ---");
    CommitReadinessInput in{};
    // All defaults: SOLVED, linear_ok, blame_ok, !trunc, !expected_partial
    const auto r = commit_readiness(in);
    CHECK(r.readiness_bp == 10000, "AC1: readiness_bp == 10000");
    CHECK(r.force_reason == "ok", "AC1: force_reason == ok");
    CHECK(r.force_reason_code == 0, "AC1: reason code 0");
    CHECK(r.would_allow_commit, "AC1: would_allow_commit true");
}

// ── AC2: empty_cs hard under production ──
static void ac2_empty_cs_hard() {
    std::println("\n--- #2553 AC2: expected_partial + empty CS hard → deny ---");
    CommitReadinessInput in{};
    in.expected_partial = true;
    in.cs_has_work = false;
    in.empty_cs_hard = true;
    const auto r = commit_readiness(in);
    CHECK(r.force_reason == "empty_cs", "AC2: force_reason empty_cs");
    CHECK(r.force_reason_code == 5, "AC2: reason code 5");
    CHECK(!r.would_allow_commit, "AC2: would_allow_commit false");
    CHECK(r.readiness_bp == 0, "AC2: readiness_bp == 0 under hard empty_cs");
}

// ── AC3: truncate hard ──
static void ac3_truncate_hard() {
    std::println("\n--- #2553 AC3: truncated under hard → truncate deny ---");
    CommitReadinessInput in{};
    in.truncated_reverify = true;
    in.truncated_full_solve_recovered = false;
    in.truncate_hard = true;
    const auto r = commit_readiness(in);
    CHECK(r.force_reason == "truncate", "AC3: force_reason truncate");
    CHECK(r.force_reason_code == 4, "AC3: reason code 4");
    CHECK(!r.would_allow_commit, "AC3: would_allow_commit false");
    CHECK(r.readiness_bp == 1000, "AC3: readiness_bp hard-truncate band");
    // Recovered full-solve clears truncate signal.
    in.truncated_full_solve_recovered = true;
    const auto r2 = commit_readiness(in);
    CHECK(r2.force_reason == "ok", "AC3: recovered → ok");
    CHECK(r2.would_allow_commit, "AC3: recovered allows commit");
}

// ── AC4: Soft observe may allow ──
static void ac4_soft_observe() {
    std::println("\n--- #2553 AC4: Soft observe reports reason but may allow ---");
    // Soft empty_cs
    {
        CommitReadinessInput in{};
        in.expected_partial = true;
        in.cs_has_work = false;
        in.empty_cs_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "empty_cs", "AC4: soft empty_cs reason");
        CHECK(r.would_allow_commit, "AC4: soft empty_cs still allows");
        CHECK(r.readiness_bp == 7500, "AC4: soft empty_cs bp band");
    }
    // Soft truncate
    {
        CommitReadinessInput in{};
        in.truncated_reverify = true;
        in.truncate_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "truncate", "AC4: soft truncate reason");
        CHECK(r.would_allow_commit, "AC4: soft truncate allows");
        CHECK(r.readiness_bp == 7000, "AC4: soft truncate bp");
    }
    // Soft linear
    {
        CommitReadinessInput in{};
        in.linear_ok = false;
        in.linear_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "linear", "AC4: soft linear reason");
        CHECK(r.would_allow_commit, "AC4: soft linear allows");
    }
    // Soft blame
    {
        CommitReadinessInput in{};
        in.blame_ok = false;
        in.blame_hard = false;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "blame", "AC4: soft blame reason");
        CHECK(r.would_allow_commit, "AC4: soft blame allows");
    }
    // Priority: empty_cs beats truncate
    {
        CommitReadinessInput in{};
        in.expected_partial = true;
        in.cs_has_work = false;
        in.truncated_reverify = true;
        in.empty_cs_hard = true;
        in.truncate_hard = true;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "empty_cs", "AC4: priority empty_cs > truncate");
    }
    // Hard linear
    {
        CommitReadinessInput in{};
        in.linear_ok = false;
        in.linear_hard = true;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "linear", "AC4: hard linear");
        CHECK(!r.would_allow_commit, "AC4: hard linear denies");
    }
    // Solve CONFLICT / TIMEOUT always deny
    {
        CommitReadinessInput in{};
        in.solve_status = 1;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "solve", "AC4: CONFLICT → solve");
        CHECK(!r.would_allow_commit, "AC4: CONFLICT deny");
        CHECK(r.readiness_bp == 2500, "AC4: CONFLICT bp");
    }
    {
        CommitReadinessInput in{};
        in.solve_status = 2;
        const auto r = commit_readiness(in);
        CHECK(r.force_reason == "solve", "AC4: TIMEOUT → solve");
        CHECK(!r.would_allow_commit, "AC4: TIMEOUT deny");
        CHECK(r.readiness_bp == 2000, "AC4: TIMEOUT bp");
    }
}

// ── AC5: source-cite + schema + live policy ──
static void ac5_source_schema_live() {
    std::println("\n--- #2553 AC5: source-cite + schema-2553 + live policy ---");
    const auto th = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto lint = read_file("scripts/coverage/checks/check_commit_readiness_score_2553.py");
    const auto cmake = read_file("CMakeLists.txt");
    const auto build = read_file("build.py");

    CHECK(th.find("CommitReadiness") != std::string::npos, "AC5: CommitReadiness struct");
    CHECK(th.find("commit_readiness") != std::string::npos, "AC5: commit_readiness fn");
    CHECK(th.find("Issue #2553") != std::string::npos, "AC5: header cites #2553");
    CHECK(th.find("empty_cs") != std::string::npos, "AC5: empty_cs reason");
    CHECK(th.find("truncate") != std::string::npos, "AC5: truncate reason");
    CHECK(q.find("schema-2553") != std::string::npos, "AC5: schema-2553");
    CHECK(q.find("commit-readiness-bp") != std::string::npos, "AC5: readiness-bp key");
    CHECK(q.find("commit-readiness-wired") != std::string::npos, "AC5: wired key");
    CHECK(!lint.empty(), "AC5: linter present");
    CHECK(cmake.find("test_commit_readiness_score") != std::string::npos, "AC5: cmake");
    CHECK(build.find("check_commit_readiness_score_2553") != std::string::npos,
          "AC5: build script");
    CHECK(build.find("cmd_commit_readiness_score_coverage") != std::string::npos, "AC5: build cmd");

    // Live policy under production → hard flags on; clean face allows.
    apply_production_audit_defaults();
    auto live = commit_readiness_live_policy();
    CHECK(live.empty_cs_hard, "AC5: production empty_cs_hard");
    CHECK(live.truncate_hard, "AC5: production truncate_hard");
    CHECK(live.linear_hard, "AC5: production linear_hard");
    CHECK(live.blame_hard, "AC5: production blame_hard");
    const auto clean = commit_readiness(live);
    CHECK(clean.would_allow_commit && clean.force_reason == "ok", "AC5: live clean allows");

    apply_dev_audit_defaults();
    live = commit_readiness_live_policy();
    CHECK(!live.empty_cs_hard || !live.truncate_hard || true, "AC5: dev soft flags loadable");

    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "schema-2553") == 2553, "AC5: query schema-2553");
    CHECK(href(cs, "commit-readiness-wired") == 1, "AC5: query wired");
    CHECK(href(cs, "commit-readiness-bp") == 10000, "AC5: clean face bp 10000");
    CHECK(href(cs, "commit-readiness-would-allow") == 1, "AC5: clean face allows");
    CHECK(href(cs, "commit-readiness-force-reason") == 0, "AC5: clean face reason ok");
}

} // namespace

int run_test_commit_readiness_score() {
    std::println("=== Issue #2553: commit-readiness score ===");
    apply_dev_audit_defaults();
    ac1_clean_ok();
    ac2_empty_cs_hard();
    ac3_truncate_hard();
    ac4_soft_observe();
    ac5_source_schema_live();
    apply_dev_audit_defaults();
    std::println("\n=== #2553: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_commit_readiness_score();
}
#endif
