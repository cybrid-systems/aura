// @category: unit
// @reason: Issue #1785 — compile:per-symbol-dirty-stats must not call
// workspace_pool_->intern without a lock; use find_by_name under
// workspace_mtx_ shared_lock + FlatAST walks under the same lock.
//
//   AC1: source cites #1785; uses workspace_mtx_ + find_by_name
//   AC2: no unlocked intern() in the primitive body
//   AC3: known symbol returns hash with expected keys
//   AC4: unbound symbol returns sensible zeros (no pool pollution)

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
using aura::compiler::types::as_int;
using aura::compiler::types::is_hash;
using aura::compiler::types::is_int;
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

} // namespace

int main() {
    // ── AC1/AC2: source ──
    {
        std::println("\n--- AC1/AC2: locked find_by_name (no unlocked intern) ---");
        auto prim = read_first({"src/compiler/evaluator_primitives_compile_05.cpp",
                                "../src/compiler/evaluator_primitives_compile_05.cpp"});
        CHECK(!prim.empty(), "read compile_05.cpp");
        CHECK(prim.find("#1785") != std::string::npos, "cites #1785");
        auto pos = prim.find("add(\"compile:per-symbol-dirty-stats\"");
        CHECK(pos != std::string::npos, "primitive present");
        auto win = prim.substr(pos, 3500);
        CHECK(win.find("workspace_mtx_") != std::string::npos, "uses workspace_mtx_");
        CHECK(win.find("shared_lock") != std::string::npos, "shared_lock for read path");
        CHECK(win.find("find_by_name") != std::string::npos, "uses find_by_name");
        // The body must not call intern( for the symbol resolve.
        CHECK(win.find("intern(sym_name)") == std::string::npos, "no intern(sym_name)");
        CHECK(win.find("->intern(") == std::string::npos, "no ->intern( in primitive window");
    }

    // ── AC3: known symbol ──
    {
        std::println("\n--- AC3: known symbol returns hash ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define foo 1)\")").has_value(), "set-code");
        (void)cs.eval("(eval-current)");
        auto r = cs.eval("(compile:per-symbol-dirty-stats \"foo\")");
        CHECK(r.has_value() && is_hash(*r), "returns hash for known sym");
        auto n = cs.eval(
            "(hash-ref (compile:per-symbol-dirty-stats \"foo\") \"per-symbol-affected-count\")");
        CHECK(n && is_int(*n) && as_int(*n) >= 0, "per-symbol-affected-count >= 0");
        auto lk = cs.eval("(hash-ref (compile:per-symbol-dirty-stats \"foo\") \"lookup-count\")");
        CHECK(lk && is_int(*lk) && as_int(*lk) >= 1, "lookup-count bumped");
    }

    // ── AC4: unbound ──
    {
        std::println("\n--- AC4: unbound symbol zeroed stats ---");
        CompilerService cs;
        CHECK(cs.eval("(set-code \"(define x 0)\")").has_value(), "set-code");
        auto r = cs.eval("(compile:per-symbol-dirty-stats \"__not_a_real_symbol_1785__\")");
        CHECK(r.has_value() && is_hash(*r), "unbound returns hash");
        auto n =
            cs.eval("(hash-ref (compile:per-symbol-dirty-stats \"__not_a_real_symbol_1785__\") "
                    "\"per-symbol-affected-count\")");
        CHECK(n && is_int(*n) && as_int(*n) == 0, "unbound affected-count 0");
    }

    std::println("\n=== test_per_symbol_dirty_pool_lock_1785: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
