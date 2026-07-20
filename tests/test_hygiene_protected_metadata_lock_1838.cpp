// @category: unit
// @reason: Issue #1838 — hygiene:protected? must hold metadata
// reader lock when reading is_macro_introduced (same race class
// as #1783 syntax-marker vs set-marker).
//
//   AC1: source cites #1838; try_acquire_metadata_reader_lock
//   AC2: hygiene:protected? returns bool on valid/invalid args
//   AC3: set-marker + protected? under sequential use stays consistent

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.compiler.service;
import aura.compiler.value;

namespace {

using aura::compiler::CompilerService;
using aura::compiler::types::as_bool;
using aura::compiler::types::as_int;
using aura::compiler::types::is_bool;
using aura::compiler::types::is_int;
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
        std::println("\n--- AC1: hygiene:protected? takes metadata reader lock ---");
        std::string src;
        for (const char* p : {"src/compiler/evaluator_primitives_compile.cpp",
                              "../src/compiler/evaluator_primitives_compile.cpp"}) {
            src = read_file(p);
            if (!src.empty())
                break;
        }
        CHECK(!src.empty(), "read compile_07.cpp");
        CHECK(src.find("#1838") != std::string::npos, "cites #1838");
        auto pos = src.find("\"hygiene:protected?\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = src.substr(pos, 800);
        CHECK(win.find("try_acquire_metadata_reader_lock") != std::string::npos,
              "uses metadata reader lock");
        CHECK(win.find("is_macro_introduced") != std::string::npos, "reads is_macro_introduced");
    }

    // ── AC2: runtime shape ──
    {
        std::println("\n--- AC2: hygiene:protected? returns bool ---");
        CompilerService cs;
        CHECK(cs.eval("(define x 1)").has_value(), "seed");
        auto bad = cs.eval("(hygiene:protected?)");
        CHECK(bad && is_bool(*bad) && !as_bool(*bad), "no-arg → #f");
        auto oob = cs.eval("(hygiene:protected? 999999)");
        CHECK(oob && is_bool(*oob) && !as_bool(*oob), "OOB → #f");
        auto z = cs.eval("(hygiene:protected? 0)");
        CHECK(z && is_bool(*z), "valid id returns bool");
    }

    // ── AC3: set-marker then protected? ──
    {
        std::println("\n--- AC3: set-marker + protected? sequential ---");
        CompilerService cs;
        // Materialize workspace AST (same pattern as #1783 / #366).
        CHECK(cs.eval("(set-code \"(define y 2)\")").has_value(), "set-code");
        auto rid = cs.eval("(car (query:find \"y\"))");
        CHECK(rid && is_int(*rid), "find y");
        const auto id = as_int(*rid);
        // Marker 1 = MacroIntroduced.
        auto set = cs.eval(std::format("(syntax:set-marker {} 1)", id));
        CHECK(set && is_bool(*set) && as_bool(*set), "set-marker ok");
        auto prot = cs.eval(std::format("(hygiene:protected? {})", id));
        CHECK(prot && is_bool(*prot) && as_bool(*prot), "node protected after MacroIntroduced");
        auto clear = cs.eval(std::format("(syntax:set-marker {} 0)", id));
        CHECK(clear && is_bool(*clear) && as_bool(*clear), "clear marker ok");
        auto unprot = cs.eval(std::format("(hygiene:protected? {})", id));
        CHECK(unprot && is_bool(*unprot) && !as_bool(*unprot), "node not protected after User");
    }

    std::println("\n=== test_hygiene_protected_metadata_lock_1838: {} passed, {} failed ===",
                 g_passed, g_failed);
    return g_failed ? 1 : 0;
}
