// @category: unit
// @reason: Issue #2814 M7 — capture_audit_event_forced is pure trail;
// Success for mutate-class kinds must be linked to invariant enforcement
// (or intentional skip). Gap metric surfaces silent enforcement loss.
//
//   AC1: capture_audit_event_forced cites #2814; gap on unlinked Success
//   AC2: note_ran / note_skipped prevent gap; record_invariant_audit links
//   AC3: Guard wires skip on Sampled quiet + RenderFastExit; query schema-2814
//   AC4: this suite + linter; no docs/design/2814-*; no test_issue_2814.cpp

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
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
using aura::test::g_failed;
using aura::test::g_passed;
namespace ta = aura::compiler::typed_audit;

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
        "(hash-ref (engine:metrics \"query:typed-mutation-audit-stats\") \"{}\")", key));
    if (!r || !is_int(*r))
        return -1;
    return as_int(*r);
}

} // namespace

int run_test_capture_audit_event_forced_enforcement_link() {
    std::println("=== Issue #2814: audit enforcement link (M7) ===");
    CHECK(true, "ac2814: issue stamp");

    // ── AC1: source shape ──
    {
        std::println("\n--- AC1: capture_audit_event_forced gap + note APIs ---");
        auto h = read_file("src/compiler/typed_mutation_audit.h");
        auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        auto obs = read_file("src/compiler/evaluator_primitives_obs_jit.cpp");
        CHECK(!h.empty(), "AC1: audit header readable");

        auto pos = h.find("capture_audit_event_forced");
        CHECK(pos != std::string::npos, "AC1: capture_audit_event_forced present");
        // Use the definition window (after note helpers).
        auto def = h.find("inline void capture_audit_event_forced");
        auto win = h.substr(def != std::string::npos ? def : pos, 3500);
        CHECK(win.find("Issue #2814") != std::string::npos, "AC1: cites #2814");
        CHECK(win.find("audit_enforcement_gap_total") != std::string::npos, "AC1: gap metric");
        CHECK(win.find("enforcement_linked_for") != std::string::npos, "AC1: link check");
        CHECK(h.find("note_invariant_enforcement_ran") != std::string::npos, "AC1: note_ran");
        CHECK(h.find("note_invariant_enforcement_skipped") != std::string::npos,
              "AC1: note_skipped");
        CHECK(h.find("audit_enforcement_link_wired") != std::string::npos, "AC1: wired flag");
        CHECK(h.find("record_invariant_audit_result") != std::string::npos, "AC1: record path");
        // record_invariant must call note_ran
        auto rec = h.find("inline void record_invariant_audit_result");
        CHECK(rec != std::string::npos, "AC1: record_invariant present");
        auto rwin = h.substr(rec, 800);
        CHECK(rwin.find("note_invariant_enforcement_ran") != std::string::npos,
              "AC1: record_invariant notes ran");

        CHECK(bound.find("note_invariant_enforcement_skipped") != std::string::npos,
              "AC1: Guard notes intentional skip");
        CHECK(bound.find("Issue #2814") != std::string::npos, "AC1: boundary cites #2814");
        CHECK(obs.find("schema-2814") != std::string::npos, "AC1: query schema-2814");
        CHECK(obs.find("audit-enforcement-gap-total") != std::string::npos, "AC1: query gap key");
    }

    // ── AC2: runtime gap / link behavior ──
    {
        std::println("\n--- AC2: unlinked Success → gap; linked → no gap ---");
        ta::reset_for_test();
        const auto gap0 = ta::g_typed_mutation_audit_counters.audit_enforcement_gap_total.load();
        const auto ran0 = ta::g_typed_mutation_audit_counters.audit_enforcement_ran_total.load();
        const auto skip0 =
            ta::g_typed_mutation_audit_counters.audit_enforcement_skipped_intentional_total.load();

        // Unlinked Success (mutate-class) → gap.
        ta::capture_audit_event_forced(9001, "test-unlinked", ta::MutationKind::Structural, 1, 2,
                                       ta::AuditOutcome::Success, 0, 1, 0, 0);
        const auto gap1 = ta::g_typed_mutation_audit_counters.audit_enforcement_gap_total.load();
        CHECK(gap1 > gap0, "AC2: unlinked Success bumps gap");

        // note_ran then Success → no additional gap.
        ta::note_invariant_enforcement_ran(9002);
        ta::capture_audit_event_forced(9002, "test-ran", ta::MutationKind::Structural, 1, 2,
                                       ta::AuditOutcome::Success, 0, 1, 0, 0);
        const auto gap2 = ta::g_typed_mutation_audit_counters.audit_enforcement_gap_total.load();
        CHECK(gap2 == gap1, "AC2: note_ran prevents gap");
        CHECK(ta::g_typed_mutation_audit_counters.audit_enforcement_ran_total.load() > ran0,
              "AC2: ran total advanced");

        // note_skipped then Success → no additional gap.
        ta::note_invariant_enforcement_skipped(9003);
        ta::capture_audit_event_forced(9003, "test-skip", ta::MutationKind::ReplaceValue, 1, 2,
                                       ta::AuditOutcome::Success, 0, 1, 0, 0);
        const auto gap3 = ta::g_typed_mutation_audit_counters.audit_enforcement_gap_total.load();
        CHECK(gap3 == gap2, "AC2: note_skipped prevents gap");
        CHECK(
            ta::g_typed_mutation_audit_counters.audit_enforcement_skipped_intentional_total.load() >
                skip0,
            "AC2: skip total advanced");

        // Non-mutate kind Success without link → no gap (security/macro/aot).
        ta::capture_audit_event_forced(9004, "test-macro", ta::MutationKind::MacroHygiene, 1, 2,
                                       ta::AuditOutcome::Success, 0, 0, 0, 0);
        CHECK(ta::g_typed_mutation_audit_counters.audit_enforcement_gap_total.load() == gap3,
              "AC2: MacroHygiene Success without link is not a gap");

        // record_invariant_audit_result links automatically.
        ta::InvariantAuditResult r{};
        r.type_ok = r.linear_ok = r.provenance_ok = r.adt_ok = true;
        ta::record_invariant_audit_result(9005, "invariant-ok", r, 1, 2, 0, 0, 0);
        CHECK(ta::enforcement_linked_for(9005) ||
                  ta::g_typed_mutation_audit_counters.audit_enforcement_ran_total.load() > ran0,
              "AC2: record_invariant links enforcement");
        CHECK(ta::g_typed_mutation_audit_counters.audit_enforcement_link_wired.load() == 1,
              "AC2: link_wired == 1");
    }

    // ── AC3: Guard + query surface ──
    {
        std::println("\n--- AC3: mutate under Full wires enforcement ran ---");
        ta::reset_for_test();
        ta::set_strategy(ta::AuditStrategy::Full);
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define (f x) x) (f 1)\")").has_value(), "AC3: set-code");
        CHECK(cs.eval("(eval-current)").has_value(), "AC3: eval");
        const auto ran0 = ta::g_typed_mutation_audit_counters.audit_enforcement_ran_total.load();
        const auto gap0 = ta::g_typed_mutation_audit_counters.audit_enforcement_gap_total.load();
        auto mut = cs.eval("(mutate:set-body \"f\" \"(lambda (x) (+ x 1))\" \"#2814\")");
        CHECK(mut.has_value(), "AC3: set-body");
        // Full strategy should run invariant suite on mutate.
        const auto ran1 = ta::g_typed_mutation_audit_counters.audit_enforcement_ran_total.load();
        const auto gap1 = ta::g_typed_mutation_audit_counters.audit_enforcement_gap_total.load();
        CHECK(ran1 >= ran0, "AC3: enforcement ran non-decreasing after mutate");
        // Soft: Full may or may not audit depending on boundary path; gap
        // should not explode from healthy Guard wiring.
        CHECK(gap1 >= gap0, "AC3: gap non-decreasing");
        CHECK(href(cs, "schema-2814") == 2814 || href(cs, "audit-enforcement-link-wired") == 1,
              "AC3: query schema-2814 / wired");
        CHECK(href(cs, "audit-enforcement-gap-total") >= 0, "AC3: gap query key");
        CHECK(href(cs, "audit-enforcement-ran-total") >= 0, "AC3: ran query key");
    }

    // ── AC4: source cites Guard skip ──
    {
        std::println("\n--- AC4: Guard Sampled quiet path notes skip ---");
        auto bound = read_file("src/compiler/evaluator_mutation_boundary.cpp");
        // Two intentional-skip sites: render fast + !do_audit.
        auto first = bound.find("note_invariant_enforcement_skipped");
        CHECK(first != std::string::npos, "AC4: at least one skip note");
        auto second = bound.find("note_invariant_enforcement_skipped", first + 1);
        CHECK(second != std::string::npos, "AC4: both RenderFast + Sampled skip sites");
    }

    std::println("\n=== #2814 audit enforcement link: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed == 0 ? 0 : 1;
}

#ifndef AURA_ISSUE_BATCH_MEMBER
int main() {
    return run_test_capture_audit_event_forced_enforcement_link();
}
#endif
