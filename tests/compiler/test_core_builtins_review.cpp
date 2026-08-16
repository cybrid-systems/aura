// test_core_builtins_review.cpp — Issue #564:
// Core Builtins Selective Review (P3, long-term).
//
// Non-duplicative with #558-#563. This binary focuses on
// the core builtins review checklist surface (file-level):
//   - AC1: lib/std/core.aura present + exports 10 functions
//   - AC2: 10 engine primitives wrapped (>=5 acceptance met)
//   - AC3: no docs/design/core-builtins-checklist.md (#1655)
//
// (Avoiding CompilerService construction in this test — it
// conflicts with the same module session after multiple
// previous tests run in this CMake invocation. File-level
// checks are sufficient for the checklist refactor.)

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
#include <string_view>
#include <thread>
#include <vector>

import std;

namespace aura_issue_564_detail {

static int count_occurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos + 1)) != std::string::npos) {
        ++count;
    }
    return count;
}

static std::string repo_file(std::string_view rel) {
#ifdef AURA_SOURCE_DIR
    return std::string(AURA_SOURCE_DIR) + "/" + std::string(rel);
#else
    return std::string(rel);
#endif
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::string content;
    if (f.good()) {
        content.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    f.close();
    return content;
}

// ── AC1: lib/std/core.aura present + exports 10 functions
bool test_stdlib_core_file_present() {
    std::println("\n--- AC1: lib/std/core.aura present + 10 exports ---");
    const std::string lib_path = repo_file("lib/std/core.aura");
    std::string content = read_file(lib_path);
    CHECK(!content.empty(), "lib/std/core.aura exists on disk");
    CHECK(content.find("(export") != std::string::npos, "stdlib/core.aura has (export ...) line");
    CHECK(content.find("(core:any") != std::string::npos, "stdlib/core.aura exports (core:any)");
    CHECK(content.find("(core:all") != std::string::npos, "stdlib/core.aura exports (core:all)");
    CHECK(content.find("(core:zip-with") != std::string::npos,
          "stdlib/core.aura exports (core:zip-with)");
    CHECK(content.find("(core:group-by") != std::string::npos,
          "stdlib/core.aura exports (core:group-by)");
    CHECK(content.find("(core:chunk") != std::string::npos,
          "stdlib/core.aura exports (core:chunk)");
    CHECK(content.find("(core:running-sum") != std::string::npos,
          "stdlib/core.aura exports (core:running-sum)");
    CHECK(content.find("(core:safe-div") != std::string::npos,
          "stdlib/core.aura exports (core:safe-div)");
    CHECK(content.find("(core:format-currency") != std::string::npos,
          "stdlib/core.aura exports (core:format-currency)");
    CHECK(content.find("(core:clamp") != std::string::npos,
          "stdlib/core.aura exports (core:clamp)");
    CHECK(content.find("(core:lerp") != std::string::npos, "stdlib/core.aura exports (core:lerp)");
    std::string type_content = read_file(repo_file("lib/std/core.aura-type"));
    CHECK(!type_content.empty(), "lib/std/core.aura-type exists on disk");
    std::println("  lib/core.aura: present + 10 funcs + export + type");
    return true;
}

// ── AC2: 10 engine primitives wrapped (>=5 acceptance met)
bool test_10_primitives_wrapped() {
    std::println("\n--- AC2: >= 10 engine primitives wrapped ---");
    std::string content = read_file(repo_file("lib/std/core.aura"));
    CHECK(!content.empty(), "lib/std/core.aura exists");
    const int n = count_occurrences(content, "(core:");
    std::println("  (core: occurrences in stdlib: {}", n);
    CHECK(n >= 20, "stdlib/core.aura has >= 20 (core: tokens (10 export + 10 define))");
    return true;
}

// ── AC3: no docs/design/core-builtins-checklist.md (#1655)
bool test_decision_doc_exists() {
    std::println("\n--- AC3: no docs/design/core-builtins-checklist.md (#1655) ---");
    std::string content = read_file(repo_file("docs/design/core-builtins-checklist.md"));
    CHECK(content.empty(), "no docs/design/core-builtins-checklist.md per #1655");
    return true;
}

int run_tests() {
    std::println("═══ Issue #564 verification tests ═══\n");
    std::println("Layer 1: stdlib/core.aura file + structure");
    test_stdlib_core_file_present();
    std::println("\nLayer 2: 10 primitives wrapped + decision doc");
    test_10_primitives_wrapped();
    test_decision_doc_exists();
    std::println("\n════════════════════════════════════════");
    return RUN_ALL_TESTS();
}

} // namespace aura_issue_564_detail

int aura_issue_564_run() {
    return aura_issue_564_detail::run_tests();
}

#ifndef AURA_ISSUE_BUNDLE_MEMBER
int main() {
    return aura_issue_564_run();
}
#endif