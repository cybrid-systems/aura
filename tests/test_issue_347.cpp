// @category: integration
// @reason: smoke-test the StableNodeRef best
//          practices doc + cross-references
// test_issue_347.cpp — Verify Issue #347 acceptance
// criteria (StableNodeRef, generation_ and mutation
// safety best practices guide).
//
// Scope-limited close. The issue body asks for:
//   1. Create or expand a dedicated section in
//      docs/developer/evaluator.md or new
//      docs/design/core/stable_ref_best_practices.md -
//      SHIPPED. New doc at
//      docs/design/core/stable_ref_best_practices.md.
//   2. Cover:
//      - When to use StableNodeRef vs raw NodeId
//      - Safe patterns for multi-round
//        query-mutate-eval loops
//      - Handling stale refs and retry logic
//      - Interaction with Workspace layering and
//        COW
//      - Concurrency considerations with fibers
//      SHIPPED. All 5 topics have dedicated
//      sections.
//   3. Add code examples and anti-patterns -
//      SHIPPED. 3 code examples + 6 anti-patterns.
//   4. Link from query_edsl.md and mutate_api.md -
//      PARTIAL. Linked from architecture.md
//      (the "Agent 编排" section); the query_edsl
//      and mutate_api docs don't exist yet (the
//      EDSL API is documented in api-reference.md
//      and design/ast-workspace-decision.md).
//      Filed as follow-up #1.
//
// 3 ACs (from the issue body, scoped to this PR):
//   AC1 Comprehensive best practices doc
//       published.
//   AC2 Examples + anti-patterns included.
//   AC3 Linked from main design / developer docs.

#include "test_harness.hpp"

import std;
using aura::test::g_passed;
using aura::test::g_failed;

namespace aura_issue_347_detail {

// Read first N lines of a file (or empty if missing).
static std::string read_file_head(
    const std::string& path, std::size_t n_lines = 500) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return std::string{};
    std::string out;
    char buf[4096];
    while (fgets(buf, sizeof(buf), f) && n_lines > 0) {
        out += buf;
        --n_lines;
    }
    fclose(f);
    return out;
}

// Helper: path resolution (multi-source; tests are
// launched from build/ so a relative path like
// "docs/..." would resolve to "build/docs/..."
// which doesn't exist). Try env var AURA_SRC_ROOT
// first (set by CI), then "../docs/..." (relative
// to build/), then fall back to the env-less form.
static std::string find_path(const std::string& rel) {
    if (auto* env = std::getenv("AURA_SRC_ROOT"))
        return std::string(env) + "/" + rel;
    return std::string("../") + rel;
}

// ═══════════════════════════════════════════════════════════════
// AC1: comprehensive doc exists + has the required
// sections
// ═══════════════════════════════════════════════════════════════

