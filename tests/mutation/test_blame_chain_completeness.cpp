// @category: unit
// @reason: Issue #1873 — blame chain completeness for cross-delta
// Issue #1873 (#1978 renamed): issue# moved from filename to header.
// conflicts and truncated reverify (partial frames + completeness rate).
//
//   AC1: source cites #1873; DeltaBlameChain has partial/truncated + is_complete
//   AC2: rich conflict → is_complete + completeness_rate advances
//   AC3: truncated reverify leaves partial blame trail + metrics
//   AC4: missing provenance warning on bare add_delta
//   AC5: apply_coercion_map / narrowing provenance strengthen present

#include "compiler/observability_metrics.h"
#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.type_checker;
import aura.core.type;

namespace {

using aura::compiler::CompilerMetrics;
using aura::compiler::Constraint;
using aura::compiler::ConstraintSystem;
using aura::compiler::SolveResult;
using aura::core::TypeRegistry;
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
        std::println("\n--- AC1: #1873 source surface ---");
        auto impl = read_first(
            {"src/compiler/type_checker_impl.cpp", "../src/compiler/type_checker_impl.cpp"});
        auto ixx =
            read_first({"src/compiler/type_checker.ixx", "../src/compiler/type_checker.ixx"});
        auto hdr = read_first(
            {"src/compiler/observability_metrics.h", "../src/compiler/observability_metrics.h"});
        auto coer =
            read_first({"src/compiler/coercion_map.ixx", "../src/compiler/coercion_map.ixx"});
        CHECK(!impl.empty(), "read impl");
        CHECK(impl.find("#1873") != std::string::npos, "impl cites #1873");
        CHECK(impl.find("record_truncated_partial_blame") != std::string::npos,
              "truncation partial blame helper");
        CHECK(impl.find("update_blame_chain_completeness_rate") != std::string::npos,
              "completeness rate updater");
        CHECK(impl.find("blame_provenance_missing_warning_total") != std::string::npos,
              "missing provenance warning");
        CHECK(!ixx.empty() && ixx.find("#1873") != std::string::npos, "ixx cites #1873");
        CHECK(ixx.find("truncated_reverify") != std::string::npos, "DeltaBlameChain.truncated");
        CHECK(ixx.find("is_complete()") != std::string::npos, "is_complete accessor");
        CHECK(ixx.find("partial") != std::string::npos, "partial field");
        CHECK(!hdr.empty() && hdr.find("blame_chain_completeness_rate") != std::string::npos,
              "completeness_rate metric");
        CHECK(hdr.find("reverify_truncation_partial_blame_total") != std::string::npos,
              "truncation partial metric");
        CHECK(!coer.empty() && coer.find("#1873") != std::string::npos,
              "apply_coercion_map strengthen cites #1873");
    }

    // ── AC2: rich complete → rate ──
    {
        std::println("\n--- AC2: rich conflict is_complete + rate ---");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        cs.set_active_mutation_id(187302);
        cs.set_active_blame_context(/*pred=*/42, /*affected=*/99);
        cs.push_blame_affected_node(100);
        const auto t = cs.fresh_var();
        CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
              "baseline");
        const auto rich0 = metrics.constraint_blame_chain_rich_complete_total.load();
        CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
              "rich CONFLICT");
        const auto& chain = cs.last_blame_chain();
        CHECK(!chain.frames.empty(), "frames present");
        CHECK(chain.complete, "rich triple complete flag");
        CHECK(chain.is_complete(), "is_complete() true (not partial/truncated)");
        CHECK(!chain.truncated_reverify, "not truncated");
        CHECK(metrics.constraint_blame_chain_rich_complete_total.load() > rich0,
              "rich_complete bumped");
        CHECK(metrics.blame_chain_completeness_rate.load() > 0, "completeness_rate > 0");
        CHECK(metrics.blame_chain_completeness_rate.load() <= 100, "rate ≤ 100");
    }

    // ── AC3: truncated reverify partial trail ──
    {
        std::println("\n--- AC3: truncated reverify partial blame ---");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        cs.set_active_mutation_id(187303);
        cs.set_active_blame_context(7, 8);

        // Stack >256 *clean* (non-dirty) constraints on a single free
        // root via add() — not add_delta — so they stay clean and the
        // var is not bound to a concrete type (keeps var_to_constraints_
        // keyed on the TYPE_VAR rep). With few touched roots the
        // effective_reverify_limit stays at the 256 base cap →
        // truncation + partial blame trail (#1873).
        const auto v = cs.fresh_var();
        for (int i = 0; i < 300; ++i) {
            const auto w = cs.fresh_var();
            cs.add({Constraint::EQUAL, v, w}); // clean, unbound
        }
        cs.mark_touched_on_delta(v, /*occurrence_narrow=*/false);
        const auto u = cs.fresh_var();
        cs.add_delta({Constraint::EQUAL, u, reg.int_type()}); // dirty worklist
        cs.mark_touched_on_delta(v, false);
        auto r = cs.solve_delta();
        CHECK(r == SolveResult::SOLVED || r == SolveResult::CONFLICT || r == SolveResult::TIMEOUT,
              "solve returns");
        const auto trunc = metrics.reverify_truncated_total.load();
        const auto partial_blame = metrics.reverify_truncation_partial_blame_total.load();
        std::println("  reverify_truncated={} partial_blame={} unscanned={}", trunc, partial_blame,
                     cs.last_blame_chain().unscanned_constraint_count);
        CHECK(trunc > 0, "reverify_truncated_total bumped");
        CHECK(partial_blame > 0, "truncation partial blame metric");
        const auto& chain = cs.last_blame_chain();
        CHECK(!chain.frames.empty(), "partial chain frames");
        CHECK(chain.partial || chain.truncated_reverify, "partial/truncated flags");
        CHECK(!chain.is_complete(), "truncated chain not is_complete");
        CHECK(chain.truncated_reverify, "truncated_reverify flag");
        CHECK(chain.unscanned_constraint_count > 0, "unscanned count set");
        CHECK(chain.root_mutation_id == 187303, "root mutation preserved on partial");
    }

    // ── AC4: missing provenance warning ──
    {
        std::println("\n--- AC4: missing provenance warning ---");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        // No active mutation / blame context.
        const auto t = cs.fresh_var();
        const auto w0 = metrics.blame_provenance_missing_warning_total.load();
        cs.add_delta({Constraint::EQUAL, t, reg.int_type()});
        CHECK(metrics.blame_provenance_missing_warning_total.load() > w0,
              "warning bumped when stamp has no context");
    }

    // ── AC5: conflict still dumpable without mutation (partial) ──
    {
        std::println("\n--- AC5: no-mutation conflict still dumpable ---");
        TypeRegistry reg;
        ConstraintSystem cs(reg);
        CompilerMetrics metrics;
        cs.set_metrics(&metrics);
        // No mutation id — incomplete but non-empty chain.
        const auto t = cs.fresh_var();
        CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.int_type()}) == SolveResult::SOLVED,
              "baseline no mut");
        CHECK(add_solve(cs, {Constraint::EQUAL, t, reg.string_type()}) == SolveResult::CONFLICT,
              "conflict no mut");
        const auto& chain = cs.last_blame_chain();
        CHECK(!chain.frames.empty(), "partial frames even without mutation");
        CHECK(chain.partial || !chain.is_complete(), "not is_complete without mut");
        CHECK(metrics.cross_delta_blame_incomplete_total.load() > 0, "incomplete counted");
    }

    std::println("\n=== test_blame_chain_completeness_1873: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
