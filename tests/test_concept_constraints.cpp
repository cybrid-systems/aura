// @category: unit
// @reason: Issue #1577 — centralized Pass concepts in
// src/core/concept_constraints.ixx (migrated from pass_manager.ixx).
//
//   AC1: module exports all Pass-related concepts
//   AC2: concepts constrain correctly (positive + negative stubs)
//   AC3: pass_manager re-exports (import pass_manager still sees Pass)
//   AC4: optimization_passes adapters still satisfy concepts
//   AC5: query:pass-concepts-stats schema 1577
//   AC6: no duplicate concept Pass definitions in pass_manager sources

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.concept_constraints;
import aura.compiler.pass_manager;
import aura.compiler.optimization_passes;
import aura.compiler.ir;
import aura.compiler.value;
import aura.compiler.service;

namespace {

using aura::test::g_failed;
using aura::test::g_passed;

// Minimal stubs for concept checking.
struct FullPass {
    void run(aura::ir::IRModule&) {}
    bool has_error() const { return false; }
    std::string_view name() const { return "full"; }
    bool is_block_dirty(std::uint32_t) const { return true; }
    void run(aura::ir::IRFunction&) {}
    void run(aura::ir::BasicBlock&) {}
    std::uint64_t pipeline_epoch_hint() const noexcept { return 0; }
    bool uses_soa_view() const { return false; }
};

struct PureAnalysisStub {
    void run(aura::ir::IRModule&) const {}
    bool has_error() const { return false; }
    std::string_view name() const { return "pure"; }
};

struct NotAPass {
    int x = 0;
};

static void ac1_inventory() {
    std::println("\n--- AC1: concept inventory ---");
    CHECK(aura::compiler::pass_concepts::kConceptConstraintsPhase >= 1, "phase >= 1");
    CHECK(aura::compiler::pass_concepts::kPassConceptCount == 10, "10 pass concepts");
    aura::compiler::pass_concepts::note_concept_constraints_import();
    CHECK(aura::compiler::pass_concepts::concept_constraints_import_hits.load() >= 1,
          "import hits");
}

static void ac2_positive_negative() {
    std::println("\n--- AC2: positive / negative concept checks ---");
    CHECK(static_cast<bool>(aura::compiler::Pass<FullPass>), "FullPass is Pass");
    CHECK(static_cast<bool>(aura::compiler::AnalysisPass<FullPass>), "FullPass Analysis");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<FullPass>), "FullPass DirtyAware");
    CHECK(static_cast<bool>(aura::compiler::IncrementalPass<FullPass>), "FullPass Incremental");
    CHECK(static_cast<bool>(aura::compiler::JITFriendlyPass<FullPass>), "FullPass JITFriendly");
    CHECK(static_cast<bool>(aura::compiler::SoAViewAwarePass<FullPass>), "FullPass SoAView");
    CHECK(static_cast<bool>(aura::compiler::ShapeStableAwarePass<FullPass>),
          "FullPass ShapeStable");

    CHECK(static_cast<bool>(aura::compiler::PureAnalysisPass<PureAnalysisStub>),
          "PureAnalysisStub pure");
    CHECK(static_cast<bool>(aura::compiler::AnalysisPass<PureAnalysisStub>),
          "PureAnalysisStub Analysis");

    CHECK(!static_cast<bool>(aura::compiler::Pass<NotAPass>), "NotAPass is not Pass");
    CHECK(!static_cast<bool>(aura::compiler::DirtyAwarePass<PureAnalysisStub>),
          "pure stub not DirtyAware");
}

static void ac3_reexport_via_pass_manager() {
    std::println("\n--- AC3: pass_manager re-exports concepts ---");
    // Same concept identity after export import.
    CHECK(
        static_cast<bool>(aura::compiler::Pass<aura::compiler::opt_registry::ConstantFoldingPass>),
        "CF Pass via pass_manager import chain");
    CHECK(static_cast<bool>(
              aura::compiler::DirtyAwarePass<aura::compiler::opt_registry::ConstantFoldingPass>),
          "CF DirtyAware");
    CHECK(static_cast<bool>(
              aura::compiler::PureAnalysisPass<aura::compiler::opt_registry::ComputeKindPass>),
          "CK PureAnalysis");
}

static void ac4_optimization_passes() {
    std::println("\n--- AC4: optimization_passes adapters ---");
    using namespace aura::compiler::opt_registry;
    CHECK(static_cast<bool>(aura::compiler::Pass<TypePropagationPass>), "TP Pass");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<TypePropagationPass>), "TP Dirty");
    CHECK(static_cast<bool>(aura::compiler::IncrementalPass<TypePropagationPass>), "TP Inc");
    CHECK(static_cast<bool>(aura::compiler::Pass<ShapeAwareFoldingPass>), "SA Pass");
    CHECK(static_cast<bool>(aura::compiler::DirtyAwarePass<ShapeAwareFoldingPass>), "SA Dirty");
}

static void ac5_stats_primitive() {
    std::println("\n--- AC5: query:pass-concepts-stats ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(engine:metrics \"query:pass-concepts-stats\")");
    CHECK(r.has_value(), "metrics returns");
    if (!r)
        return;
    CHECK(aura::compiler::types::is_hash(*r), "hash");
    auto schema = cs.eval("(hash-ref (engine:metrics \"query:pass-concepts-stats\") 'schema)");
    CHECK(schema.has_value() && aura::compiler::types::is_int(*schema) &&
              aura::compiler::types::as_int(*schema) == 1577,
          "schema == 1577");
    auto count =
        cs.eval("(hash-ref (engine:metrics \"query:pass-concepts-stats\") 'concept-count)");
    CHECK(count.has_value() && aura::compiler::types::is_int(*count) &&
              aura::compiler::types::as_int(*count) == 10,
          "concept-count == 10");
}

static void ac6_no_duplicate_in_pass_manager_source() {
    std::println("\n--- AC6: pass_manager has no concept Pass definition ---");
    std::ifstream in("src/compiler/pass_manager.ixx");
    CHECK(static_cast<bool>(in), "open pass_manager.ixx");
    if (!in)
        return;
    std::string line;
    int concept_pass_defs = 0;
    int import_constraints = 0;
    while (std::getline(in, line)) {
        if (line.find("concept Pass") != std::string::npos && line.find("//") == std::string::npos)
            ++concept_pass_defs;
        // Allow comments mentioning concept Pass
        if (line.find("concept Pass =") != std::string::npos)
            ++concept_pass_defs;
        if (line.find("import aura.core.concept_constraints") != std::string::npos)
            ++import_constraints;
    }
    CHECK(concept_pass_defs == 0, "no concept Pass = in pass_manager");
    CHECK(import_constraints >= 1, "pass_manager imports concept_constraints");
}

} // namespace

int main() {
    std::println("=== test_concept_constraints (#1577) ===");
    ac1_inventory();
    ac2_positive_negative();
    ac3_reexport_via_pass_manager();
    ac4_optimization_passes();
    ac5_stats_primitive();
    ac6_no_duplicate_in_pass_manager_source();
    std::println("\n=== {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