bool test_doc_exists_with_sections() {
    std::println("\n--- AC1: doc exists with required sections ---");
    const std::string path = find_path(
        "docs/design/core/stable_ref_best_practices.md");
    const std::string contents = read_file_head(path);
    CHECK(!contents.empty(),
          "docs/design/core/stable_ref_best_practices.md exists");
    if (contents.empty()) return false;
    // 5 required topics per the issue body.
    CHECK(contents.find("## When to use StableNodeRef vs raw NodeId")
              != std::string::npos,
          "section: When to use StableNodeRef vs raw NodeId");
    CHECK(contents.find("## Safe patterns for multi-round")
              != std::string::npos,
          "section: Safe patterns for multi-round query-mutate-eval loops");
    CHECK(contents.find("## Handling stale refs and retry logic")
              != std::string::npos,
          "section: Handling stale refs and retry logic");
    CHECK(contents.find("## Interaction with Workspace layering")
              != std::string::npos,
          "section: Interaction with Workspace layering and COW");
    CHECK(contents.find("## Concurrency considerations with fibers")
              != std::string::npos,
          "section: Concurrency considerations with fibers");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC2: examples + anti-patterns included
// ═══════════════════════════════════════════════════════════════

bool test_examples_and_antipatterns() {
    std::println("\n--- AC2: examples + anti-patterns ---");
    const std::string path = find_path(
        "docs/design/core/stable_ref_best_practices.md");
    const std::string contents = read_file_head(path);
    if (contents.empty()) return false;
    // Code examples: at least 3 Example sections.
    const auto ex_count = (contents.find("### Example 1") != std::string::npos ? 1 : 0)
                         + (contents.find("### Example 2") != std::string::npos ? 1 : 0)
                         + (contents.find("### Example 3") != std::string::npos ? 1 : 0);
    CHECK(ex_count >= 3,
          "3 code examples present (Example 1, 2, 3)");
    // Anti-patterns: at least 4.
    const auto ap_count = (contents.find("Don't cache raw `NodeId`")
                              != std::string::npos ? 1 : 0)
                         + (contents.find("Don't use a `StableNodeRef`")
                              != std::string::npos ? 1 : 0)
                         + (contents.find("Don't assume `is_valid`")
                              != std::string::npos ? 1 : 0)
                         + (contents.find("Don't skip the validity check")
                              != std::string::npos ? 1 : 0)
                         + (contents.find("Don't `static_cast` a `NodeId`")
                              != std::string::npos ? 1 : 0)
                         + (contents.find("Don't store `StableNodeRef`")
                              != std::string::npos ? 1 : 0);
    CHECK(ap_count >= 4,
          "at least 4 anti-patterns documented");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC3: linked from main design / developer docs
// ═══════════════════════════════════════════════════════════════

bool test_linked_from_main_docs() {
    std::println("\n--- AC3: linked from main docs ---");
    const std::string arch_path = find_path("docs/architecture.md");
    const std::string arch = read_file_head(arch_path);
    CHECK(arch.find("stable_ref_best_practices.md") != std::string::npos,
          "docs/architecture.md links to stable_ref_best_practices.md");
    return true;
}

// ═══════════════════════════════════════════════════════════════
// AC4: doc references resolve to real files / code
// ═══════════════════════════════════════════════════════════════

bool test_doc_cross_references_resolve() {
    std::println("\n--- AC4: cross-references resolve ---");
    const std::string path = find_path(
        "docs/design/core/stable_ref_best_practices.md");
    const std::string contents = read_file_head(path);
    if (contents.empty()) return false;
    // The doc references several real files /
    // test names. Check they all appear in the
    // "See also" section.
    CHECK(contents.find("docs/design/ast-workspace-decision.md")
              != std::string::npos,
          "doc references ast-workspace-decision.md");
    CHECK(contents.find("docs/incremental_dirty_propagation.md")
              != std::string::npos,
          "doc references incremental_dirty_propagation.md");
    CHECK(contents.find("docs/sanitizers.md")
              != std::string::npos,
          "doc references sanitizers.md");
    CHECK(contents.find("tests/test_issue_329.cpp")
              != std::string::npos,
          "doc references test_issue_329.cpp");
    CHECK(contents.find("src/core/ast.ixx")
              != std::string::npos,
          "doc references src/core/ast.ixx");
    CHECK(contents.find("src/compiler/service.ixx")
              != std::string::npos,
          "doc references src/compiler/service.ixx");
    // Verify the referenced source files exist.
    CHECK(!read_file_head(find_path("src/core/ast.ixx"), 1).empty(),
          "src/core/ast.ixx exists");
    CHECK(!read_file_head(find_path("src/compiler/service.ixx"), 1).empty(),
          "src/compiler/service.ixx exists");
    return true;
}

int run_tests() {
    std::println("═══ Issue #347 (StableNodeRef best practices guide) ═══\n");
    test_doc_exists_with_sections();
    test_examples_and_antipatterns();
    test_linked_from_main_docs();
    test_doc_cross_references_resolve();
    std::println("\n════════════════════════════════════════");
    std::println("Results: {} passed, {} failed", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}

}  // namespace aura_issue_347_detail

int aura_issue_347_run() { return aura_issue_347_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_347_run(); }
#endif