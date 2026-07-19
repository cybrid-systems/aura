// @category: unit
// @reason: Issue #1862 — Env::lookup_by_symid walks bindings_symid_
// without a shared_lock; concurrent bind_symid* is unsupported under
// the #1861 single-writer contract (no env_symid_mtx_ on hot path).
//
//   AC1: source cites #1862 on lookup_by_symid; no env_symid_mtx_
//   AC2: sequential bind_symid then lookup_by_symid (local + parent)
//   AC3: most-recent-wins after re-bind same SymId; missing → nullopt

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
        std::println("\n--- AC1: #1862 lookup_by_symid single-writer docs ---");
        auto env_cpp =
            read_first({"src/compiler/evaluator_env.cpp", "../src/compiler/evaluator_env.cpp"});
        auto env_ixx = read_first({"src/compiler/evaluator.ixx", "../src/compiler/evaluator.ixx"});
        CHECK(!env_cpp.empty(), "read evaluator_env.cpp");
        CHECK(!env_ixx.empty(), "read evaluator.ixx");
        CHECK(env_cpp.find("#1862") != std::string::npos, "cites #1862");
        auto pos = env_cpp.find("Env::lookup_by_symid");
        CHECK(pos != std::string::npos, "lookup_by_symid present");
        // Comment block sits just before the definition.
        auto win = env_cpp.substr(pos > 500 ? pos - 500 : 0, 1100);
        CHECK(win.find("#1862") != std::string::npos, "lookup_by_symid window cites #1862");
        CHECK(win.find("single-writer") != std::string::npos ||
                  win.find("#1861") != std::string::npos,
              "ties to single-writer / #1861");
        CHECK(win.find("env_symid_mtx_") != std::string::npos ||
                  win.find("no shared_lock") != std::string::npos ||
                  win.find("no env_symid") != std::string::npos,
              "documents no env_symid_mtx_ / no shared_lock");
        // Contract may mention env_symid_mtx_ by name; must not declare one.
        CHECK(env_ixx.find("env_symid_mtx_{") == std::string::npos &&
                  env_ixx.find("env_symid_mtx_;") == std::string::npos &&
                  env_ixx.find("mutex env_symid") == std::string::npos,
              "no env_symid_mtx_ member declaration");
        CHECK(env_ixx.find("#1862") != std::string::npos, "Env class cites #1862 sibling");
        // Writer side documented.
        auto bpos = env_cpp.find("Env::bind_symid_with_linear_state");
        CHECK(bpos != std::string::npos, "bind_symid_with_linear_state present");
        auto bwin = env_cpp.substr(bpos > 350 ? bpos - 350 : 0, 700);
        CHECK(bwin.find("#1862") != std::string::npos || bwin.find("#1861") != std::string::npos,
              "bind_symid path cites race sibling");
    }

    // ── AC2: sequential bind_symid + lookup_by_symid ──
    {
        std::println("\n--- AC2: sequential bind_symid then lookup_by_symid ---");
        StringPool pool;
        auto sx = pool.intern("x");
        auto sy = pool.intern("y");
        Env root;
        root.set_pool(&pool);
        root.bind_symid(sx, make_int(10));
        root.bind_symid(sy, make_int(20));

        auto vx = root.lookup_by_symid(sx);
        auto vy = root.lookup_by_symid(sy);
        CHECK(vx && is_int(*vx) && as_int(*vx) == 10, "local lookup_by_symid x");
        CHECK(vy && is_int(*vy) && as_int(*vy) == 20, "local lookup_by_symid y");

        // String mirror when pool set.
        auto vsx = root.lookup("x");
        CHECK(vsx && is_int(*vsx) && as_int(*vsx) == 10, "string mirror x after bind_symid");

        // Parent chain.
        Env leaf;
        leaf.set_parent(&root);
        auto vxp = leaf.lookup_by_symid(sx);
        CHECK(vxp && is_int(*vxp) && as_int(*vxp) == 10, "parent chain lookup_by_symid x");
    }

    // ── AC3: rebind most-recent-wins + miss ──
    {
        std::println("\n--- AC3: rebind most-recent-wins; missing nullopt ---");
        StringPool pool;
        auto s = pool.intern("shadow");
        Env env;
        env.set_pool(&pool);
        env.bind_symid(s, make_int(1));
        env.bind_symid(s, make_int(2));
        auto v = env.lookup_by_symid(s);
        CHECK(v && is_int(*v) && as_int(*v) == 2, "most-recent-wins after re-bind_symid");
        auto miss = env.lookup_by_symid(pool.intern("nope"));
        CHECK(!miss.has_value(), "missing SymId → nullopt");
        // Force many binds (vector growth) then lookup — sequential only.
        for (int i = 0; i < 64; ++i) {
            auto name = std::string("n") + std::to_string(i);
            env.bind_symid(pool.intern(name), make_int(i));
        }
        auto last = env.lookup_by_symid(pool.intern("n63"));
        CHECK(last && is_int(*last) && as_int(*last) == 63, "post-growth lookup still correct");
    }

    std::println("\n=== test_env_lookup_by_symid_race_1862: {} passed, {} failed ===", g_passed,
                 g_failed);
    return g_failed ? 1 : 0;
}
