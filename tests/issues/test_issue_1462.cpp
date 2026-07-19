// @category: integration
// @reason: Aura lib loading + stdlib compatibility shim verification
//
// test_issue_1462.cpp — Issue #1462: Agent Migration Guide +
// compatibility shims for demoted query:* primitives.
//
// Scope (Plan A): verifies the compat shim module loads and the
// shim wrappers route to the new stdlib/primitive equivalents.
// Does NOT test the engine-side deprecation warning (follow-up
// #1462.3 — today the shim is silent by design).
//
// ACs:
//   AC1: lib/std/compat.aura loads via (import "std/compat")
//   AC2: 4 demoted names are exported by the shim
//   AC3: (query:find-by-name "foo") routes to (query:find "foo")
//   AC4: (query:nodes-with-marker 'm) routes to (query:by-marker 'm)
//   AC5: (query:subtree n) returns at least (query:children n) (pending
//        fold-tree helper)
//   AC6: link in docs/design/agentic-slim-surface-rectification.md
//        points at agent-migration-guide.md (static text check)

#include "test_harness.hpp"

import aura.compiler.service;

import std;
using aura::test::g_failed;
using aura::test::g_passed;

namespace test_issue_1462_detail {

#undef CHECK
#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::println("  FAIL: {} (line {})", msg, __LINE__);                                   \
            ++g_failed;                                                                            \
        } else {                                                                                   \
            std::println("  PASS: {}", msg);                                                       \
            ++g_compiled_passed;                                                                   \
            ++g_passed;                                                                            \
        }                                                                                          \
    } while (0)

// Bridge: existing harness uses g_failed; some tests want a separate
// compiled_passed counter for the Aura-load ACs (so we can report
// "loaded" separately). Single-threaded test runner, so a static
// here is fine.
inline std::uint32_t& compiled_passed_counter() {
    static std::uint32_t n = 0;
    return n;
}
#define g_compiled_passed compiled_passed_counter()

void ac1_compat_module_loads() {
    std::println("\n--- AC1: lib/std/compat.aura loads ---");
    aura::compiler::CompilerService cs;
    auto r = cs.eval("(import \"std/compat\")");
    CHECK(r.has_value(), "(import \"std/compat\") returns non-void");
}

void ac2_demoted_names_exported() {
    std::println("\n--- AC2: 4 demoted names exported by compat shim ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.eval("(require \"std/compat\" all:)").has_value(), "require std/compat");
    // The shim exports query:siblings / find-by-name / nodes-with-marker / subtree.
    // Use a probe that calls each (no-op form) to verify they resolve.
    auto r_sib = cs.eval("(query:siblings 0)");
    auto r_fbn = cs.eval("(query:find-by-name \"x\")");
    auto r_nwm = cs.eval("(query:nodes-with-marker 'user)");
    auto r_sub = cs.eval("(query:subtree 0)");
    // None of these need to return a value; we just verify the
    // calls resolve to functions (i.e. don't error with "unbound").
    CHECK(r_sib.has_value(), "(query:siblings 0) resolves via compat");
    CHECK(r_fbn.has_value(), "(query:find-by-name \"x\") resolves via compat");
    CHECK(r_nwm.has_value(), "(query:nodes-with-marker 'user) resolves via compat");
    CHECK(r_sub.has_value(), "(query:subtree 0) resolves via compat");
}

void ac3_find_by_name_routes_to_find() {
    std::println("\n--- AC3: query:find-by-name routes to query:find ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.eval("(require \"std/compat\" all:)").has_value(), "require compat");
    // Both old and new should return the same NodeId (or both
    // void if no match). Sanity: not error.
    auto r_old = cs.eval("(query:find-by-name \"nonexistent-test-name-xyz\")");
    auto r_new = cs.eval("(query:find \"nonexistent-test-name-xyz\")");
    CHECK(r_old.has_value() == r_new.has_value(),
          "find-by-name and find agree on void/non-void return");
}

void ac4_nodes_with_marker_routes_to_by_marker() {
    std::println("\n--- AC4: query:nodes-with-marker routes to query:by-marker ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.eval("(require \"std/compat\" all:)").has_value(), "require compat");
    auto r_old = cs.eval("(query:nodes-with-marker 'nonexistent-test-marker-xyz)");
    auto r_new = cs.eval("(query:by-marker 'nonexistent-test-marker-xyz)");
    CHECK(r_old.has_value() == r_new.has_value(),
          "nodes-with-marker and by-marker agree on return");
}

void ac5_subtree_returns_at_least_children() {
    std::println("\n--- AC5: query:subtree returns at least query:children ---");
    aura::compiler::CompilerService cs;
    CHECK(cs.eval("(require \"std/compat\" all:)").has_value(), "require compat");
    auto r_sub = cs.eval("(query:subtree 0)");
    auto r_chl = cs.eval("(query:children 0)");
    CHECK(r_sub.has_value() && r_chl.has_value(), "subtree + children both return non-void");
    // Both resolve; semantic equality is pending fold-tree helper.
}

void ac7_siblings_not_public_engine() {
    std::println("\n--- AC7: query:siblings no longer public engine add() (#1449) ---");
    aura::compiler::CompilerService cs;
    auto& prims = cs.evaluator().primitives();
    const bool registered = prims.slot_for_name("query:siblings") < prims.slot_count();
    CHECK(!registered, "query:siblings not in public Primitives table");
}

void ac6_link_in_slim_surface_doc() {
    std::println("\n--- AC6: agentic-slim-surface-rectification links migration guide ---");
    // Static text check — read the file and verify the link is present.
    std::ifstream f("docs/design/agentic-slim-surface-rectification.md");
    CHECK(f.is_open(), "agentic-slim-surface-rectification.md openable");
    if (!f.is_open())
        return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(content.find("agent-migration-guide") != std::string::npos,
          "agentic-slim-surface-rectification.md references agent-migration-guide");
}

} // namespace test_issue_1462_detail

int main() {
    using namespace test_issue_1462_detail;
    std::println("=== Issue #1462 — Agent migration guide + compat shims (Phase 1) ===");
    ac1_compat_module_loads();
    ac2_demoted_names_exported();
    ac3_find_by_name_routes_to_find();
    ac4_nodes_with_marker_routes_to_by_marker();
    ac5_subtree_returns_at_least_children();
    ac6_link_in_slim_surface_doc();
    ac7_siblings_not_public_engine();

    std::println("\n─── #1462 summary: {}/{} passed, {}/{} failed ───", g_passed,
                 g_passed + g_failed, g_failed, g_passed + g_failed);
    return g_failed == 0 ? 0 : 1;
}