// @category: unit
// @reason: Issue #1885 — module_boundary.ixx layering + contracts
// Issue #1885 (#1978 renamed): issue# moved from filename to header.
//
// AC1: module exports ModuleLayer inventory + phase constants
// AC2: layer_may_depend_on / AllowedDependency encode Core←Compiler DAG
// AC3: CrossLayerStableRef / DirtyPropagator wrap concepts
// AC4: concepts.ixx / pass_manager.ixx / service.ixx reference module_boundary
// AC5: core re-exports boundary via import aura.core

#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <print>
#include <string>

import std;
import aura.core.module_boundary;
import aura.core;

namespace {

using aura::core::boundary::AllowedDependency;
using aura::core::boundary::CrossLayerDirtyPropagator;
using aura::core::boundary::CrossLayerStableRef;
using aura::core::boundary::kModuleBoundaryIssue;
using aura::core::boundary::kModuleBoundaryPhase;
using aura::core::boundary::kModuleLayerCount;
using aura::core::boundary::layer_may_depend_on;
using aura::core::boundary::layer_name;
using aura::core::boundary::ModuleLayer;
using aura::core::boundary::ProvenanceScoped;
using aura::test::g_failed;
using aura::test::g_passed;

// Minimal stubs for concept checks.
struct StableStub {
    bool is_valid(const int&) const { return true; }
    std::uint32_t id() const { return 1; }
};

struct DirtyStub {
    void mark_dirty(std::uint32_t) {}
    void mark_dirty_upward(std::uint32_t, std::size_t) {}
    bool is_dirty(std::uint32_t) const { return false; }
    void clear_dirty(std::uint32_t) {}
};

struct TenantStub {
    std::uint64_t tenant_id() const { return 42; }
};

struct NotStable {
    int x = 0;
};

void ac1_inventory() {
    std::println("\n--- AC1: inventory ---");
    CHECK(kModuleBoundaryIssue == 1885, "issue 1885");
    CHECK(kModuleBoundaryPhase >= 1, "phase >= 1");
    CHECK(kModuleLayerCount == 10, "10 layers");
    CHECK(layer_name(ModuleLayer::Core) == "core", "core name");
    CHECK(layer_name(ModuleLayer::Compiler) == "compiler", "compiler name");
    CHECK(layer_name(ModuleLayer::Parser) == "parser", "parser name");
}

void ac2_dependency_dag() {
    std::println("\n--- AC2: dependency DAG ---");
    // Allowed
    CHECK(layer_may_depend_on(ModuleLayer::Compiler, ModuleLayer::Core), "Compiler→Core");
    CHECK(layer_may_depend_on(ModuleLayer::Compiler, ModuleLayer::Parser), "Compiler→Parser");
    CHECK(layer_may_depend_on(ModuleLayer::Parser, ModuleLayer::Core), "Parser→Core");
    CHECK(layer_may_depend_on(ModuleLayer::Serve, ModuleLayer::Core), "Serve→Core");
    CHECK(layer_may_depend_on(ModuleLayer::Serve, ModuleLayer::Compiler), "Serve→Compiler bridge");
    CHECK(layer_may_depend_on(ModuleLayer::Core, ModuleLayer::Core), "Core→Core");
    // Forbidden
    CHECK(!layer_may_depend_on(ModuleLayer::Core, ModuleLayer::Compiler), "no Core→Compiler");
    CHECK(!layer_may_depend_on(ModuleLayer::Core, ModuleLayer::Parser), "no Core→Parser");
    CHECK(!layer_may_depend_on(ModuleLayer::Parser, ModuleLayer::Compiler), "no Parser→Compiler");
    CHECK(!layer_may_depend_on(ModuleLayer::Serve, ModuleLayer::Parser), "no Serve→Parser");
    CHECK(!layer_may_depend_on(ModuleLayer::Serve, ModuleLayer::Exec), "no Serve→Exec lateral");

    static_assert(AllowedDependency<ModuleLayer::Compiler, ModuleLayer::Core>);
    static_assert(!layer_may_depend_on(ModuleLayer::Core, ModuleLayer::Compiler));
    CHECK(static_cast<bool>(AllowedDependency<ModuleLayer::Compiler, ModuleLayer::Core>),
          "concept AllowedDependency Compiler→Core");
}

void ac3_cross_layer_contracts() {
    std::println("\n--- AC3: cross-layer contracts ---");
    CHECK(static_cast<bool>(CrossLayerStableRef<StableStub, int>), "StableStub CrossLayer");
    CHECK(!static_cast<bool>(CrossLayerStableRef<NotStable, int>), "NotStable not CrossLayer");
    CHECK(static_cast<bool>(CrossLayerDirtyPropagator<DirtyStub>), "DirtyStub propagator");
    CHECK(static_cast<bool>(ProvenanceScoped<TenantStub>), "TenantStub provenance");
    CHECK(!static_cast<bool>(ProvenanceScoped<NotStable>), "NotStable not ProvenanceScoped");
}

static bool file_contains(const char* path, std::string_view needle) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

void ac4_entry_file_references() {
    std::println("\n--- AC4: entry files reference module_boundary ---");
    CHECK(file_contains("src/core/concepts.ixx", "module_boundary"), "concepts.ixx ref");
    CHECK(file_contains("src/compiler/pass_manager.ixx", "module_boundary") ||
              file_contains("src/compiler/pass_manager.ixx", "ModuleLayer"),
          "pass_manager.ixx ref");
    CHECK(file_contains("src/compiler/service.ixx", "module_boundary") ||
              file_contains("src/compiler/service.ixx", "ModuleLayer"),
          "service.ixx ref");
    CHECK(file_contains("src/core/core.ixx", "module_boundary"), "core.ixx re-export");
    CHECK(file_contains("docs/architecture.md", "module_boundary"), "architecture.md");
}

void ac5_reexport_via_core() {
    std::println("\n--- AC5: re-export via aura.core ---");
    // import aura.core already done; constants must resolve under boundary::
    CHECK(aura::core::boundary::kModuleBoundaryIssue == 1885, "via aura.core");
    CHECK(static_cast<int>(aura::core::boundary::ModuleLayer::Compiler) == 2, "Compiler rank 2");
}

} // namespace

int main() {
    std::println("=== Issue #1885: module_boundary layering ===");
    ac1_inventory();
    ac2_dependency_dag();
    ac3_cross_layer_contracts();
    ac4_entry_file_references();
    ac5_reexport_via_core();
    std::println("\n=== #1885: {} passed, {} failed ===", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
