// @category: unit
// @reason: Issue #1839 — compiler_service_ is a documented non-owning
// back-pointer to the owning CompilerService (wired once via
// set_compiler_service(this)). Concurrent rebind/free while
// compile:ir-stats runs is unsupported (#1835 / #1837 sibling).
//
//   AC1: evaluator.ixx cites #1839 ownership contract
//   AC2: CompilerService wires compiler_service_ to this
//   AC3: compile:ir-stats remains callable (void or hash)

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::Evaluator;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_void;
using aura::test::g_failed;
using aura::test::g_passed;

std::string read_file(const char* path) {
    std::ifstream in(path);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    // ── AC1: source ──
    {
        std::println("\n--- AC1: ownership contract documented ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read evaluator.ixx");
        CHECK(src.find("#1839") != std::string::npos, "cites #1839");
        CHECK(src.find("set_compiler_service") != std::string::npos, "setter present");
        CHECK(src.find("non-owning") != std::string::npos, "documents non-owning");
        CHECK(src.find("compile:ir-stats") != std::string::npos ||
                  src.find("CompilerService") != std::string::npos,
              "mentions service / ir-stats consumers");
    }

    // ── AC2: service wires back-pointer ──
    {
        std::println("\n--- AC2: CompilerService wires compiler_service_ ---");
        CompilerService cs;
        CHECK(cs.evaluator().compiler_service() != nullptr, "service pointer set");
        CHECK(cs.evaluator().compiler_service() == static_cast<void*>(&cs), "back-pointer is &cs");
        // Still stable after a normal eval (no rebind).
        CHECK(cs.eval("(+ 1 1)").has_value(), "eval ok");
        CHECK(cs.evaluator().compiler_service() == static_cast<void*>(&cs),
              "pointer stable after eval");
    }

    // ── AC3: ir-stats callable ──
    {
        std::println("\n--- AC3: compile:ir-stats callable ---");
        CompilerService cs;
        // Fresh service: no module compiled → void is OK; hash after compile also OK.
        auto r = cs.eval("(engine:metrics \"compile:ir-stats\")");
        if (!r)
            r = cs.eval("(compile:ir-stats)");
        CHECK(r.has_value(), "ir-stats returns");
        CHECK(is_void(*r) || is_hash(*r), "void (no module) or hash");
    }

    std::println("\n=== test_compiler_service_ownership_1839: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
