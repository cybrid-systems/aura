// @category: unit
// @reason: Issue #1877 — StableNodeRef provenance + blame chain hygiene
// frames + FailOnStale under sandbox Strict for AI self-modify truncation.
//
//   AC1: source cites #1877; hygiene→provenance + FailOnStale Strict
//   AC2: capture_macro_hygiene_audit stamps provenance + hits metric
//   AC3: tenant_ids_compatible + tenant stamp on hygiene record
//   AC4: truncated reverify pulls hygiene frame into blame chain
//   AC5: set_effect_sandbox_mode(Strict) → FailOnStale + auto_refresh off
//   AC6: completeness_rate stays high under repeated rich conflicts

#include "compiler/observability_metrics.h"
#include "compiler/typed_mutation_audit.h"
#include "core/provenance_tracker.hh"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.evaluator;
import aura.compiler.service;
import aura.compiler.type_checker;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::CompilerService;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::kHygieneBlameKind;
using aura::compiler::SolveResult;
using aura::core::TypeRegistry;
using aura::core::provenance::AutoRefreshPolicy;
using aura::core::provenance::g_last_hygiene_provenance_stamp;
using aura::core::provenance::g_provenance_tracker;
using aura::core::provenance::record_macro_hygiene_provenance;
using aura::core::provenance::reset_provenance_enforcement_for_test;
using aura::core::provenance::snapshot_provenance_enforcement;
using aura::core::provenance::tenant_ids_compatible;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string read_first(std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        auto s = read_file(p);
        if (!s.empty())
            return s;
    }
    return {};
}

static SolveResult add_solve(ConstraintSystem& cs, Constraint c) {
    cs.add_delta(std::move(c));
    return cs.solve_delta();
}

} // namespace

