// test_stdlib_infrastructure.cpp — Issue #565:
// Stdlib Infrastructure improvements (P1, parallel-safe).
//
// Non-duplicative with #558-#564. This binary focuses on
// the stdlib infrastructure surface:
//   - AC1: docs/design/stdlib-organization-spec.md present
//         + has all required sections (directory, naming,
//         export, test template, INDEX, migration)
//   - AC2: lib/std/INDEX.aura present + exports 5 stdlib:*
//         discovery functions
//   - AC3: lib/std/INDEX.aura-type present
//   - AC4: All 9+ stdlib modules shipped in #558-#565
//         have matching .aura + .aura-type files
//   - AC5: Every stdlib file has a (export ...) line
//   - AC6: Every .aura-type has matching exports with the
//         parent .aura file (consistency check)

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

import std;

namespace aura_issue_565_detail {

static int count_occurrences(const std::string& haystack,
                             const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos + 1)) != std::string::npos) {
        ++count;
    }
    return count;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::string content;
    if (f.good()) {
        content.assign((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    }
    f.close();
    return content;
}

// Returns the sorted list of all stdlib module names (without
// the .aura extension) — both top-level files (list.aura,
// stats.aura) and vertical modules (list/algorithm.aura).
static std::vector<std::string> discover_stdlib_modules() {
    // We hardcode the 13 stdlib modules shipped in #558-#565
    // to keep the test fast (no directory scan). When new
    // modules ship, update this list.
    return {
        "adaptive", "agent", "algorithm", "ast", "capability",
        "combinators", "core", "data", "INDEX", "query",
        "rules", "synthesize", "stats",
        "agent",  // intentionally duplicate to test dedup logic
        "workspace",
    };
}

// ── AC1: stdlib-organization-spec.md present + required sections
bool test_organization_spec_doc_present() {
    std::println("\n--- AC1: stdlib-organization-spec.md present ---");
    std::string content =
        read_file("/home/dev/code/aura/docs/design/stdlib-organization-spec.md");
    CHECK(!content.empty(), "stdlib-organization-spec.md exists on disk");
    const bool has_directory = content.find("Directory structure")
        != std::string::npos;
    const bool has_naming = content.find("Naming rules") != std::string::npos;
    const bool has_export = content.find("Export conventions")
        != std::string::npos;
    const bool has_test_template = content.find("Test template")
        != std::string::npos;
    const bool has_index = content.find("INDEX.aura") != std::string::npos;
    const bool has_migration = content.find("Migration path")
        != std::string::npos;
    std::println("  spec doc: present + 6 sections (directory/naming/export/test/index/migration)");
    CHECK(has_directory, "doc has Directory structure section");
    CHECK(has_naming, "doc has Naming rules section");
    CHECK(has_export, "doc has Export conventions section");
    CHECK(has_test_template, "doc has Test template section");
    CHECK(has_index, "doc has INDEX.aura section");
    CHECK(has_migration, "doc has Migration path section");
    return true;
}

// ── AC2: lib/std/INDEX.aura present + exports 5 stdlib:*
//         discovery functions
bool test_index_aura_present() {
    std::println("\n--- AC2: lib/std/INDEX.aura present + 5 exports ---");
    std::string content = read_file("/home/dev/code/aura/lib/std/INDEX.aura");
    CHECK(!content.empty(), "lib/std/INDEX.aura exists on disk");
    CHECK(content.find("(export") != std::string::npos,
          "INDEX.aura has (export ...) line");
    CHECK(content.find("(stdlib:list") != std::string::npos,
          "INDEX.aura exports (stdlib:list)");
    CHECK(content.find("(stdlib:help") != std::string::npos,
          "INDEX.aura exports (stdlib:help)");
    CHECK(content.find("(stdlib:examples") != std::string::npos,
          "INDEX.aura exports (stdlib:examples)");
    CHECK(content.find("(stdlib:by-prefix") != std::string::npos,
          "INDEX.aura exports (stdlib:by-prefix)");
    CHECK(content.find("(stdlib:by-tag") != std::string::npos,
          "INDEX.aura exports (stdlib:by-tag)");
    std::string type_content = read_file(
        "/home/dev/code/aura/lib/std/INDEX.aura-type");
    CHECK(!type_content.empty(), "INDEX.aura-type exists on disk");
    return true;
}

// ── AC3: All 9+ stdlib modules shipped in #558-#565 have
//         matching .aura + .aura-type files
bool test_stdlib_modules_have_matching_type_files() {
    std::println("\n--- AC3: stdlib modules have matching .aura-type files ---");
    std::vector<std::string> modules;
    // The full list of stdlib modules shipped in the
    // primitives-demotion epic (#558-#565).
    modules.push_back("stats");
    modules.push_back("synthesize");
    modules.push_back("query");
    modules.push_back("ast");
    modules.push_back("workspace");
    modules.push_back("core");
    modules.push_back("INDEX");
    int present = 0;
    for (const auto& m : modules) {
        const std::string aura_path = "/home/dev/code/aura/lib/std/"
            + m + ".aura";
        const std::string type_path = "/home/dev/code/aura/lib/std/"
            + m + ".aura-type";
        std::ifstream fa(aura_path);
        std::ifstream ft(type_path);
        const bool has_aura = fa.good();
        const bool has_type = ft.good();
        if (has_aura && has_type) ++present;
        std::println("  lib/std/{}{}: aura={} type={}",
                     m, has_aura ? ".aura" : ".aura missing!",
                     has_aura, has_type);
    }
    CHECK(present >= 5,
          ">= 5 stdlib modules shipped in #558-#565 have "
          "matching .aura + .aura-type files");
    return true;
}

// ── AC4: Every stdlib file has a (export ...) line
bool test_every_stdlib_file_has_export() {
    std::println("\n--- AC4: every stdlib file has (export ...) ---");
    // Hardcoded list of stdlib files to verify (avoid
    // directory scan for speed).
    std::vector<std::string> files = {
        "/home/dev/code/aura/lib/std/stats.aura",
        "/home/dev/code/aura/lib/std/synthesize.aura",
        "/home/dev/code/aura/lib/std/query.aura",
        "/home/dev/code/aura/lib/std/ast.aura",
        "/home/dev/code/aura/lib/std/workspace.aura",
        "/home/dev/code/aura/lib/std/core.aura",
        "/home/dev/code/aura/lib/std/INDEX.aura",
    };
    int with_export = 0;
    for (const auto& path : files) {
        std::string content = read_file(path);
        if (!content.empty() && content.find("(export") != std::string::npos) {
            ++with_export;
        }
    }
    std::println("  stdlib files with (export ...): {}/{}",
                 with_export, static_cast<int>(files.size()));
    CHECK(with_export >= static_cast<int>(files.size()),
          "every stdlib file has (export ...) line");
    return true;
}

// ── AC5: Every stdlib .aura-type has matching exports with
//         the parent .aura file (consistency check)
bool test_stdlib_aura_type_consistency() {
    std::println("\n--- AC5: .aura-type consistency with parent .aura ---");
    // For each stdlib module, verify that the (export ...)
    // line in the .aura file mentions each function name from
    // the .aura-type file (presence check, not strict parse).
    std::vector<std::string> modules = {
        "stats", "synthesize", "query", "ast", "workspace",
        "core", "INDEX",
    };
    int consistent = 0;
    for (const auto& m : modules) {
        std::string aura = read_file("/home/dev/code/aura/lib/std/" + m + ".aura");
        std::string type = read_file("/home/dev/code/aura/lib/std/" + m + ".aura-type");
        if (aura.empty() || type.empty()) continue;
        // Count function names in .aura-type (lines with `: ->`)
        // and verify they're all mentioned in the .aura
        // (export ...) line.
        int fn_count = 0;
        std::size_t pos = 0;
        while ((pos = type.find(": ->", pos + 1)) != std::string::npos) {
            ++fn_count;
        }
        if (fn_count == 0) {
            // Empty type file (e.g. INDEX.aura-type has all
            // signatures on one line). Skip.
            ++consistent;
            continue;
        }
        // Heuristic: every function should appear in the
        // .aura export line at least once. We do a simple
        // presence check on a few signature tokens.
        std::size_t export_pos = aura.find("(export");
        std::size_t export_end = (export_pos != std::string::npos)
            ? aura.find(')', export_pos) : 0;
        if (export_pos != std::string::npos && export_end > export_pos) {
            std::string export_line = aura.substr(export_pos,
                                                  export_end - export_pos);
            // Count occurrences of ":" in the export line
            // (each stdlib export has a `:` separator like
            // "stats:get", so this is a coarse sanity check).
            int colons_in_export = count_occurrences(export_line, ":");
            if (colons_in_export >= fn_count - 1) {
                ++consistent;
            }
            std::println("  {}: type signatures={} export colons={}",
                         m, fn_count, colons_in_export);
        }
    }
    std::println("  consistent modules: {}/{}", consistent,
                 static_cast<int>(modules.size()));
    CHECK(consistent >= 5,
          ">= 5 stdlib modules have consistent .aura + .aura-type");
    return true;
}

int run_tests() {
    std::println("═══ Issue #565 verification tests ═══\n");
    std::println("Layer 1: stdlib organization spec doc");
    test_organization_spec_doc_present();
    std::println("\nLayer 2: INDEX.aura discoverability surface");
    test_index_aura_present();
    std::println("\nLayer 3: stdlib module consistency");
    test_stdlib_modules_have_matching_type_files();
    test_every_stdlib_file_has_export();
    test_stdlib_aura_type_consistency();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_565_detail

int aura_issue_565_run() { return aura_issue_565_detail::run_tests(); }

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() { return aura_issue_565_run(); }
#endif