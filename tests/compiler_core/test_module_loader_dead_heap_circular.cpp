// @category: unit
// @reason: Issue #1692 — load_module_file circular-dep path must not
// Issue #1488/#1692 (#1978 renamed): issue# moved from filename to header.
// intern an unused error string into string_heap_ (dead push; #1488 class).
//
//   AC1: source has no eidx + push_back on circular path
//   AC2: audit_dead_heap_push.py reports 0 candidates
//   AC3: circular module load does not intern "circular dependency: ..." strings

#include "test_harness.hpp"

#include <cstdlib>
#include <fstream>
#include <print>
#include <sstream>
#include <string>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

static std::string read_file(const char* path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& body) {
    std::ofstream out(path);
    if (!out)
        return false;
    out << body;
    return static_cast<bool>(out);
}

} // namespace

int main() {
    // ── AC1: source-level contract ──
    {
        std::println("\n--- AC1: no dead circular-dep heap push in module_loader ---");
        auto src = read_file("src/compiler/evaluator_module_loader.cpp");
        CHECK(!src.empty(), "read evaluator_module_loader.cpp");
        auto pos = src.find("loading_stack_.count(resolved)");
        CHECK(pos != std::string::npos, "locate circular-dep check");
        if (pos != std::string::npos) {
            auto window = src.substr(pos, 500);
            // Code must not reintroduce the dead-push couplet (comment may
            // still mention string_heap_.push_back as "do NOT …").
            CHECK(window.find("auto eidx") == std::string::npos, "no eidx capture");
            CHECK(window.find("string_heap_.push_back(\"circular") == std::string::npos &&
                      window.find("string_heap_.push_back(\"circular dependency") ==
                          std::string::npos &&
                      window.find("push_back(\"circular dependency: \" + resolved)") ==
                          std::string::npos,
                  "no circular-dep string_heap_ push");
            CHECK(window.find("make_void()") != std::string::npos, "still returns make_void()");
            CHECK(window.find("circular dependency") != std::string::npos,
                  "stderr message retained");
        }
    }

    // ── AC2: repo-wide audit clean ──
    {
        std::println("\n--- AC2: audit_dead_heap_push clean ---");
        int rc = std::system("python3 scripts/audit_dead_heap_push.py >/tmp/audit_1692.out 2>&1");
        CHECK(rc == 0, "audit_dead_heap_push exit 0");
        auto out = read_file("/tmp/audit_1692.out");
        CHECK(out.find("clean") != std::string::npos ||
                  out.find("0 candidates") != std::string::npos,
              "audit reports clean / 0 candidates");
    }

    // ── AC3: circular load does not intern circular-dep error strings ──
    {
        std::println("\n--- AC3: circular load heap ---");
        // Absolute paths so import resolves without AURA_PATH.
        const std::string path_a = "/tmp/aura_circ_a_1692.aura";
        const std::string path_b = "/tmp/aura_circ_b_1692.aura";
        CHECK(write_file(path_a, std::string("(import \"") + path_b + "\")\n"), "write a");
        CHECK(write_file(path_b, std::string("(import \"") + path_a + "\")\n"), "write b");

        CompilerService cs;
        const auto heap0 = cs.evaluator().string_heap().size();
        // Loading A should hit circular when B imports A (or fail soft with void).
        auto r = cs.evaluator().load_module_file(path_a);
        (void)r;
        const auto heap1 = cs.evaluator().string_heap().size();
        auto heap = cs.evaluator().string_heap();
        bool found_circ_msg = false;
        const std::string needle_prefix = "circular dependency: ";
        for (std::size_t i = heap0; i < heap1 && i < heap.size(); ++i) {
            if (heap[i].starts_with(needle_prefix) ||
                heap[i].find("circular dependency") != std::string::npos)
                found_circ_msg = true;
        }
        std::println("  heap before={} after={} delta={} found_circ_msg={}", heap0, heap1,
                     heap1 - heap0, found_circ_msg);
        CHECK(!found_circ_msg, "no circular-dependency string interned into string_heap_");
        // Modest growth bound (import may intern paths for other reasons).
        CHECK(heap1 - heap0 <= 16, "circular load heap growth bounded");
    }

    std::println("\n=== test_module_loader_dead_heap_circular_1692: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