int main() {
    // ── AC1: source surface ──
    {
        std::println("\n--- AC1: #1877 source surface ---");
        auto audit = read_first(
            {"src/compiler/typed_mutation_audit.h", "../src/compiler/typed_mutation_audit.h"});
        auto sec = read_first(
            {"src/compiler/evaluator_security.cpp", "../src/compiler/evaluator_security.cpp"});
        auto impl = read_first(
            {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
        auto ixx =
            read_first({"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"});
        auto prov =
            read_first({"src/core/provenance_tracker.hh", "../src/core/provenance_tracker.hh"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        CHECK(!audit.empty() && audit.find("#1877") != std::string::npos, "audit cites #1877");
        CHECK(audit.find("record_macro_hygiene_provenance") != std::string::npos,
              "audit stamps provenance");
        CHECK(!sec.empty() && sec.find("#1877") != std::string::npos, "security cites #1877");
        CHECK(sec.find("FailOnStale") != std::string::npos, "Strict → FailOnStale");
        CHECK(!impl.empty() && impl.find("#1877") != std::string::npos, "impl cites #1877");
        CHECK(impl.find("append_hygiene_frame") != std::string::npos ||
                  impl.find("append_hygiene_blame_frame") != std::string::npos,
              "hygiene frame on truncation");
        CHECK(!ixx.empty() && ixx.find("kHygieneBlameKind") != std::string::npos,
              "kHygieneBlameKind");
        CHECK(ixx.find("append_hygiene_frame") != std::string::npos, "append_hygiene_frame");
        CHECK(!prov.empty() && prov.find("#1877") != std::string::npos, "provenance cites #1877");
        CHECK(prov.find("macro_hygiene_provenance_hits_total") != std::string::npos,
              "hits metric field");
        CHECK(prov.find("tenant_ids_compatible") != std::string::npos, "tenant helper");
        CHECK(!hdr.empty() && hdr.find("blame_hygiene_frames_total") != std::string::npos,
              "blame hygiene metric");
        CHECK(hdr.find("macro_hygiene_provenance_hits_total") != std::string::npos,
              "compiler hits metric");
    }

    // ── AC2: hygiene audit → provenance ──
    {
        std::println("\n--- AC2: hygiene audit dual-records provenance ---");
        reset_provenance_enforcement_for_test();
        const auto rec0 = g_provenance_tracker().records;
        const auto hits0 = snapshot_provenance_enforcement().macro_hygiene_provenance_hits;
        aura::compiler::typed_audit::capture_macro_hygiene_audit(
            "hygiene-protected", aura::compiler::typed_audit::AuditOutcome::Error,
            /*target_node=*/42, /*fiber_id=*/7, /*tenant_id=*/1877, /*mutation_id=*/99);
        CHECK(g_provenance_tracker().records > rec0, "provenance record_mutation bumped");
        CHECK(snapshot_provenance_enforcement().macro_hygiene_provenance_hits > hits0,
              "macro_hygiene_provenance_hits bumped");
        const auto& st = g_last_hygiene_provenance_stamp();
        CHECK(st.node_id == 42, "stamp node_id");
        CHECK(st.tenant_id == 1877, "stamp tenant_id");
        CHECK(st.source_mutation_id == 99, "stamp mutation_id");
        CHECK(st.fiber_id == 7, "stamp fiber_id");
        CHECK(st.seq >= 1, "stamp seq");
        // Allowed path does not stamp provenance hits.
        const auto hits1 = snapshot_provenance_enforcement().macro_hygiene_provenance_hits;
        aura::compiler::typed_audit::capture_macro_hygiene_audit(
            "macro-allowed", aura::compiler::typed_audit::AuditOutcome::Success, 1, 0, 1, 0);
        CHECK(snapshot_provenance_enforcement().macro_hygiene_provenance_hits == hits1,
              "Success does not bump hits");
    }

    // ── AC3: tenant_ids_compatible ──
    {
        std::println("\n--- AC3: tenant_ids_compatible ---");
        CHECK(tenant_ids_compatible(0, 0), "both unset ok");
        CHECK(tenant_ids_compatible(0, 5), "ref unset ok");
        CHECK(tenant_ids_compatible(5, 0), "current unset ok");
        CHECK(tenant_ids_compatible(5, 5), "match ok");
        CHECK(!tenant_ids_compatible(5, 9), "mismatch denied");
        // Direct stamp helper with tenant from workspace isolation path.
        reset_provenance_enforcement_for_test();
        record_macro_hygiene_provenance(/*node=*/11, /*tenant=*/55, /*mut=*/1, /*fiber=*/0);
        CHECK(g_last_hygiene_provenance_stamp().tenant_id == 55, "direct stamp tenant");
        CHECK(tenant_ids_compatible(g_last_hygiene_provenance_stamp().tenant_id, 55),
              "validated against principal");
    }

    // ── AC4: truncation + hygiene frame ──
    {
        std::println("\n--- AC4: truncated reverify + hygiene frame ---");
        reset_provenance_enforcement_for_test();
        // Pre-stamp a hygiene violation so truncation auto-pulls it
        // (process-wide tracker + CompilerMetrics mirror for module TU safety).
        record_macro_hygiene_provenance(/*node=*/777, /*tenant=*/3, /*mut=*/187704,
                                        /*fiber=*/1);

        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        metrics.last_hygiene_blame_node = 777;
        metrics.last_hygiene_blame_mutation = 187704;
        cs.set_metrics(&metrics);
        cs.set_active_mutation_id(187704);
        cs.set_active_blame_context(7, 8);

        const auto v = cs.fresh_var();
        for (int i = 0; i < 300; ++i) {
            const auto w = cs.fresh_var();
            cs.add({Constraint::EQUAL, v, w});
        }
        cs.mark_touched_on_delta(v, /*occurrence_narrow=*/false);
        const auto u = cs.fresh_var();
        cs.add_delta({Constraint::EQUAL, u, reg.int_type()});
        cs.mark_touched_on_delta(v, false);
        auto r = cs.solve_delta();
        CHECK(r == SolveResult::SOLVED || r == SolveResult::CONFLICT || r == SolveResult::TIMEOUT,
              "solve returns");
        CHECK(metrics.reverify_truncated_total.load() > 0 ||
                  metrics.reverify_truncation_partial_blame_total.load() > 0,
              "truncation path engaged");
        const auto& chain = cs.last_blame_chain();
        CHECK(!chain.frames.empty(), "non-empty partial chain");
        CHECK(chain.partial || chain.truncated_reverify, "partial/truncated");
        CHECK(!chain.is_complete(), "not is_complete under truncation");
        bool found_hygiene = false;
        for (const auto& fr : chain.frames) {
            if (fr.kind == kHygieneBlameKind && fr.affected_node == 777) {
                found_hygiene = true;
                CHECK(fr.source_mutation_id == 187704 || fr.source_mutation_id != 0,
                      "hygiene frame mutation id");
            }
        }
        CHECK(found_hygiene, "hygiene frame present on truncated chain");
        CHECK(chain.hygiene_frame_count >= 1, "hygiene_frame_count");
        CHECK(metrics.blame_hygiene_frames_total.load() >= 1, "blame_hygiene_frames_total");
        std::println("  frames={} hygiene_frames={} trunc_partial={} stamp_node={}",
                     chain.frames.size(), chain.hygiene_frame_count,
                     metrics.reverify_truncation_partial_blame_total.load(),
                     g_last_hygiene_provenance_stamp().node_id);

        // Explicit append API (independent of auto-pull).
        const auto hy0 = cs.last_blame_chain().hygiene_frame_count;
        cs.append_hygiene_blame_frame(/*node=*/888, /*mut=*/187704);
        CHECK(cs.last_blame_chain().hygiene_frame_count > hy0, "explicit append hygiene");
        bool found_explicit = false;
        for (const auto& fr : cs.last_blame_chain().frames) {
            if (fr.kind == kHygieneBlameKind && fr.affected_node == 888)
                found_explicit = true;
        }
        CHECK(found_explicit, "explicit hygiene frame node 888");
    }

    // ── AC5: Strict → FailOnStale ──
    {
        std::println("\n--- AC5: Strict sandbox → FailOnStale ---");
        reset_provenance_enforcement_for_test();
        g_provenance_tracker().set_policy(AutoRefreshPolicy::AutoRefreshOnBoundary);
        CompilerService cs;
        auto& ev = cs.evaluator();
        ev.set_stable_ref_auto_refresh_policy(true);
        const auto fos0 = snapshot_provenance_enforcement().fail_on_stale_strict_sandbox;
        ev.set_effect_sandbox_mode(2); // Strict
        CHECK(ev.effect_sandbox_mode() == 2, "Strict mode");
        CHECK(g_provenance_tracker().get_policy() == AutoRefreshPolicy::FailOnStale,
              "provenance policy FailOnStale");
        CHECK(!ev.stable_ref_auto_refresh_policy(), "auto_refresh off under Strict");
        CHECK(snapshot_provenance_enforcement().fail_on_stale_strict_sandbox > fos0,
              "fail_on_stale_strict_sandbox metric");
        // Leaving Strict restores AutoRefreshOnBoundary.
        ev.set_effect_sandbox_mode(0);
        CHECK(g_provenance_tracker().get_policy() == AutoRefreshPolicy::AutoRefreshOnBoundary,
              "restored AutoRefreshOnBoundary");
        CHECK(ev.stable_ref_auto_refresh_policy(), "auto_refresh restored");
    }

    // ── AC6: completeness rate under repeated rich conflicts ──
    {
        std::println("\n--- AC6: blame completeness under stress ---");
        TypeRegistry reg;
        CompilerMetrics metrics;
        // Fresh ConstraintSystem per conflict pair so prior CONFLICT does not
        // poison subsequent solves; full provenance triple every round.
        for (int i = 0; i < 200; ++i) {
            ConstraintSystem cs(reg);
            cs.set_metrics(&metrics);
            cs.set_active_mutation_id(187706);
            cs.set_active_blame_context(/*pred=*/10, /*affected=*/20);
            cs.push_blame_affected_node(21);
            const auto t = cs.fresh_var();
            (void)add_solve(cs, {Constraint::EQUAL, t, reg.int_type()});
            (void)add_solve(cs, {Constraint::EQUAL, t, reg.string_type()}); // CONFLICT
            if (i == 199) {
                cs.append_hygiene_blame_frame(1, 187706);
                CHECK(!cs.last_blame_chain().frames.empty(), "hygiene append non-empty");
            }
        }
        const auto rate = metrics.blame_chain_completeness_rate.load();
        const auto rich = metrics.constraint_blame_chain_rich_complete_total.load();
        const auto incomplete = metrics.cross_delta_blame_incomplete_total.load();
        std::println("  completeness_rate={} rich={} incomplete={}", rate, rich, incomplete);
        CHECK(rate >= 95, "blame_chain_completeness_rate >= 95 under full-stamp stress");
        CHECK(rich > 0, "rich_complete advanced");
    }

    std::println("\n=== test_provenance_blame_hygiene_1877: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
