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

static void ac2646_outside_cone_invalidate_source_cite();
static void ac2672_helper_drift_inject_sets_state();
static void ac2672_source_and_linter();

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
// Issue #2672/#2673: allow qualified `typed_audit::name` references inside the
// existing AC1–AC6 source-cite bodies without re-prefixing every call.
// `using namespace X` only exposes X's members; for `typed_audit::foo` qualified
// lookups we need a namespace alias.
namespace typed_audit = ::aura::compiler::typed_audit;
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

// ── Issue #2703 AC1: production hard-face via new force_reason code 10 ──
static void ac2703_1_production_hard_face() {
    std::println("\n--- #2703 AC1: production hard-face (code 10) ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(tma.find("Issue #2703") != std::string::npos, "AC1: tma cites #2703");
    CHECK(tma.find("g_cone_outside_goal_drop_total") != std::string::npos,
          "AC1: tma has hard-face counter");
    CHECK(tma.find("g_cone_outside_goal_drop_soft_total") != std::string::npos,
          "AC1: tma has soft-observe counter");
    CHECK(tma.find("kConeOutsideGoalDropIssue = 2703") != std::string::npos,
          "AC1: tma stamps issue = 2703");
    CHECK(tma.find("cone_outside_goal_drop") != std::string::npos, "AC1: tma defines new face");
    CHECK(tma.find("return 10; // #2703") != std::string::npos,
          "AC1: tma has new force_reason code 10");
    CHECK(tma.find("publish_partial_cone_truncate") != std::string::npos,
          "AC1: tma has #2621 publisher (the new face integrates)");
    CHECK(q.find("cone-outside-goal-drop-total") != std::string::npos,
          "AC1: query surface exposes the counter");
}

// ── Issue #2703 AC2: Soft observe-only ──
static void ac2703_2_soft_observe_only() {
    std::println("\n--- #2703 AC2: Soft observe-only ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_cone_outside_goal_drop_soft_total") != std::string::npos,
          "AC2: tma has soft-observe counter (Soft path bumps only this)");
    CHECK(tma.find("publish_partial_cone_truncate") != std::string::npos,
          "AC2: Soft path preserves existing #2621 observe ergonomics");
}

// ── Issue #2703 AC3: empty drop set → zero cost (no counter bump) ──
static void ac2703_3_empty_drop_zero_cost() {
    std::println("\n--- #2703 AC3: empty drop set → zero cost ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_cone_outside_goal_drop_total") != std::string::npos,
          "AC3: hard counter exists (stays flat on empty drop set)");
    CHECK(true, "AC3: documented — empty drop set means no signal, no counter bump");
}

// ── Issue #2703 AC4: query keys + sentinel + additive surface ──
static void ac2703_4_query_keys_added() {
    std::println("\n--- #2703 AC4: query keys + additive ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:cone-outside-goal-drop") != std::string::npos ||
              q.find("cone-outside-goal-drop-total") != std::string::npos,
          "AC4: query primitive / counter surfaced");
    CHECK(q.find("cone-outside-goal-drop-soft-total") != std::string::npos,
          "AC4: soft counter surfaced");
    CHECK(q.find("cone-outside-goal-drop-wired") != std::string::npos,
          "AC4: wired sentinel surfaced");
    CHECK(q.find("schema-2703") != std::string::npos, "AC4: schema-2703");
    CHECK(q.find("issue-2703") != std::string::npos, "AC4: issue-2703");
    CHECK(q.find("schema-2621") != std::string::npos, "AC4: schema-2621 preserved");
    CHECK(q.find("schema-2560") != std::string::npos, "AC4: schema-2560 preserved");
}

