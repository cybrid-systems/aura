// @category: unit
// @reason: Issue #1861 — Env::bind_with_linear_state is single-writer
// (no env_mtx_); StringPool::intern is not thread-safe. Concurrent
// bind vs lookup on the same Env is unsupported; sequential bind+lookup
// with a shared pool must keep string/SymId paths consistent.
//
//   AC1: Env / StringPool / bind_with_linear_state document #1861
//   AC2: sequential bind_with_linear_state + pool → lookup + lookup_by_symid
//   AC3: binding_index_ sees last binding after multi-name binds

#include "test_harness.hpp"

#include <fstream>
#include <print>
#include <string>
#include <string_view>

import std;
import aura.core;
import aura.compiler.evaluator;
import aura.compiler.value;

namespace {

using aura::ast::StringPool;
using aura::compiler::Env;
using aura::compiler::types::as_int;
using aura::compiler::types::is_int;
using aura::compiler::types::make_int;
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
    // ── AC1: source contract ──
    {
        std::println("\n--- AC1: #1861 single-writer / intern docs ---");
        auto env_ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        auto env_cpp =
            read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
        auto ast_ixx = read_first({"src/core/ast.ixx", "../src/core/ast.ixx"});
        CHECK(!env_ixx.empty(), "read evaluator.ixx");
        CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
        CHECK(!ast_ixx.empty(), "read ast.ixx");
        CHECK(env_ixx.find("#1861") != std::string::npos, "Env cites #1861");
        CHECK(env_ixx.find("single-writer") != std::string::npos, "documents single-writer");
        CHECK(env_cpp.find("#1861") != std::string::npos, "bind_with_linear_state cites #1861");
        auto bpos = env_cpp.find("Env::bind_with_linear_state");
        CHECK(bpos != std::string::npos, "bind_with_linear_state present");
        // Comment block sits just before the definition.
        auto win = env_cpp.substr(bpos > 400 ? bpos - 400 : 0, 900);
        CHECK(win.find("single-writer") != std::string::npos ||
                  win.find("Not atomic") != std::string::npos ||
                  win.find("not locked") != std::string::npos,
              "bind path documents no lock / single-writer");
        CHECK(win.find("not thread-safe") != std::string::npos ||
                  win.find("Not thread-safe") != std::string::npos ||
                  env_cpp.find("StringPool::intern") != std::string::npos,
              "documents intern not thread-safe");
        // Must NOT claim intern is "safe on shared pool" without serialize.
        CHECK(env_cpp.find("safe on shared pool") == std::string::npos,
              "removed misleading 'safe on shared pool' claim");
        CHECK(ast_ixx.find("#1861") != std::string::npos, "StringPool cites #1861");
        CHECK(ast_ixx.find("Not thread-safe") != std::string::npos ||
                  ast_ixx.find("not thread-safe") != std::string::npos,
              "StringPool documents intern not TS");
    }

    // ── AC2: sequential bind + pool → dual lookup consistent ──
    {
        std::println("\n--- AC2: sequential bind_with_linear_state dual path ---");
        StringPool pool;
        Env env;
        env.set_pool(&pool);
        env.bind_with_linear_state("x", make_int(42), /*state=*/0);
        env.bind_with_linear_state("y", make_int(7), /*state=*/0);

        auto vx = env.lookup("x");
        CHECK(vx && is_int(*vx) && as_int(*vx) == 42, "lookup x via string");
        auto vy = env.lookup("y");
        CHECK(vy && is_int(*vy) && as_int(*vy) == 7, "lookup y via string");

        auto sx = pool.find_by_name("x");
        auto sy = pool.find_by_name("y");
        CHECK(sx.has_value() && sy.has_value(), "pool has x and y");
        if (sx && sy) {
            auto vsx = env.lookup_by_symid(*sx);
            auto vsy = env.lookup_by_symid(*sy);
            CHECK(vsx && is_int(*vsx) && as_int(*vsx) == 42, "lookup_by_symid x");
            CHECK(vsy && is_int(*vsy) && as_int(*vsy) == 7, "lookup_by_symid y");
        }
        // Re-bind same name (shadow / overwrite index) still consistent.
        env.bind_with_linear_state("x", make_int(99), /*state=*/0);
        auto vx2 = env.lookup("x");
        CHECK(vx2 && is_int(*vx2) && as_int(*vx2) == 99, "rebinding x updates string lookup");
        if (sx) {
            // Most-recent-wins on symid path (rbegin).
            auto vsx2 = env.lookup_by_symid(*sx);
            CHECK(vsx2 && is_int(*vsx2) && as_int(*vsx2) == 99, "rebinding x updates symid lookup");
        }
    }

    // ── AC3: binding_index_ tracks multi-name binds ──
    {
        std::println("\n--- AC3: multi-name binds remain look-uppable ---");
        Env env;
        for (int i = 0; i < 32; ++i) {
            auto name = std::string("v") + std::to_string(i);
            env.bind(name, make_int(i));
        }
        for (int i = 0; i < 32; ++i) {
            auto name = std::string("v") + std::to_string(i);
            auto v = env.lookup(name);
            CHECK(v && is_int(*v) && as_int(*v) == i,
                  (std::string("lookup ") + name + " == " + std::to_string(i)).c_str());
        }
        // Missing name still nullopt.
        CHECK(!env.lookup("nope").has_value(), "missing → nullopt");
    }

    std::println("\n=== test_env_bind_linear_race_1861: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