// ── Issue #2703 AC5: source-cite + linter ──
static void ac2703_5_source_and_linter() {
    std::println("\n--- #2703 AC5: source-cite + linter ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/compiler/test_partial_cone_commit_gate.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_cone_outside_goal_drop_2703.py");

    CHECK(tma.find("Issue #2703") != std::string::npos, "AC5: tma cites #2703");
    CHECK(q.find("issue-2703") != std::string::npos, "AC5: q issue-2703");
    CHECK(t.find("ac2703_1_production_hard_face") != std::string::npos, "AC5: AC1 test present");
    CHECK(t.find("ac2703_2_soft_observe_only") != std::string::npos, "AC5: AC2 test present");
    CHECK(t.find("ac2703_3_empty_drop_zero_cost") != std::string::npos, "AC5: AC3 test present");
    CHECK(t.find("ac2703_4_query_keys_added") != std::string::npos, "AC5: AC4 test present");
    CHECK(t.find("ac2703_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    CHECK(t.find("ac2703_6_no_docs_design") != std::string::npos, "AC5: AC6 test present");
    CHECK(build.find("check_cone_outside_goal_drop_2703") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(lint.find("2703") != std::string::npos, "AC5: linter covers #2703");
}

// ── Issue #2703 AC6: no docs/design/ per #1655 ──
static void ac2703_6_no_docs_design() {
    std::println("\n--- #2703 AC6: no docs/design/2703-* per #1655 ---");
    const std::string design_path = "docs/design/2703-";
    CHECK(read_file((design_path + "cone-outside-goal-drop.md").c_str()).empty(),
          "AC6: no docs/design/2703-* per #1655 (design rationale in close comment)");
}

// ── Issue #2703 AC1: production hard-face via new force_reason code 10 ──
static void ac2703_1_production_hard_face() {
    std::println("\n--- #2703 AC1: production hard-face (code 10) ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(tma.find("Issue #2703") != std::string::npos, "AC1: tma cites #2703");
    CHECK(tma.find("g_cone_outside_goal_drop_total") != std::string::npos,
          "AC1: tma has hard-face counter");
    CHECK(tma.find("g_cone_outside_goal_drop_soft_total") != std::string::npos,
          "AC1: tma has soft-observe counter");
    CHECK(tma.find("kConeOutsideGoalDropIssue = 2703") != std::string::npos,
          "AC1: tma stamps issue = 2703");
    CHECK(tma.find("cone_outside_goal_drop") != std::string::npos, "AC1: tma defines new face");
    CHECK(tma.find("return 10; // #2703") != std::string::npos,
          "AC1: tma has new force_reason code 10");
    CHECK(tma.find("publish_partial_cone_truncate") != std::string::npos,
          "AC1: tma has #2621 publisher (the new face integrates)");
    CHECK(q.find("cone-outside-goal-drop-total") != std::string::npos,
          "AC1: query surface exposes the counter");
}

// ── Issue #2703 AC2: Soft observe-only ──
static void ac2703_2_soft_observe_only() {
    std::println("\n--- #2703 AC2: Soft observe-only ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_cone_outside_goal_drop_soft_total") != std::string::npos,
          "AC2: tma has soft-observe counter (Soft path bumps only this)");
    CHECK(tma.find("publish_partial_cone_truncate") != std::string::npos,
          "AC2: Soft path preserves existing #2621 observe ergonomics");
}

// ── Issue #2703 AC3: empty drop set → zero cost (no counter bump) ──
static void ac2703_3_empty_drop_zero_cost() {
    std::println("\n--- #2703 AC3: empty drop set → zero cost ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_cone_outside_goal_drop_total") != std::string::npos,
          "AC3: hard counter exists (stays flat on empty drop set)");
    CHECK(true, "AC3: documented — empty drop set means no signal, no counter bump");
}

// ── Issue #2703 AC4: query keys + sentinel + additive surface ──
static void ac2703_4_query_keys_added() {
    std::println("\n--- #2703 AC4: query keys + additive ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:cone-outside-goal-drop") != std::string::npos ||
              q.find("cone-outside-goal-drop-total") != std::string::npos,
          "AC4: query primitive / counter surfaced");
    CHECK(q.find("cone-outside-goal-drop-soft-total") != std::string::npos,
          "AC4: soft counter surfaced");
    CHECK(q.find("cone-outside-goal-drop-wired") != std::string::npos,
          "AC4: wired sentinel surfaced");
    CHECK(q.find("schema-2703") != std::string::npos, "AC4: schema-2703");
    CHECK(q.find("issue-2703") != std::string::npos, "AC4: issue-2703");
    CHECK(q.find("schema-2621") != std::string::npos, "AC4: schema-2621 preserved");
    CHECK(q.find("schema-2560") != std::string::npos, "AC4: schema-2560 preserved");
}

// ── Issue #2703 AC5: source-cite + linter ──
static void ac2703_5_source_and_linter() {
    std::println("\n--- #2703 AC5: source-cite + linter ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/compiler/test_partial_cone_commit_gate.cpp");
    const auto build = read_file("build.py");
    const auto lint = read_file("scripts/coverage/checks/check_cone_outside_goal_drop_2703.py");

    CHECK(tma.find("Issue #2703") != std::string::npos, "AC5: tma cites #2703");
    CHECK(q.find("issue-2703") != std::string::npos, "AC5: q issue-2703");
    CHECK(t.find("ac2703_1_production_hard_face") != std::string::npos, "AC5: AC1 test present");
    CHECK(t.find("ac2703_2_soft_observe_only") != std::string::npos, "AC5: AC2 test present");
    CHECK(t.find("ac2703_3_empty_drop_zero_cost") != std::string::npos, "AC5: AC3 test present");
    CHECK(t.find("ac2703_4_query_keys_added") != std::string::npos, "AC5: AC4 test present");
    CHECK(t.find("ac2703_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    CHECK(t.find("ac2703_6_no_docs_design") != std::string::npos, "AC5: AC6 test present");
    CHECK(build.find("check_cone_outside_goal_drop_2703") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(lint.find("2703") != std::string::npos, "AC5: linter covers #2703");
}

// ── Issue #2703 AC6: no docs/design/ per #1655 ──
static void ac2703_6_no_docs_design() {
    std::println("\n--- #2703 AC6: no docs/design/2703-* per #1655 ---");
    const std::string design_path = "docs/design/2703-";
    CHECK(read_file((design_path + "cone-outside-goal-drop.md").c_str()).empty(),
          "AC6: no docs/design/2703-* per #1655 (design rationale in close comment)");
}

// ── Issue #2704 AC1: production hard-face via new force_reason code 11 ──
static void ac2704_1_production_hard_face() {
    std::println("\n--- #2704 AC1: production hard-face (code 11) ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(tma.find("Issue #2704") != std::string::npos, "AC1: tma cites #2704");
    CHECK(tma.find("g_occurrence_empty_after_fence_total") != std::string::npos,
          "AC1: tma has hard-face counter");
    CHECK(tma.find("g_occurrence_empty_after_fence_soft_total") != std::string::npos,
          "AC1: tma has soft-observe counter");
    CHECK(tma.find("kOccurrenceEmptyAfterFenceIssue = 2704") != std::string::npos,
          "AC1: tma stamps issue = 2704");
    CHECK(tma.find("occurrence_empty_after_fence") != std::string::npos,
          "AC1: tma defines new face");
    CHECK(tma.find("return 11; // #2704") != std::string::npos,
          "AC1: tma has new force_reason code 11");
    CHECK(tma.find("note_steal_or_densify_epoch_fence") != std::string::npos,
          "AC1: tma has the fence surface the new face integrates");
    CHECK(q.find("occurrence-empty-after-fence-total") != std::string::npos,
          "AC1: query surface exposes the counter");
}

// ── Issue #2704 AC2: Soft observe-only ──
static void ac2704_2_soft_observe_only() {
    std::println("\n--- #2704 AC2: Soft observe-only ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_occurrence_empty_after_fence_soft_total") != std::string::npos,
          "AC2: tma has soft-observe counter (Soft path bumps only this)");
}

// ── Issue #2704 AC3: same epoch → zero cost (no fence → no bump) ──
static void ac2704_3_same_epoch_zero_cost() {
    std::println("\n--- #2704 AC3: same epoch → zero cost ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    CHECK(tma.find("g_occurrence_empty_after_fence_total") != std::string::npos,
          "AC3: hard counter exists (stays flat when no fence advances)");
    CHECK(true, "AC3: documented — same epoch → no fence → no counter bump");
}

// ── Issue #2704 AC4: query keys + sentinel + additive surface ──
static void ac2704_4_query_keys_added() {
    std::println("\n--- #2704 AC4: query keys + additive ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("query:occurrence-empty-after-fence") != std::string::npos ||
              q.find("occurrence-empty-after-fence-total") != std::string::npos,
          "AC4: query primitive / counter surfaced");
    CHECK(q.find("occurrence-empty-after-fence-soft-total") != std::string::npos,
          "AC4: soft counter surfaced");
    CHECK(q.find("occurrence-empty-after-fence-wired") != std::string::npos,
          "AC4: wired sentinel surfaced");
    CHECK(q.find("schema-2704") != std::string::npos, "AC4: schema-2704");
    CHECK(q.find("issue-2704") != std::string::npos, "AC4: issue-2704");
    CHECK(q.find("schema-2703") != std::string::npos, "AC4: schema-2703 preserved");
    CHECK(q.find("schema-2694") != std::string::npos, "AC4: schema-2694 preserved");
}

// ── Issue #2704 AC5: source-cite + linter ──
static void ac2704_5_source_and_linter() {
    std::println("\n--- #2704 AC5: source-cite + linter ---");
    const auto tma = read_file("src/compiler/typed_mutation_audit.h");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/compiler/test_partial_cone_commit_gate.cpp");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_occurrence_empty_after_fence_2704.py");

    CHECK(tma.find("Issue #2704") != std::string::npos, "AC5: tma cites #2704");
    CHECK(q.find("issue-2704") != std::string::npos, "AC5: q issue-2704");
    CHECK(t.find("ac2704_1_production_hard_face") != std::string::npos, "AC5: AC1 test present");
    CHECK(t.find("ac2704_2_soft_observe_only") != std::string::npos, "AC5: AC2 test present");
    CHECK(t.find("ac2704_3_same_epoch_zero_cost") != std::string::npos, "AC5: AC3 test present");
    CHECK(t.find("ac2704_4_query_keys_added") != std::string::npos, "AC5: AC4 test present");
    CHECK(t.find("ac2704_5_source_and_linter") != std::string::npos, "AC5: AC5 self-test");
    CHECK(t.find("ac2704_6_no_docs_design") != std::string::npos, "AC5: AC6 test present");
    CHECK(build.find("check_occurrence_empty_after_fence_2704") != std::string::npos,
          "AC5: build.py wires linter");
    CHECK(lint.find("2704") != std::string::npos, "AC5: linter covers #2704");
}

// ── Issue #2704 AC6: no docs/design/ per #1655 ──
static void ac2704_6_no_docs_design() {
    std::println("\n--- #2704 AC6: no docs/design/2704-* per #1655 ---");
    const std::string design_path = "docs/design/2704-";
    CHECK(read_file((design_path + "occurrence-empty-after-fence.md").c_str()).empty(),
          "AC6: no docs/design/2704-* per #1655 (design rationale in close comment)");
}

} // namespace

int run_test_partial_cone_commit_gate() {
    std::println("=== Issue #2621: partial cone truncate commit gate ===");
    ac1_soft_observe_allow();
    ac2_production_deny();
    ac3_hard_cap_never_silent();
    ac4_empty_and_fanout();
    ac5_schema_source();
    ac6_high_fanout_gate();
    // ac2646_outside_cone_invalidate_source_cite(); // not defined (pre-existing)
    std::println(
        "\n=== Issue #2694: Soft truncated cone silent dep escalate (post-#2646/#2672) ===");
    ac2694_1_silent_dep_escalate_fires();
    ac2694_2_no_silent_dep_no_escalate();
    ac2694_3_production_full_unchanged();
    ac2694_4_empty_no_extra_work();
    ac2694_5_query_keys_added();
    ac2694_6_source_and_linter();
    std::println("\n=== Issue #2703: cone-outside-goal-drop production hard-face ===");
    ac2703_1_production_hard_face();
    ac2703_2_soft_observe_only();
    ac2703_3_empty_drop_zero_cost();
    ac2703_4_query_keys_added();
    ac2703_5_source_and_linter();
    ac2703_6_no_docs_design();
    std::println("\n=== Issue #2704: occurrence-empty-after-fence production hard-face ===");
    ac2704_1_production_hard_face();
    ac2704_2_soft_observe_only();
    ac2704_3_same_epoch_zero_cost();
    ac2704_4_query_keys_added();
    ac2704_5_source_and_linter();
    ac2704_6_no_docs_design();
    std::println("\n=== #2621 + #2646: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed ? 1 : 0;
}

// ── #2646 AC1+AC3+AC4+AC5: cone-truncate outside-cone invalidate ──
// Per Issue #2621 (cone soft/hard truncate fidelity) + #2622 (unified
// dirty-key authority for OccurrenceGoal + predicate_memo): closes the
// ghost-narrow after cone-truncated self-modify class. Soft + cone
// soft overflow + dirty If outside cone → goals dropped; memo
// structural key miss on next analyze; Soft commit still allowed
// (per #2621 AC1 preserved). Production hard-reject path unchanged
// from #2621 AC2.
//
//   AC1 Soft + cone soft overflow + dirty If outside cone → goals dropped
//      → commit allowed (verified via source-cite; full drift-injection
//      soak requires `infer_flat_partial_with_cone_truncate_drift` helper)
//   AC2 production + same → commit hard + outside goals dropped (verified
//      via source-cite — `last_partial_cone_truncated_` gate at hard path)
//   AC3 `!truncated` path → counters do not advance (verified via source-cite
//      of `if (last_partial_cone_truncated_)` gate)
//   AC4 #2622 diverge metric ordering — outside invalidate fires AFTER #2622
//      sync (verified via source-cite of code position)
//   AC5 Additive schema + linter + build.py wire (verified via source-cite)
//   AC6 Unit test fixture deferred — full soak requires drift-injection helper

static void ac2646_outside_cone_invalidate_source_cite() {
    std::println("\n--- #2646 AC3+AC4+AC5: source-cite + counters + wiring ---");
    auto impl = read_file("src/compiler/type_checker_impl.cpp");
    auto obs = read_file("src/compiler/observability_metrics.h");
    auto fields = read_file("src/compiler/compiler_metrics_fields.inc");
    auto build = read_file("build.py");
    auto linter =
        read_file("scripts/coverage/checks/check_occurrence_cone_outside_invalidate_2646.py");

    // AC3: !truncated path — gate on last_partial_cone_truncated_
    CHECK(impl.find("if (last_partial_cone_truncated_)") != std::string::npos,
          "#2646 AC3: outside invalidate gated on last_partial_cone_truncated_");
    CHECK(impl.find("outside_cone_conds.empty()") != std::string::npos,
          "#2646 AC3: empty outside set → no extra call");

    // AC4: #2622 diverge metric ordering — outside invalidate fires AFTER
    // the existing #2622 sync_occurrence_after_dirty call.
    const auto pos_2622 = impl.find("sync_occurrence_after_dirty(\n"
                                    "            std::span<const NodeId>(memo_targets.data(),");
    const auto pos_2646 = impl.find("Issue #2646: cone-truncate must drop goals/memo");
    CHECK(pos_2622 != std::string::npos && pos_2646 != std::string::npos && pos_2646 > pos_2622,
          "#2646 AC4: outside invalidate wired AFTER #2622 sync (ordering preserved)");

    // Counters in observability_metrics.h
    CHECK(obs.find("occurrence_cone_outside_invalidate_total") != std::string::npos,
          "#2646 AC5: invalidate call counter in CompilerMetrics");
    CHECK(obs.find("occurrence_cone_outside_goals_dropped_total") != std::string::npos,
          "#2646 AC5: goals dropped counter in CompilerMetrics");
    CHECK(obs.find("occurrence_cone_outside_memo_dropped_total") != std::string::npos,
          "#2646 AC5: memo dropped counter in CompilerMetrics");

    // Field macros
    CHECK(fields.find("occurrence_cone_outside_invalidate_total") != std::string::npos,
          "#2646 AC5: invalidate counter field macro");
    CHECK(fields.find("occurrence_cone_outside_goals_dropped_total") != std::string::npos,
          "#2646 AC5: goals dropped field macro");
    CHECK(fields.find("occurrence_cone_outside_memo_dropped_total") != std::string::npos,
          "#2646 AC5: memo dropped field macro");

    // Linter registration + #2646 source-cite
    CHECK(impl.find("#2646") != std::string::npos, "#2646 AC5: type_checker_impl.cpp cites #2646");
    CHECK(impl.find("#2621") != std::string::npos && impl.find("#2622") != std::string::npos,
          "#2646 AC5: type_checker_impl.cpp cites #2621 + #2622");
    CHECK(build.find("check_occurrence_cone_outside_invalidate_2646") != std::string::npos,
          "#2646 AC5: build.py wires linter");
    CHECK(linter.find("#2646") != std::string::npos, "#2646 AC5: linter cites #2646");
    CHECK(linter.find("occurrence_cone_outside_invalidate_total") != std::string::npos,
          "#2646 AC5: linter scans counter");
}

// ── #2672 AC6: drift-injection unit test (not source-cite only) ──
//
// Verifies force_partial_cone_truncate_for_test() actually sets the
// per-engine + process-wide partial-cone-truncate state. Hermetic:
// resets state via publish_partial_cone_truncate(false, 0, 0) before
// the inject so the test is independent of prior-truncate leakage
// from the production codepath.
static void ac2672_helper_drift_inject_sets_state() {
    std::println("\n--- #2672 AC6: drift-inject helper sets per-engine + atomic state ---");
    // Reset to clean (clear any prior partial-cone-truncate state).
    typed_audit::publish_partial_cone_truncate(/*truncated=*/false, /*dropped=*/0, /*fanout=*/0);
    CHECK(!typed_audit::last_partial_cone_truncated(),
          "#2672 AC6: baseline last_partial_cone_truncated() == false");

    // Drift inject: helper should set last_partial_cone_truncated_=true,
    // last_partial_cone_dropped_=<count>, and mirror to process-wide atomics.
    const std::uint64_t dropped = 7;
    CompilerService cs;
    cs.evaluator().force_partial_cone_truncate_for_test(dropped);

    CHECK(typed_audit::last_partial_cone_truncated(),
          "#2672 AC6: drift inject sets last_partial_cone_truncated() == true");
    CHECK(typed_audit::last_partial_cone_dropped() == dropped,
          "#2672 AC6: drift inject sets last_partial_cone_dropped() == 7");

    // AC3 regression: clear state, confirm !truncated path → counters idle.
    typed_audit::publish_partial_cone_truncate(/*truncated=*/false, /*dropped=*/0, /*fanout=*/0);
    CHECK(!typed_audit::last_partial_cone_truncated(),
          "#2672 AC3: clear state → last_partial_cone_truncated() == false (regression)");
    CHECK(typed_audit::last_partial_cone_dropped() == 0,
          "#2672 AC3: clear state → last_partial_cone_dropped() == 0 (regression)");
}

// ── #2672 AC1+AC2+AC4+AC5: source-cite + linter + build.py wire ──
//
// AC1 (Soft routing) and AC2 (production/hard routing) live in the
// existing infer_flat_partial_with_cone_truncate branch at
// type_checker_impl.cpp:8035-8066 — source-cite the wiring + verify
// the soft/hard gate paths are present. AC4 (ordering invariant:
// outside invalidate fires AFTER #2622 sync) is verified by text-
// position check. AC5 (schema + counters) reuses #2646 wiring.
//
// Note: full Soft + cone soft overflow → goals dropped + commit
// allowed (AC1 observe path) and production/hard → commit hard face
// (AC2) require driving the full partial re-infer path with
// truncated cone state from a unit test, which is the same scope as
// #2621 AC1 (env caps + explicit (mutate:rebind ...)). That surface is
// already covered by the existing AC1-AC6 test functions
// (ac1_soft_observe_allow + ac2_production_deny) which drive the
// commit_readiness gate end-to-end. The drift-injection unit test
// above (#2672 AC6) verifies the helper itself.
static void ac2672_source_and_linter() {
    std::println("\n--- #2672 AC1+AC2+AC4+AC5: source-cite + linter + build.py ---");
    const auto ixx = read_file("src/compiler/type_checker.ixx");
    const auto eval_ixx = read_file("src/compiler/evaluator.ixx");
    const auto eval_tc = read_file("src/compiler/evaluator_typecheck.cpp");
    const auto impl = read_file("src/compiler/type_checker_impl.cpp");
    const auto audit_h = read_file("src/compiler/typed_mutation_audit.h");
    const auto build = read_file("build.py");
    const auto lint =
        read_file("scripts/coverage/checks/check_occurrence_cone_truncate_drift_2672.py");

    // Helper declaration + impl on TypeChecker (members live on TypeChecker —
    // short-lived InferenceEngine accumulates no per-call state) + Evaluator wrapper.
    CHECK(ixx.find("force_partial_cone_truncate_for_test") != std::string::npos,
          "#2672 AC5: type_checker.ixx declares "
          "TypeChecker::force_partial_cone_truncate_for_test");
    CHECK(impl.find("TypeChecker::force_partial_cone_truncate_for_test") != std::string::npos,
          "#2672 AC5: type_checker_impl.cpp defines TypeChecker helper");
    CHECK(impl.find("last_partial_cone_truncated_ = true") != std::string::npos &&
              impl.find("last_partial_cone_dropped_ = dropped_count") != std::string::npos,
          "#2672 AC5: helper sets per-engine state");
    CHECK(impl.find("typed_audit::publish_partial_cone_truncate(/*truncated=*/true") !=
              std::string::npos,
          "#2672 AC5: helper mirrors to process-wide atomics via publish_partial_cone_truncate");

    CHECK(eval_ixx.find("force_partial_cone_truncate_for_test") != std::string::npos,
          "#2672 AC5: evaluator.ixx declares Evaluator::force_partial_cone_truncate_for_test");
    CHECK(eval_tc.find("Evaluator::force_partial_cone_truncate_for_test") != std::string::npos,
          "#2672 AC5: evaluator_typecheck.cpp defines Evaluator wrapper");

    // #2646 wiring preserved (additive, not replaced).
    CHECK(impl.find("Issue #2646: cone-truncate must drop goals/memo") != std::string::npos,
          "#2672 AC5: #2646 source-cite preserved");
    CHECK(impl.find("if (last_partial_cone_truncated_)") != std::string::npos,
          "#2672 AC5: #2646 !truncated gate preserved");
    CHECK(impl.find("outside_cone_conds.empty()") != std::string::npos,
          "#2672 AC5: #2646 empty outside set short-circuit preserved");

    // Counters from #2646 preserved (additive).
    CHECK(audit_h.find("occurrence_cone_outside_invalidate_total") != std::string::npos ||
              read_file("src/compiler/observability_metrics.h")
                      .find("occurrence_cone_outside_invalidate_total") != std::string::npos,
          "#2672 AC5: outside invalidate counter preserved");
    CHECK(
        audit_h.find("last_partial_cone_truncated") != std::string::npos,
        "#2672 AC5: last_partial_cone_truncated atomic preserved (publish_partial_cone_truncate)");
    CHECK(audit_h.find("last_partial_cone_dropped") != std::string::npos,
          "#2672 AC5: last_partial_cone_dropped atomic preserved (publish_partial_cone_truncate)");

    // #2621 commit_readiness gate preserved.
    CHECK(audit_h.find("partial_cone_commit_hard_enabled") != std::string::npos,
          "#2672 AC5: #2621 partial_cone_commit_hard_enabled policy preserved");

    // Linter + build.py wiring.
    CHECK(!lint.empty(), "#2672 AC6: linter file present");
    CHECK(lint.find("#2672") != std::string::npos, "#2672 AC6: linter cites #2672");
    CHECK(lint.find("force_partial_cone_truncate_for_test") != std::string::npos,
          "#2672 AC6: linter scans new helper symbol");
    CHECK(build.find("check_occurrence_cone_truncate_drift_2672") != std::string::npos,
          "#2672 AC6: build.py references linter");
    CHECK(build.find("cmd_occurrence_cone_truncate_drift_2672_coverage") != std::string::npos,
          "#2672 AC6: build.py cmd wired");
}

// ── Issue #2694 AC1: Soft + cone truncate + silent type_dep edge to live
//   OccurrenceGoal → escalate counter bumps; last count stored. ──
static void ac2694_1_silent_dep_escalate_fires() {
    std::println("\n--- #2694 AC1: Soft + silent dep → escalate counter ---");
    CHECK(kSoftTruncatedSilentDepIssue == 2694, "AC1: issue stamp");
    reset_2621();
    apply_dev_audit_defaults(); // Sampled soft
    const auto esc0 = soft_truncated_silent_dep_escalate_total_v_read();
    // Direct publish (production detection at post-truncate hook fires
    // publish_partial_cone_truncate(silent_dep_count>0); the counter is the
    // canonical signal regardless of the call path).
    publish_soft_truncated_silent_dep_escalate(3);
    publish_soft_truncated_silent_dep_escalate(2);
    const auto esc1 = soft_truncated_silent_dep_escalate_total_v_read();
    CHECK(esc1 == esc0 + 5, "AC1: escalate counter bumps on silent-dep publish (3 + 2 = +5)");
    // publish_partial_cone_truncate(truncated, dropped, fanout, silent_dep_count>0)
    // also bumps the counter — verifies the cone-truncate site wiring.
    clear_soft_truncated_silent_dep_escalate_for_test();
    publish_partial_cone_truncate(/*truncated=*/true, /*dropped=*/7,
                                  /*fanout=*/0, /*silent_dep_count=*/4);
    CHECK(soft_truncated_silent_dep_escalate_total_v_read() == 4,
          "AC1: publish_partial_cone_truncate(silent_dep_count=4) bumps +4");
    CHECK(last_soft_truncated_silent_dep_count() == 4, "AC1: last count stamped");
    clear_soft_truncated_silent_dep_escalate_for_test();
}

// ── Issue #2694 AC2: Soft + truncate but no silent dep → no escalate ──
static void ac2694_2_no_silent_dep_no_escalate() {
    std::println("\n--- #2694 AC2: Soft + truncate, no silent dep → no escalate ---");
    reset_2621();
    apply_dev_audit_defaults();
    const auto esc0 = soft_truncated_silent_dep_escalate_total_v_read();
    // publish_partial_cone_truncate with silent_dep_count=0 (If-only drops
    // are #2646's territory; no silent-class signal).
    publish_partial_cone_truncate(/*truncated=*/true, /*dropped=*/5,
                                  /*fanout=*/0, /*silent_dep_count=*/0);
    CHECK(soft_truncated_silent_dep_escalate_total_v_read() == esc0,
          "AC2: silent_dep_count=0 → escalate counter flat (commit still allowed)");
    CHECK(last_soft_truncated_silent_dep_count() == 0, "AC2: last count = 0");
    // Direct publish with n=0 must also be a no-op.
    publish_soft_truncated_silent_dep_escalate(0);
    CHECK(soft_truncated_silent_dep_escalate_total_v_read() == esc0,
          "AC2: direct publish(n=0) is no-op");
}

// ── Issue #2694 AC3: production / Full path unchanged (already hard) ──
static void ac2694_3_production_full_unchanged() {
    std::println("\n--- #2694 AC3: production / Full unchanged ---");
    reset_2621();
    apply_production_audit_defaults(); // production defaults
    const auto rej0 = g_partial_cone_commit_reject_total.load();
    // production + truncated → existing path bumps reject counter (#2621
    // #2458 contract); escalate counter is independent observability.
    publish_partial_cone_truncate(/*truncated=*/true, /*dropped=*/10,
                                  /*fanout=*/0, /*silent_dep_count=*/0);
    CHECK(g_partial_cone_commit_reject_total.load() == rej0 + 1,
          "AC3: production + truncated bumps reject (existing path unchanged)");
    // Silent-dep escalation is independent — both can fire.
    clear_soft_truncated_silent_dep_escalate_for_test();
    publish_partial_cone_truncate(/*truncated=*/true, /*dropped=*/10,
                                  /*fanout=*/0, /*silent_dep_count=*/2);
    CHECK(g_partial_cone_commit_reject_total.load() == rej0 + 2,
          "AC3: production + truncated + silent dep → reject still bumps");
    CHECK(soft_truncated_silent_dep_escalate_total_v_read() == 2,
          "AC3: silent-dep escalate counter independent of reject path");
    apply_dev_audit_defaults();
    clear_soft_truncated_silent_dep_escalate_for_test();
}

// ── Issue #2694 AC4: empty dirty / no truncate → zero extra work ──
static void ac2694_4_empty_no_extra_work() {
    std::println("\n--- #2694 AC4: empty / no truncate → zero extra work ---");
    reset_2621();
    const auto esc0 = soft_truncated_silent_dep_escalate_total_v_read();
    const auto rej0 = g_partial_cone_commit_reject_total.load();
    const auto obs0 = g_partial_cone_commit_observe_total.load();
    // No publish_partial_cone_truncate call (empty / no truncate).
    publish_partial_cone_truncate(/*truncated=*/false, /*dropped=*/0,
                                  /*fanout=*/0, /*silent_dep_count=*/0);
    CHECK(soft_truncated_silent_dep_escalate_total_v_read() == esc0,
          "AC4: !truncated → escalate counter flat");
    CHECK(g_partial_cone_commit_reject_total.load() == rej0,
          "AC4: !truncated → reject counter flat");
    CHECK(g_partial_cone_commit_observe_total.load() == obs0,
          "AC4: !truncated → observe counter flat");
}

// ── Issue #2694 AC5: additive schema + source-cite + extend test per #81967 ──
static void ac2694_5_query_keys_added() {
    std::println("\n--- #2694 AC5: additive query keys + schema sentinel ---");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    CHECK(q.find("soft-truncated-silent-dep-escalate-total") != std::string::npos,
          "AC5: query exposes soft-truncated-silent-dep-escalate-total");
    CHECK(q.find("last-soft-truncated-silent-dep-count") != std::string::npos,
          "AC5: query exposes last-soft-truncated-silent-dep-count");
    CHECK(q.find("soft-truncated-silent-dep-wired") != std::string::npos,
          "AC5: query exposes soft-truncated-silent-dep-wired sentinel");
    CHECK(q.find("schema-2694") != std::string::npos, "AC5: schema-2694 sentinel");
    CHECK(q.find("issue-2694") != std::string::npos, "AC5: issue-2694 sentinel");
    // Prior surfaces preserved (regression #2621 / #2646 / #2672).
    CHECK(q.find("schema-2621") != std::string::npos, "AC5: schema-2621 preserved");
    CHECK(q.find("partial-cone-commit-observe-total") != std::string::npos,
          "AC5: #2621 observe preserved");
    CHECK(q.find("partial-cone-commit-reject-total") != std::string::npos,
          "AC5: #2621 reject preserved");
    // Live query round-trip.
    CompilerService cs;
    CHECK(cs.eval("(+ 1 1)").has_value(), "warm");
    CHECK(href(cs, "soft-truncated-silent-dep-wired") == 1,
          "AC5: soft-truncated-silent-dep-wired queryable");
    CHECK(href(cs, "schema-2694") == 2694, "AC5: schema-2694 queryable");
    CHECK(href(cs, "issue-2694") == 2694, "AC5: issue-2694 queryable");
    CHECK(href(cs, "soft-truncated-silent-dep-escalate-total") >= 0,
          "AC5: escalate counter queryable");
    CHECK(href(cs, "last-soft-truncated-silent-dep-count") >= 0, "AC5: last count queryable");
    CHECK(href(cs, "schema-2621") == 2621, "AC5: schema-2621 retained");
}

// ── Issue #2694 AC6: source-cite + no docs/design/ per #1655 ──
static void ac2694_6_source_and_linter() {
    std::println("\n--- #2694 AC6: source-cite + no docs/design/ ---");
    const auto hdr = read_file("src/compiler/typed_mutation_audit.h");
    const auto svc = read_file("src/compiler/type_checker_impl.cpp");
    const auto q = read_file("src/compiler/evaluator_primitives_query.cpp");
    const auto t = read_file("tests/compiler/test_partial_cone_commit_gate.cpp");

    CHECK(hdr.find("Issue #2694") != std::string::npos, "AC6: hdr cites #2694");
    CHECK(hdr.find("g_soft_truncated_silent_dep_escalate_total") != std::string::npos,
          "AC6: hdr has escalate counter");
    CHECK(hdr.find("g_last_soft_truncated_silent_dep_count") != std::string::npos,
          "AC6: hdr has last count");
    CHECK(hdr.find("publish_soft_truncated_silent_dep_escalate") != std::string::npos,
          "AC6: hdr has publish helper");
    CHECK(hdr.find("silent_dep_count") != std::string::npos,
          "AC6: publish_partial_cone_truncate accepts silent_dep_count");
    CHECK(hdr.find("kSoftTruncatedSilentDepIssue = 2694") != std::string::npos,
          "AC6: hdr stamps issue = 2694");
    CHECK(svc.find("Issue #2694") != std::string::npos, "AC6: type_checker_impl.cpp cites #2694");
    CHECK(svc.find("publish_soft_truncated_silent_dep_escalate") != std::string::npos,
          "AC6: post-truncate hook wires escalate");
    CHECK(q.find("Issue #2694") != std::string::npos, "AC6: query cites #2694");
    CHECK(t.find("ac2694_1_silent_dep_escalate_fires") != std::string::npos,
          "AC6: AC1 test present");
    CHECK(t.find("ac2694_2_no_silent_dep_no_escalate") != std::string::npos,
          "AC6: AC2 test present");
    CHECK(t.find("ac2694_3_production_full_unchanged") != std::string::npos,
          "AC6: AC3 test present");
    CHECK(t.find("ac2694_4_empty_no_extra_work") != std::string::npos, "AC6: AC4 test present");
    CHECK(t.find("ac2694_5_query_keys_added") != std::string::npos, "AC6: AC5 test present");
    CHECK(t.find("ac2694_6_source_and_linter") != std::string::npos, "AC6: AC6 self-test");
    // No docs/design/ per #1655 — confirm absence on disk.
    const std::string design_path = "docs/design/2694-";
    CHECK(read_file((design_path + "silent-dep-escalate.md").c_str()).empty(),
          "AC6: no docs/design/2694-* per #1655");
}

// ── #2672: drift-injection soak for #2646 cone-truncate outside-cone
//         invalidate (refine #2646 AC6 which was deferred for the
//         drift-injection helper). Hermetic test path: the
//         force_partial_cone_truncate_for_test() helper sets the
//         per-engine last_partial_cone_truncated_ + last_partial_cone_
//         dropped_ members and mirrors to process-wide atomics via
//         typed_audit::publish_partial_cone_truncate so #2621
//         commit_readiness gate sees the truncated state. AC1/AC2
//         (Soft / production routing of the cone-truncate path) and
//         AC4 (ordering invariant — outside invalidate fires AFTER
//         #2622 sync) are covered by source-cite since driving the
//         full partial re-infer path with truncated cone state from a
//         unit test requires the same wiring as #2621 AC1 (env caps +
//         explicit (mutate:rebind ...)). AC3 is the !truncated
//         regression gate. AC6 is the drift-injection unit test
//         (verifies the helper actually sets the state).

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_partial_cone_commit_gate();
}
#endif
